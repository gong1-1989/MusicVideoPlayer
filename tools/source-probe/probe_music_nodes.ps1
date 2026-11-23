# 对音乐候选节点做真实 HTTP 验证。
# 只记录状态码、JSON 结构、字段名、数组路径和搜索是否随关键词变化；
# 不落盘歌曲标题、播放地址、封面地址或 demo 密码。
# 用法：pwsh -File tools/source-probe/probe_music_nodes.ps1 -Rounds 1

param(
    [string]$Rounds = '1,2,3,4,5,6,7,8,9,10',
    [string]$Keyword = 'love',
    [string]$SecondKeyword = 'jazz',
    [int]$Limit = 5,
    [int]$Throttle = 16,
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Join-Path $PSScriptRoot '..\..'
$nodesFile = Join-Path $root 'docs\source-probe\music-nodes.json'
if ([string]::IsNullOrWhiteSpace($OutFile)) {
    $OutFile = Join-Path $root 'docs\source-probe\music-probe-results.json'
}

$doc = Get-Content -LiteralPath $nodesFile -Raw -Encoding utf8 | ConvertFrom-Json
$allNodes = @($doc.nodes)
$roundFilter = @($Rounds -split ',' | ForEach-Object { [int]$_.Trim() })
$selected = @($allNodes | Where-Object { $roundFilter -contains [int]$_.round })
if ($selected.Count -eq 0) { throw '没有选中任何节点' }

$script:KeywordValue = $Keyword
$script:SecondKeywordValue = $SecondKeyword
$script:LimitValue = $Limit

$results = $selected | ForEach-Object -Parallel {
    $node = $_
    $keyword = $using:KeywordValue
    $secondKeyword = $using:SecondKeywordValue
    $limit = $using:LimitValue

    function Get-JsonFields {
        param($Object, [string]$Prefix = '', [int]$Depth = 0)
        $map = @{}
        if ($null -eq $Object -or $Depth -gt 3) { return $map }
        if ($Object -is [System.Management.Automation.PSCustomObject]) {
            foreach ($prop in $Object.PSObject.Properties) {
                $name = if ($Prefix) { "$Prefix.$($prop.Name)" } else { $prop.Name }
                $value = $prop.Value
                if ($null -eq $value) { continue }
                if ($value -is [string] -or $value -is [int] -or $value -is [long] -or $value -is [double] -or $value -is [bool]) {
                    $map[$name] = [string]$value
                } elseif ($value -is [System.Management.Automation.PSCustomObject]) {
                    $child = Get-JsonFields -Object $value -Prefix $name -Depth ($Depth + 1)
                    foreach ($k in $child.Keys) { $map[$k] = $child[$k] }
                } elseif ($value -is [System.Collections.IEnumerable] -and -not ($value -is [string])) {
                    $arr = @($value)
                    if ($arr.Count -gt 0) {
                        if ($arr[0] -is [System.Management.Automation.PSCustomObject]) {
                            $take = [Math]::Min($arr.Count, 2)
                            for ($i = 0; $i -lt $take; $i++) {
                                $child = Get-JsonFields -Object $arr[$i] -Prefix ("$name[$i]") -Depth ($Depth + 1)
                                foreach ($k in $child.Keys) { $map[$k] = $child[$k] }
                            }
                        } elseif ($arr[0] -is [string]) {
                            $map[$name] = [string]$arr[0]
                        }
                    }
                }
            }
        }
        return $map
    }

    function Get-BestArray {
        param($Node, [string]$Path = '', [int]$Depth = 0)
        $best = @()
        $bestPath = ''
        if ($null -eq $Node -or $Depth -gt 6) {
            return [pscustomobject]@{ Items = $best; Path = $bestPath }
        }
        if ($Node -is [System.Collections.IEnumerable] -and -not ($Node -is [string]) -and -not ($Node -is [System.Management.Automation.PSCustomObject])) {
            $arr = @($Node)
            if ($arr.Count -gt 0 -and $arr[0] -is [System.Management.Automation.PSCustomObject]) {
                return [pscustomobject]@{ Items = $arr; Path = $Path }
            }
            return [pscustomobject]@{ Items = @(); Path = '' }
        }
        if ($Node -is [System.Management.Automation.PSCustomObject]) {
            foreach ($prop in $Node.PSObject.Properties) {
                $childPath = if ($Path) { "$Path.$($prop.Name)" } else { $prop.Name }
                $value = $prop.Value
                if ($null -eq $value) { continue }
                if ($value -is [System.Collections.IEnumerable] -and -not ($value -is [string]) -and -not ($value -is [System.Management.Automation.PSCustomObject])) {
                    $arr = @($value)
                    if ($arr.Count -gt 0 -and $arr[0] -is [System.Management.Automation.PSCustomObject] -and $arr.Count -gt $best.Count) {
                        $best = $arr
                        $bestPath = $childPath
                    }
                } elseif ($value -is [System.Management.Automation.PSCustomObject]) {
                    $child = Get-BestArray -Node $value -Path $childPath -Depth ($Depth + 1)
                    if ($child.Items.Count -gt $best.Count) {
                        $best = $child.Items
                        $bestPath = $child.Path
                    }
                }
            }
        }
        return [pscustomobject]@{ Items = $best; Path = $bestPath }
    }

    function Test-AudioValue {
        param([string]$Name, [string]$Value)
        if ([string]::IsNullOrWhiteSpace($Value)) { return $false }
        $lowerName = $Name.ToLowerInvariant()
        $lowerValue = $Value.ToLowerInvariant()
        # 文件名也可能是 .wav/.mp3，但文件名不能直接播放；必须先确认是 URL。
        if (-not $lowerValue.StartsWith('http')) { return $false }
        $audioSuffix = [regex]::IsMatch($lowerValue, '\.(mp3|m4a|flac|aac|ogg|wav|opus)(\?|$)')
        $strongName = @(
            'previewurl', 'preview_url', 'preview', 'url_resolved',
            'streamurl', 'stream_url', 'stream', 'stream.url', 'audio_url', 'audio',
            'download_url', 'downloadurl', 'listen_url', 'listenurl',
            'files[0].download_url', 'files[0].file'
        ) -contains $lowerName
        if ($strongName) { return $true }
        return $audioSuffix
    }

    function Get-Titles {
        param($Items, [string]$TitleField)
        if ([string]::IsNullOrWhiteSpace($TitleField)) { return @() }
        $titles = @()
        foreach ($item in @($Items | Select-Object -First 10)) {
            if ($item -isnot [System.Management.Automation.PSCustomObject]) { continue }
            # 只取标题路径的第一个值，避免把嵌套字段拼进来
            $value = $item
            foreach ($segment in ($TitleField -split '\.')) {
                if ($value -isnot [System.Management.Automation.PSCustomObject]) { $value = $null; break }
                $value = $value.PSObject.Properties[$segment].Value
            }
            if ($null -ne $value) {
                $text = ([string]$value).Trim()
                if ($text) { $titles += $text }
            }
        }
        return $titles
    }

    function Invoke-Probe {
        param([string]$Url)
        try {
            $r = Invoke-WebRequest -Uri $Url -TimeoutSec 16 -MaximumRedirection 5 `
                -Headers @{ 'User-Agent' = 'MVP-SourceProbe/1.0'; 'Accept' = 'application/json,text/plain,*/*' } `
                -UseBasicParsing -SkipCertificateCheck
            $body = $r.Content
            if ($body -is [byte[]]) { $body = [System.Text.Encoding]::UTF8.GetString($body) }
            else { $body = [string]$body }
            return [pscustomobject]@{
                Status = [int]$r.StatusCode
                ContentType = [string]$r.Headers['Content-Type']
                Body = $body
                Error = ''
            }
        } catch {
            $resp = $_.Exception.Response
            $status = 0
            $body = ''
            if ($null -ne $resp) {
                try { $status = [int]$resp.StatusCode } catch { $status = 0 }
                try {
                    if ($null -ne $resp.Content) { $body = [string]$resp.Content.ReadAsStringAsync().Result }
                } catch { $body = '' }
            }
            return [pscustomobject]@{
                Status = $status
                ContentType = ''
                Body = $body
                Error = $_.Exception.Message
            }
        }
    }

    function Get-Url {
        param([string]$Template, [string]$Value)
        $encoded = [System.Uri]::EscapeDataString($Value)
        return $Template.Replace('{keyword}', $encoded).Replace('{limit}', [string]$limit)
    }

    function Get-HostPath {
        param([string]$Url)
        try {
            $u = [System.Uri]$Url
            return "$($u.Host)$($u.AbsolutePath)"
        } catch { return '' }
    }

    $urlTemplate = [string]$node.baseUrl
    $firstUrl = Get-Url $urlTemplate $keyword
    $first = Invoke-Probe $firstUrl
    $errorKind = 'ok'
    $arrayPath = ''
    $itemCount = 0
    $titleField = ''
    $streamField = ''
    $streamKind = ''
    $playableCount = 0
    $fieldNames = @()
    $searchCompared = $false
    $secondCount = 0
    $sameTitles = $false
    $searchOk = $false
    $note = ''

    if ($first.Status -eq 0) {
        $errorKind = 'connection'
        $note = $first.Error
    } elseif ($first.Status -eq 401 -or $first.Status -eq 403) {
        $errorKind = 'auth'
        $note = '需要登录/密钥'
    } elseif ($first.Status -eq 404) {
        $errorKind = 'notFound'
        $note = '路径不存在'
    } elseif ($first.Status -eq 429) {
        $errorKind = 'rateLimit'
        $note = '限流'
    } elseif ($first.Status -ge 500) {
        $errorKind = 'serverError'
        $note = "HTTP $($first.Status)"
    } elseif ([string]::IsNullOrWhiteSpace($first.Body)) {
        $errorKind = 'empty'
        $note = '空响应'
    } else {
        $prefix = $first.Body.TrimStart()
        $lowerPrefix = $prefix.Substring(0, [Math]::Min($prefix.Length, 160)).ToLowerInvariant()
        if ($lowerPrefix.StartsWith('<?xml') -or $lowerPrefix.StartsWith('<rss') -or $lowerPrefix.StartsWith('<list') -or ($lowerPrefix.StartsWith('<') -and -not $lowerPrefix.StartsWith('<!doctype') -and -not $lowerPrefix.StartsWith('<html'))) {
            $errorKind = 'xml'
            $note = 'XML 响应，需要 f=json'
        } elseif ($lowerPrefix.StartsWith('<!doctype') -or $lowerPrefix.StartsWith('<html')) {
            $errorKind = 'html'
            $note = '网页响应，不是 JSON API'
        } else {
        try {
            $json = $first.Body | ConvertFrom-Json -Depth 20
            $rootValue = $json
            # Subsonic 风格错误：HTTP 200 但 subsonic-response.status=failed
            if ($rootValue -is [System.Management.Automation.PSCustomObject] -and $null -ne $rootValue.PSObject.Properties['subsonic-response']) {
                $sub = $rootValue.PSObject.Properties['subsonic-response'].Value
                if ($null -ne $sub -and $null -ne $sub.PSObject.Properties['status'] -and [string]$sub.status -eq 'failed') {
                    $errorKind = 'auth'
                    $note = 'Subsonic 返回 failed（认证/参数）'
                }
            }
            if ($errorKind -ne 'auth') {
                $arrayInfo = Get-BestArray -Node $rootValue
                if ($arrayInfo.Items.Count -eq 0) {
                    $errorKind = 'noArray'
                    $note = 'JSON 没有对象数组'
                } else {
                    $arrayPath = $arrayInfo.Path
                    $itemCount = $arrayInfo.Items.Count
                    $fields = Get-JsonFields -Object $arrayInfo.Items[0]
                    $fieldNames = @($fields.Keys | Sort-Object)
                    $titleNeedles = @('trackname', 'track_name', 'songname', 'song_name', 'upload_name', 'title', 'name', 'display_name')
                    foreach ($needle in $titleNeedles) {
                        foreach ($key in $fieldNames) {
                            if ($key.ToLowerInvariant() -eq $needle -or $key.ToLowerInvariant().EndsWith(".$needle")) {
                                $titleField = $key
                                break
                            }
                        }
                        if ($titleField) { break }
                    }
                    foreach ($key in $fieldNames) {
                        $value = [string]$fields[$key]
                        if (Test-AudioValue -Name $key -Value $value) {
                            $streamField = $key
                            $streamKind = 'direct'
                            $playableCount++
                            break
                        }
                    }
                    if (-not $titleField) {
                        $errorKind = 'noTitle'
                        $note = '有数组但猜不出标题字段'
                    } elseif (-not $streamField) {
                        $errorKind = 'noStream'
                        $note = '有标题但猜不出播放地址字段'
                    } else {
                        $errorKind = 'ok'
                    }
                }
            }
        } catch {
            $errorKind = 'notJson'
            $note = 'JSON 解析失败'
        }
        }
    }

    # 首轮有标题时，用第二个关键词验证“关键词是否真的生效”。
    # 仅对含 {keyword} 的地址做；catalog 节点不做二次搜索。
    if ($itemCount -gt 0 -and $titleField -and $urlTemplate.Contains('{keyword}')) {
        $searchCompared = $true
        $secondUrl = Get-Url $urlTemplate $secondKeyword
        $second = Invoke-Probe $secondUrl
        $secondCount = 0
        $firstTitles = Get-Titles -Items (Get-BestArray -Node $json).Items -TitleField $titleField
        $secondTitles = @()
        if ($second.Status -ge 200 -and $second.Status -lt 300 -and -not [string]::IsNullOrWhiteSpace($second.Body)) {
            try {
                $secondJson = $second.Body | ConvertFrom-Json -Depth 20
                $secondArray = Get-BestArray -Node $secondJson
                $secondCount = $secondArray.Items.Count
                $secondTitles = Get-Titles -Items $secondArray.Items -TitleField $titleField
            } catch { $secondCount = 0 }
        }
        if ($secondCount -eq 0) {
            $searchOk = $true
            $note = '二次关键词无结果，按首轮结果接受'
        } else {
            $firstSet = @($firstTitles | Sort-Object -Unique)
            $secondSet = @($secondTitles | Sort-Object -Unique)
            $sameTitles = ($firstSet.Count -gt 0 -and $secondSet.Count -eq $firstSet.Count -and @(Compare-Object -ReferenceObject $firstSet -DifferenceObject $secondSet).Count -eq 0)
            $searchOk = -not $sameTitles
            if ($sameTitles) { $note = '两次返回同一组标题，关键词可能被忽略' }
        }
    }

    [pscustomobject]@{
        round = [int]$node.round
        source = [string]$node.source
        name = [string]$node.name
        kind = [string]$node.kind
        hostPath = Get-HostPath $urlTemplate
        httpStatus = $first.Status
        contentType = $first.ContentType
        errorKind = $errorKind
        arrayPath = $arrayPath
        itemCount = $itemCount
        titleField = $titleField
        streamField = $streamField
        streamKind = $streamKind
        playableCount = $playableCount
        searchCompared = $searchCompared
        secondCount = $secondCount
        sameTitles = $sameTitles
        searchOk = $searchOk
        note = $note
        fieldNames = $fieldNames
    }
} -ThrottleLimit $Throttle

$results = @($results | Sort-Object round, name, hostPath)
$total = $results.Count
$stats = [ordered]@{
    total = $total
    httpOk = @($results | Where-Object { $_.httpStatus -ge 200 -and $_.httpStatus -lt 300 }).Count
    json = @($results | Where-Object { $_.errorKind -in @('ok','noArray','noTitle','noStream') }).Count
    auth = @($results | Where-Object { $_.errorKind -eq 'auth' }).Count
    xml = @($results | Where-Object { $_.errorKind -eq 'xml' }).Count
    html = @($results | Where-Object { $_.errorKind -eq 'html' }).Count
    noArray = @($results | Where-Object { $_.errorKind -eq 'noArray' }).Count
    noTitle = @($results | Where-Object { $_.errorKind -eq 'noTitle' }).Count
    noStream = @($results | Where-Object { $_.errorKind -eq 'noStream' }).Count
    connection = @($results | Where-Object { $_.errorKind -eq 'connection' }).Count
    serverError = @($results | Where-Object { $_.errorKind -eq 'serverError' }).Count
    searchable = @($results | Where-Object { $_.searchOk }).Count
    catalogOnly = @($results | Where-Object { $_.itemCount -gt 0 -and $_.streamField -eq '' }).Count
    directPlayable = @($results | Where-Object { $_.streamField -ne '' }).Count
}
$roundStats = for ($r = 1; $r -le 10; $r++) {
    $items = @($results | Where-Object { $_.round -eq $r })
    if ($items.Count -eq 0) { continue }
    [pscustomobject]@{
        round = $r
        tested = $items.Count
        searchable = @($items | Where-Object { $_.searchOk }).Count
        catalogOnly = @($items | Where-Object { $_.itemCount -gt 0 -and $_.streamField -eq '' }).Count
        directPlayable = @($items | Where-Object { $_.streamField -ne '' }).Count
    }
}

$out = [ordered]@{
    schemaVersion = 1
    mediaType = 'music'
    generatedAt = (Get-Date -Format 'yyyy-MM-dd')
    keyword = $Keyword
    secondKeyword = $SecondKeyword
    note = '只保存状态、结构、字段名和计数，不保存歌曲标题、播放地址、封面地址和凭据。'
    stats = $stats
    rounds = $roundStats
    results = $results
}
$dir = Split-Path -Parent $OutFile
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$out | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutFile -Encoding utf8
Write-Output "wrote $OutFile"
$stats | Format-Table -AutoSize
$roundStats | Format-Table -AutoSize
