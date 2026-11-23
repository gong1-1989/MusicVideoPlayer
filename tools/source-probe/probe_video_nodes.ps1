# 对 docs/source-probe/video-nodes.json 中的候选节点做真实 HTTP 验证。
# 只记录状态码、响应格式、MacCMS 字段是否存在、条目数、耗时，不保存影片标题和播放地址。
# 用法：pwsh -File tools/source-probe/probe_video_nodes.ps1 [-Rounds 1,2,3]

param(
    [int[]]$Rounds = @(),
    [int]$TimeoutSeconds = 8,
    [int]$Parallel = 16,
    [string]$InputFile = '',
    [string]$OutputFile = ''
)

$ErrorActionPreference = 'Stop'
if (-not $InputFile) { $InputFile = Join-Path $PSScriptRoot '..\..\docs\source-probe\video-nodes.json' }
if (-not $OutputFile) { $OutputFile = Join-Path $PSScriptRoot '..\..\docs\source-probe\video-probe-results.json' }

$dataset = Get-Content -LiteralPath $InputFile -Raw -Encoding utf8 | ConvertFrom-Json
$nodes = @($dataset.nodes)
if ($Rounds.Count -gt 0) {
    $nodes = @($nodes | Where-Object { $Rounds -contains [int]$_.round })
}
if ($nodes.Count -eq 0) { throw '没有待验证节点' }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

public sealed class ProbeRow
{
    public int Round { get; set; }
    public string Source { get; set; }
    public string Name { get; set; }
    public string BaseUrl { get; set; }
    public string Kind { get; set; }
    public string WorkingQuery { get; set; }
    public int Status { get; set; }
    public string FinalHost { get; set; }
    public string ContentType { get; set; }
    public string Format { get; set; }
    public int ListCount { get; set; }
    public bool HasVodName { get; set; }
    public bool HasVodPlayUrl { get; set; }
    public bool HasVodPlayFrom { get; set; }
    public bool Ok { get; set; }
    public bool SearchOk { get; set; }
    public bool SearchCompared { get; set; }
    public int SecondListCount { get; set; }
    public bool SameTitles { get; set; }
    public long ElapsedMs { get; set; }
    public int Bytes { get; set; }
    public string Error { get; set; }
}

public static class VideoProbe
{
    private static readonly string[] Variants = new[]
    {
        "?ac=detail&wd=%E7%94%B5%E5%BD%B1",
        "?ac=list&wd=%E7%94%B5%E5%BD%B1",
        "?ac=videolist&wd=%E7%94%B5%E5%BD%B1",
        "?ac=detail"
    };

    public static ProbeRow[] Run(string[] baseUrls, string[] kinds, int[] rounds,
        string[] sources, string[] names, int timeoutSeconds, int parallel)
    {
        var handler = new SocketsHttpHandler
        {
            AllowAutoRedirect = true,
            MaxAutomaticRedirections = 6,
            AutomaticDecompression = DecompressionMethods.All,
            ConnectTimeout = TimeSpan.FromSeconds(timeoutSeconds),
            PooledConnectionLifetime = TimeSpan.FromMinutes(2),
            SslOptions = { RemoteCertificateValidationCallback = delegate { return true; } }
        };
        using var client = new HttpClient(handler)
        {
            Timeout = TimeSpan.FromSeconds(timeoutSeconds + 4),
            MaxResponseContentBufferSize = 4 * 1024 * 1024
        };
        client.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/124.0 Safari/537.36");
        client.DefaultRequestHeaders.TryAddWithoutValidation("Accept",
            "application/json,text/xml,application/xml,text/html;q=0.9,*/*;q=0.8");

        var rows = new ProbeRow[baseUrls.Length];
        Parallel.For(0, baseUrls.Length, new ParallelOptions { MaxDegreeOfParallelism = parallel }, i =>
        {
            rows[i] = ProbeOne(client, baseUrls[i], kinds[i], timeoutSeconds);
            rows[i].Round = rounds[i];
            rows[i].Source = sources[i];
            rows[i].Name = names[i];
            rows[i].BaseUrl = baseUrls[i];
            rows[i].Kind = kinds[i];
        });
        return rows;
    }

    private static ProbeRow ProbeOne(HttpClient client, string baseUrl, string kind, int timeoutSeconds)
    {
        var variants = new List<string>();
        if (kind == "web" || kind == "parser")
        {
            variants.Add("");
        }
        else
        {
            variants.AddRange(Variants);
        }

        ProbeRow best = null;
        foreach (var variant in variants)
        {
            var url = baseUrl.TrimEnd('?') + variant;
            var attempt = new ProbeRow { Format = "none", WorkingQuery = variant };
            var sw = Stopwatch.StartNew();
            try
            {
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(timeoutSeconds));
                using var request = new HttpRequestMessage(HttpMethod.Get, url);
                using var response = client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cts.Token)
                    .GetAwaiter().GetResult();
                attempt.Status = (int)response.StatusCode;
                attempt.FinalHost = response.RequestMessage?.RequestUri?.Host ?? "";
                attempt.ContentType = response.Content.Headers.ContentType?.MediaType ?? "";
                var bodyTask = response.Content.ReadAsStringAsync(cts.Token);
                bodyTask.GetAwaiter().GetResult();
                var body = bodyTask.Result ?? "";
                attempt.Bytes = Encoding.UTF8.GetByteCount(body);
                attempt.ElapsedMs = sw.ElapsedMilliseconds;
                attempt.Format = DetectFormat(body);
                attempt.Error = "";

                if (attempt.Format == "json")
                {
                    InspectJson(body, attempt);
                    if (attempt.HasVodName && attempt.HasVodPlayUrl)
                    {
                        attempt.Ok = true;
                        // 只用一次“带 wd 的地址返回 200”不能证明搜索词生效：
                        // 很多 MacCMS 会忽略 wd，返回同一份最新列表。
                        // 必须换第二个关键词再取一次，标题集合完全相同就是 catalog-only。
                        attempt.SearchOk = ValidateSearch(client, url, variant, body, timeoutSeconds, attempt);
                        if (!attempt.SearchOk && string.IsNullOrEmpty(attempt.Error))
                        {
                            attempt.Error = attempt.SearchCompared
                                ? "两次关键词标题集合完全相同，接口忽略 wd"
                                : "仅支持无关键词列目录，不支持 wd 搜索";
                        }
                        return attempt;
                    }
                }
                else if (attempt.Format == "xml" && attempt.Status >= 200 && attempt.Status < 300)
                {
                    attempt.Error = "XML 接口（当前探查器只解析 JSON）";
                }
            }
            catch (Exception ex)
            {
                attempt.Status = 0;
                attempt.Format = "error";
                attempt.ElapsedMs = sw.ElapsedMilliseconds;
                attempt.Error = ShortError(ex);
            }

            if (best == null || Rank(attempt) > Rank(best))
            {
                best = attempt;
            }
        }
        return best ?? new ProbeRow { Format = "error", Error = "没有可尝试的地址" };
    }

    private static bool ValidateSearch(HttpClient client, string url, string variant,
        string firstBody, int timeoutSeconds, ProbeRow row)
    {
        if (!variant.Contains("wd=")) return false;

        var secondUrl = url
            .Replace("wd=" + Uri.EscapeDataString("电影"), "wd=" + Uri.EscapeDataString("电视剧"))
            .Replace("wd=%E7%94%B5%E5%BD%B1", "wd=%E7%94%B5%E7%BB%BC%E5%89%A7");
        int status;
        string error;
        var secondBody = FetchBody(client, secondUrl, timeoutSeconds, out status, out error);
        if (status < 200 || status >= 300 || string.IsNullOrEmpty(secondBody) || DetectFormat(secondBody) != "json")
        {
            row.Error = "二次关键词请求失败：" + (error.Length > 80 ? error.Substring(0, 80) : error);
            return false;
        }

        var firstNames = VodNames(firstBody);
        var secondNames = VodNames(secondBody);
        row.SearchCompared = true;
        row.SecondListCount = secondNames.Count;
        row.SameTitles = SameNames(firstNames, secondNames);
        return !row.SameTitles && firstNames.Count > 0;
    }

    private static string FetchBody(HttpClient client, string url, int timeoutSeconds,
        out int status, out string error)
    {
        status = 0;
        error = "";
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(timeoutSeconds));
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            using var response = client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cts.Token)
                .GetAwaiter().GetResult();
            status = (int)response.StatusCode;
            return response.Content.ReadAsStringAsync(cts.Token).GetAwaiter().GetResult() ?? "";
        }
        catch (Exception ex)
        {
            error = ShortError(ex);
            return "";
        }
    }

    private static List<string> VodNames(string body)
    {
        var names = new List<string>();
        try
        {
            using var doc = JsonDocument.Parse(body);
            JsonElement list;
            var root = doc.RootElement;
            if (root.ValueKind == JsonValueKind.Object
                && root.TryGetProperty("list", out list)
                && list.ValueKind == JsonValueKind.Array)
            {
                foreach (var item in list.EnumerateArray())
                    AddVodName(item, names);
            }
            else if (root.ValueKind == JsonValueKind.Array)
            {
                foreach (var item in root.EnumerateArray())
                    AddVodName(item, names);
            }
        }
        catch
        {
        }
        return names;
    }

    private static void AddVodName(JsonElement item, List<string> names)
    {
        if (item.ValueKind != JsonValueKind.Object) return;
        JsonElement name;
        if (item.TryGetProperty("vod_name", out name) && name.ValueKind == JsonValueKind.String)
            names.Add(name.GetString() ?? "");
    }

    private static bool SameNames(List<string> first, List<string> second)
    {
        if (first.Count != second.Count) return false;
        if (first.Count == 0) return true;
        var set = new HashSet<string>(second);
        foreach (var name in first)
            if (!set.Contains(name)) return false;
        return true;
    }

    private static int Rank(ProbeRow row)
    {
        if (row.Ok) return 10;
        if (row.Format == "json" && row.ListCount > 0) return 8;
        if (row.Format == "json") return 7;
        if (row.Format == "xml") return 6;
        if (row.Format == "html" && row.Status >= 200 && row.Status < 400) return 4;
        if (row.Format == "text" && row.Status >= 200 && row.Status < 300) return 3;
        if (row.Status == 404) return 1;
        return 0;
    }


    private static string DetectFormat(string body)
    {
        var text = body.TrimStart();
        if (text.Length == 0) return "empty";
        if (text[0] == '{' || text[0] == '[') return "json";
        if (text.StartsWith("<?xml", StringComparison.OrdinalIgnoreCase)
            || text.StartsWith("<rss", StringComparison.OrdinalIgnoreCase)
            || text.StartsWith("<list", StringComparison.OrdinalIgnoreCase))
            return "xml";
        if (text.StartsWith("<!doctype", StringComparison.OrdinalIgnoreCase)
            || text.StartsWith("<html", StringComparison.OrdinalIgnoreCase))
            return "html";
        return "text";
    }

    private static void InspectJson(string body, ProbeRow row)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            var root = doc.RootElement;
            JsonElement list;
            if (root.ValueKind == JsonValueKind.Object && root.TryGetProperty("list", out list)
                && list.ValueKind == JsonValueKind.Array)
            {
                row.ListCount = list.GetArrayLength();
                if (row.ListCount > 0 && list[0].ValueKind == JsonValueKind.Object)
                {
                    var item = list[0];
                    row.HasVodName = item.TryGetProperty("vod_name", out _);
                    row.HasVodPlayUrl = item.TryGetProperty("vod_play_url", out _);
                    row.HasVodPlayFrom = item.TryGetProperty("vod_play_from", out _);
                }
                return;
            }
            if (root.ValueKind == JsonValueKind.Array)
            {
                row.ListCount = root.GetArrayLength();
                if (row.ListCount > 0 && root[0].ValueKind == JsonValueKind.Object)
                {
                    var item = root[0];
                    row.HasVodName = item.TryGetProperty("vod_name", out _);
                    row.HasVodPlayUrl = item.TryGetProperty("vod_play_url", out _);
                    row.HasVodPlayFrom = item.TryGetProperty("vod_play_from", out _);
                }
            }
        }
        catch (Exception ex)
        {
            row.Error = "JSON 解析失败：" + ex.Message;
        }
    }

    private static string ShortError(Exception ex)
    {
        var inner = ex;
        while (inner.InnerException != null) inner = inner.InnerException;
        var text = inner.Message ?? inner.GetType().Name;
        return text.Length > 160 ? text.Substring(0, 160) : text;
    }
}
'@

$rows = [VideoProbe]::Run(
    @($nodes.baseUrl),
    @($nodes.kind),
    @($nodes | ForEach-Object { [int]$_.round }),
    @($nodes.source),
    @($nodes.name),
    $TimeoutSeconds,
    $Parallel
)

$result = [pscustomobject]@{
    generatedAt = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ssK')
    inputFile = $InputFile
    timeoutSeconds = $TimeoutSeconds
    parallel = $Parallel
    note = '仅记录 HTTP 状态、响应格式和 MacCMS 字段是否存在，不保存影片标题与播放地址；每个候选都用“电影/电视剧”两次关键词比对标题集合。'
    stats = $null
    rows = $rows
}
$roundStats = $rows | Group-Object Round | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        round = [int]$_.Name
        nodes = $g.Count
        catalogOk = @($g | Where-Object Ok).Count
        searchOk = @($g | Where-Object SearchOk).Count
        catalogOnly = @($g | Where-Object { $_.Ok -and -not $_.SearchOk }).Count
        jsonNoStream = @($g | Where-Object { $_.Format -eq 'json' -and -not $_.Ok }).Count
        xml = @($g | Where-Object Format -eq 'xml').Count
        html = @($g | Where-Object Format -eq 'html').Count
        dead = @($g | Where-Object { $_.Status -eq 0 -or $_.Status -ge 400 }).Count
    }
} | Sort-Object round
$result.stats = [pscustomobject]@{
    total = $rows.Count
    catalogOk = @($rows | Where-Object Ok).Count
    searchOk = @($rows | Where-Object SearchOk).Count
    catalogOnly = @($rows | Where-Object { $_.Ok -and -not $_.SearchOk }).Count
    searchCompared = @($rows | Where-Object SearchCompared).Count
    byRound = $roundStats
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutputFile -Encoding utf8

"验证完成：$($rows.Count) 个节点，可列目录 $($result.stats.catalogOk) 个，支持关键词搜索 $($result.stats.searchOk) 个"
$roundStats | Format-Table -AutoSize
