# 收集音乐站点/搜索接口候选节点，按 10 轮输出。
# 只记录公开文档、公开实例页里出现的接口地址与参数名，不保存歌曲标题、播放地址。
# 用法：pwsh -File tools/source-probe/collect_music_nodes.ps1

$ErrorActionPreference = 'Stop'
$outFile = Join-Path $PSScriptRoot '..\..\docs\source-probe\music-nodes.json'
$accessed = '2026-11-23'

$script:nodes = [System.Collections.Generic.List[object]]::new()
$script:seen = @{}

# 节点数据会随仓库提交，不能把公开 demo 的账号密码原样写进去。
# u/p 只用于 Subsonic 兼容；t/s 是 token/salt。统一替换成占位符，
# 真实值只允许出现在 probe 脚本的测试凭据表或用户本地配置里。
function ConvertTo-RedactedNodeUrl {
    param([string]$Url)
    $redactions = [ordered]@{
        u       = '{user}'
        p       = '{password}'
        t       = '{token}'
        s       = '{salt}'
        app_key = '{appKey}'
        api_key = '{apiKey}'
    }
    foreach ($entry in $redactions.GetEnumerator()) {
        $replacement = '$1' + $entry.Value
        $Url = [regex]::Replace(
            $Url,
            ('(?i)([?&]{0}=)[^&]*' -f [regex]::Escape($entry.Key)),
            $replacement
        )
    }
    return $Url
}

function Add-Node {
    param(
        [int]$Round,
        [string]$Source,
        [string]$Name,
        [string]$BaseUrl,
        [string]$Kind = 'search',
        [string]$Note = ''
    )
    $redactedUrl = ConvertTo-RedactedNodeUrl $BaseUrl
    # 去重仍按原始 URL：同一路径换 demo 账号也是独立验证节点，
    # 但写入文件的 baseUrl 已脱敏，不会把 u/p 带出去。
    $key = $BaseUrl.TrimEnd('/').ToLowerInvariant()
    if ($script:seen.ContainsKey($key)) { return }
    $script:seen[$key] = $true
    $hostName = ''
    try { $hostName = ([uri]$BaseUrl).Host.ToLowerInvariant() } catch { }
    $script:nodes.Add([pscustomobject]@{
            round    = $Round
            source   = $Source
            name     = $Name
            baseUrl  = $redactedUrl
            host     = $hostName
            kind     = $Kind
            note     = $Note
            accessed = $accessed
        })
}

# 第 1 轮：无需密钥的公开音乐搜索 API。
# Deezer: https://developers.deezer.com/api/search
# iTunes: https://performance-partners.apple.com/search-api
# Audius: https://docs.audius.org/developers/api/
# Radio Browser: https://de1.api.radio-browser.info/
# ccMixter: https://ccmixter.org/query-api
# Openverse: https://api.openverse.org/v1/
# Mixcloud: https://www.mixcloud.com/developers/
$r1 = @(
    @('Deezer 搜索', 'https://api.deezer.com/search?q={keyword}&limit={limit}', 'search', 'title + preview(.mp3)'),
    @('iTunes 搜索', 'https://itunes.apple.com/search?term={keyword}&media=music&limit={limit}', 'search', 'trackName + previewUrl(.m4a)'),
    @('Audius 搜索', 'https://api.audius.co/v1/tracks/search?query={keyword}&limit={limit}', 'search', 'title，音频需再拼 stream 端点'),
    @('Audius Discovery', 'https://discoveryprovider.audius.co/v1/tracks/search?query={keyword}&limit={limit}', 'search', '同一 API 的 discovery provider'),
    @('Radio Browser DE', 'https://de1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', 'name + url_resolved'),
    @('Radio Browser NL', 'https://nl1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '镜像'),
    @('Radio Browser AT', 'https://at1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '镜像'),
    @('Radio Browser FR', 'https://fr1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '镜像'),
    @('Radio Browser UK', 'https://uk1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '镜像'),
    @('Radio Browser US', 'https://us1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '镜像'),
    @('ccMixter 查询', 'https://ccmixter.org/api/query?f=json&search={keyword}&limit={limit}', 'search', 'upload_name + 下载字段'),
    @('Openverse 音频', 'https://api.openverse.org/v1/audio/?q={keyword}&page_size={limit}', 'search', 'title + url'),
    @('Openverse Jamendo', 'https://api.openverse.org/v1/audio/?q={keyword}&source=jamendo&page_size={limit}', 'search', '按 source 过滤'),
    @('Openverse Freesound', 'https://api.openverse.org/v1/audio/?q={keyword}&source=freesound&page_size={limit}', 'search', '按 source 过滤'),
    @('Mixcloud 搜索', 'https://api.mixcloud.com/search/?q={keyword}&type=cloudcast&limit={limit}', 'search', '只读 API；结果多为节目页')
)
foreach ($n in $r1) { Add-Node 1 'public-api-docs' $n[0] $n[1] $n[2] $n[3] }

# 第 2 轮：Subsonic 兼容服务（Navidrome / Subsonic demo）。
# Navidrome demo: https://www.navidrome.org/demo/
# Subsonic API: https://www.subsonic.org/pages/api.jsp
$r2 = @(
    @('Navidrome search3 JSON', 'https://demo.navidrome.org/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '标准 search3'),
    @('Navidrome search2 JSON', 'https://demo.navidrome.org/rest/search2?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '旧版 search2'),
    @('Navidrome search JSON', 'https://demo.navidrome.org/rest/search?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '旧版 search'),
    @('Navidrome 只搜歌曲', 'https://demo.navidrome.org/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}&artistCount=0&albumCount=0', 'search', '显式关闭专辑/歌手'),
    @('Navidrome 错误密码', 'https://demo.navidrome.org/rest/search3?u=demo&p=wrong&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'auth', '错误凭据诊断'),
    @('Subsonic search3 JSON', 'https://demo.subsonic.org/rest/search3?u=guest4&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '标准 search3'),
    @('Subsonic search2 JSON', 'https://demo.subsonic.org/rest/search2?u=guest4&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '旧版 search2'),
    @('Subsonic search JSON', 'https://demo.subsonic.org/rest/search?u=guest4&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '旧版 search'),
    @('Subsonic guest5', 'http://demo.subsonic.org/rest/search3?u=guest5&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '公开 demo 账号'),
    @('Subsonic guest6', 'http://demo.subsonic.org/rest/search3?u=guest6&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '公开 demo 账号'),
    @('music.install.so', 'http://music.install.so/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '论坛旧 demo'),
    @('Navidrome XML', 'https://demo.navidrome.org/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=xml&query={keyword}&songCount={limit}', 'xml', '默认 XML 诊断'),
    @('Subsonic XML', 'https://demo.subsonic.org/rest/search3?u=guest4&p=guest&v=1.16.1&c=MVP&f=xml&query={keyword}&songCount={limit}', 'xml', '默认 XML 诊断'),
    @('Navidrome 无认证', 'https://demo.navidrome.org/rest/search3?query={keyword}&songCount={limit}', 'auth', '无 u/p 诊断'),
    @('Subsonic 无认证', 'https://demo.subsonic.org/rest/search3?query={keyword}&songCount={limit}', 'auth', '无 u/p 诊断')
)
foreach ($n in $r2) { Add-Node 2 'navidrome-subsonic-demo' $n[0] $n[1] $n[2] $n[3] }

# 第 3 轮：Funkwhale 实例与 API v1/v2。
# https://docs.funkwhale.audio/developer/api/
# https://network.funkwhale.audio/
$r3 = @(
    @('open.audio tracks v1', 'https://open.audio/api/v1/tracks/?q={keyword}&page_size={limit}', 'search', '公开实例'),
    @('open.audio artists v1', 'https://open.audio/api/v1/artists/?q={keyword}&page_size={limit}', 'search', '公开实例'),
    @('open.audio albums v1', 'https://open.audio/api/v1/albums/?q={keyword}&page_size={limit}', 'search', '公开实例'),
    @('open.audio search v1', 'https://open.audio/api/v1/search?query={keyword}', 'search', '聚合搜索'),
    @('open.audio albums v2', 'https://open.audio/api/v2/albums?q={keyword}', 'search', '文档示例'),
    @('open.audio tracks v2', 'https://open.audio/api/v2/tracks/?q={keyword}&page_size={limit}', 'search', 'v2 试验接口'),
    @('demo.funkwhale tracks v1', 'https://demo.funkwhale.audio/api/v1/tracks/?q={keyword}&page_size={limit}', 'search', '官方 demo'),
    @('demo.funkwhale artists v1', 'https://demo.funkwhale.audio/api/v1/artists/?q={keyword}&page_size={limit}', 'search', '官方 demo'),
    @('demo.funkwhale albums v1', 'https://demo.funkwhale.audio/api/v1/albums/?q={keyword}&page_size={limit}', 'search', '官方 demo'),
    @('demo.funkwhale albums v2', 'https://demo.funkwhale.audio/api/v2/albums?q={keyword}', 'search', 'v2 试验接口'),
    @('demo.funkwhale search v1', 'https://demo.funkwhale.audio/api/v1/search?query={keyword}', 'search', '聚合搜索'),
    @('tanukitunes albums v2', 'https://tanukitunes.com/api/v2/albums?q={keyword}', 'search', '文档示例实例'),
    @('soultuna tracks v1', 'https://soultuna.nry.pw/api/v1/tracks/?q={keyword}&page_size={limit}', 'search', '公开 Liberapay 实例'),
    @('funkwhale.duckdns tracks v1', 'https://funkwhale.duckdns.org/api/v1/tracks/?q={keyword}&page_size={limit}', 'search', '公开 Liberapay 实例'),
    @('funkwhale.audio tracks v1', 'https://funkwhale.audio/api/v1/tracks/?q={keyword}&page_size={limit}', 'search', '项目入口域名')
)
foreach ($n in $r3) { Add-Node 3 'funkwhale-docs-network' $n[0] $n[1] $n[2] $n[3] }

# 第 4 轮：AzuraCast 公开 API 与 station 子资源。
# https://demo.azuracast.com/api
$r4 = @(
    @('AzuraCast nowplaying 全部', 'https://demo.azuracast.com/api/nowplaying', 'catalog', '当前播放列表'),
    @('AzuraCast nowplaying station', 'https://demo.azuracast.com/api/nowplaying/azuratest_radio', 'catalog', 'station shortcode'),
    @('AzuraCast nowplaying id', 'https://demo.azuracast.com/api/nowplaying/1', 'catalog', 'station id'),
    @('AzuraCast stations', 'https://demo.azuracast.com/api/stations', 'catalog', '公开电台列表'),
    @('AzuraCast station id', 'https://demo.azuracast.com/api/station/1', 'catalog', 'station 详情'),
    @('AzuraCast station shortcode', 'https://demo.azuracast.com/api/station/azuratest_radio', 'catalog', 'station 详情'),
    @('AzuraCast station nowplaying', 'https://demo.azuracast.com/api/station/1/nowplaying', 'catalog', 'station 当前播放'),
    @('AzuraCast station history', 'https://demo.azuracast.com/api/station/1/history', 'catalog', '历史'),
    @('AzuraCast station requests', 'https://demo.azuracast.com/api/station/1/requests', 'catalog', '点播'),
    @('AzuraCast station streamers', 'https://demo.azuracast.com/api/station/1/streamers', 'catalog', '主播'),
    @('AzuraCast station playlists', 'https://demo.azuracast.com/api/station/1/playlists', 'catalog', '播放列表'),
    @('AzuraCast station media', 'https://demo.azuracast.com/api/station/1/media', 'catalog', '媒体库'),
    @('AzuraCast station files', 'https://demo.azuracast.com/api/station/1/files', 'catalog', '文件'),
    @('AzuraCast station search', 'https://demo.azuracast.com/api/station/1/search?q={keyword}', 'search', '搜索端点待核实'),
    @('AzuraCast global search', 'https://demo.azuracast.com/api/search?q={keyword}', 'search', '全局搜索待核实')
)
foreach ($n in $r4) { Add-Node 4 'azuracast-demo-api' $n[0] $n[1] $n[2] $n[3] }

# 第 5 轮：Jellyfin / Ampache 兼容入口。
# https://demo.jellyfin.org/
# https://ampache.org/demo/ ; https://demo.ampache.dev/jellyfin
$r5 = @(
    @('Jellyfin Items Audio', 'https://demo.jellyfin.org/Items?searchTerm={keyword}&IncludeItemTypes=Audio&Limit={limit}', 'search', '标准 /Items'),
    @('Jellyfin Items Recursive', 'https://demo.jellyfin.org/Items?searchTerm={keyword}&Recursive=true&Limit={limit}', 'search', '递归搜索'),
    @('Jellyfin Search Hints', 'https://demo.jellyfin.org/Search/Hints?searchTerm={keyword}&Limit={limit}', 'search', '联想搜索'),
    @('Jellyfin Audio', 'https://demo.jellyfin.org/Audio?searchTerm={keyword}&Limit={limit}', 'search', '音频端点'),
    @('Jellyfin Info Public', 'https://demo.jellyfin.org/System/Info/Public', 'catalog', '公开系统信息'),
    @('Jellyfin Users Public', 'https://demo.jellyfin.org/Users/Public', 'catalog', '公开用户'),
    @('Ampache api song', 'https://demo.ampache.dev/api/search?type=song&filter={keyword}&limit={limit}', 'search', 'Ampache API'),
    @('Ampache api album', 'https://demo.ampache.dev/api/search?type=album&filter={keyword}&limit={limit}', 'search', 'Ampache API'),
    @('Ampache api artist', 'https://demo.ampache.dev/api/search?type=artist&filter={keyword}&limit={limit}', 'search', 'Ampache API'),
    @('Ampache xml song', 'https://demo.ampache.dev/server/xml.server.php?action=search&type=song&filter={keyword}&limit={limit}', 'search', 'XML API'),
    @('Ampache Jellyfin Items', 'https://demo.ampache.dev/jellyfin/Items?searchTerm={keyword}&IncludeItemTypes=Audio&Limit={limit}', 'search', 'Jellyfin 兼容层'),
    @('Ampache rest search3', 'https://demo.ampache.dev/rest/search3?query={keyword}&songCount={limit}', 'search', 'Subsonic 兼容层'),
    @('Ampache api v1', 'https://demo.ampache.dev/api/v1/search?q={keyword}&limit={limit}', 'search', 'v1 试验路径'),
    @('Jellyfin MusicAudio', 'https://demo.jellyfin.org/Items?searchTerm={keyword}&IncludeItemTypes=MusicAudio&Limit={limit}', 'search', 'MusicAudio 类型'),
    @('Jellyfin Items Fields', 'https://demo.jellyfin.org/Items?searchTerm={keyword}&IncludeItemTypes=Audio&Fields=Path&Limit={limit}', 'search', '带 Path 字段')
)
foreach ($n in $r5) { Add-Node 5 'jellyfin-ampache-demo' $n[0] $n[1] $n[2] $n[3] }

# 第 6 轮：Radio Browser 镜像与网络电台目录。
# https://api.radio-browser.info/ ; https://dir.xiph.org/
$r6 = @(
    @('Radio Browser all', 'https://all.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '聚合镜像'),
    @('Radio Browser api', 'https://api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', '主入口'),
    @('Radio Browser www', 'https://www.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', 'www 子域'),
    @('Radio Browser streams', 'https://streams.radio-browser.info/json/stations/search?name={keyword}&limit={limit}', 'search', 'streams 子域'),
    @('Radio Browser DE hidebroken', 'https://de1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}&hidebroken=true', 'search', '过滤坏流'),
    @('Radio Browser DE2 hidebroken', 'https://de2.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}&hidebroken=true', 'search', '官方镜像'),
    @('Radio Browser FI hidebroken', 'https://fi1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}&hidebroken=true', 'search', '官方镜像'),
    @('Radio Browser NL hidebroken', 'https://nl1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}&hidebroken=true', 'search', '过滤坏流'),
    @('Radio Browser byname', 'https://de1.api.radio-browser.info/json/stations/byname/{keyword}?limit={limit}', 'search', '路径式名称搜索'),
    @('Radio Browser bytag', 'https://de1.api.radio-browser.info/json/stations/bytag/{keyword}?limit={limit}', 'search', '路径式标签搜索'),
    @('Xiph 目录 XML', 'https://dir.xiph.org/yp.xml', 'xml', 'YP XML 目录'),
    @('Xiph 搜索页', 'https://dir.xiph.org/search?search={keyword}', 'html', '网页搜索'),
    @('Shoutcast', 'https://www.shoutcast.com/', 'html', '网页目录'),
    @('Internet Radio', 'https://www.internet-radio.com/search/?q={keyword}', 'html', '网页目录'),
    @('Icecast 官网', 'https://www.icecast.org/', 'html', '项目入口')
)
foreach ($n in $r6) { Add-Node 6 'radio-browser-xiph' $n[0] $n[1] $n[2] $n[3] }

# 第 7 轮：公开音乐媒体站的 WordPress REST 搜索。
# 这些站点公开提供 /wp-json/；只取文章搜索结果，不作为音频直链源。
$r7 = @(
    @('Pitchfork', 'https://pitchfork.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('Stereogum', 'https://www.stereogum.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('Consequence', 'https://consequence.net/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('BrooklynVegan', 'https://brooklynvegan.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('NME', 'https://www.nme.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('Rolling Stone', 'https://www.rollingstone.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('Spin', 'https://www.spin.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('Loudwire', 'https://loudwire.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '金属媒体'),
    @('Metal Injection', 'https://metalinjection.net/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '金属媒体'),
    @('Dancing Astronaut', 'https://dancingastronaut.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '电子音乐媒体'),
    @('This Song Is Sick', 'https://thissongissick.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('AltPress', 'https://www.altpress.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('Clash', 'https://www.clashmusic.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('DIY', 'https://diymag.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体'),
    @('The Line of Best Fit', 'https://www.thelineofbestfit.com/wp-json/wp/v2/search?search={keyword}&per_page={limit}', 'search', '音乐媒体')
)
foreach ($n in $r7) { Add-Node 7 'music-wordpress-rest' $n[0] $n[1] $n[2] $n[3] }

# 第 8 轮：开放音乐档案、netlabel 与聚合目录。
# https://archive.org/advancedsearch.php ; https://freemusicarchive.org/api
# https://developer.jamendo.com/v3.0 ; https://freesound.org/docs/api/
$r8 = @(
    @('Internet Archive identifier', 'https://archive.org/advancedsearch.php?q={keyword}&fl[]=identifier&rows={limit}&output=json', 'search', '元数据，不含直链'),
    @('Internet Archive title', 'https://archive.org/advancedsearch.php?q={keyword}&fl[]=title&rows={limit}&output=json', 'search', '元数据'),
    @('Free Music Archive api', 'https://freemusicarchive.org/api/search?q={keyword}', 'search', '开放音乐档案'),
    @('Free Music Archive 搜索页', 'https://freemusicarchive.org/search?quicksearch={keyword}', 'html', '网页搜索'),
    @('Jamendo tracks 无 key', 'https://api.jamendo.com/v3.0/tracks/?format=json&limit={limit}&search={keyword}', 'auth', '需要 client_id'),
    @('Jamendo tracks demo key', 'https://api.jamendo.com/v3.0/tracks/?client_id=demo&format=json&limit={limit}&search={keyword}', 'auth', 'demo key 诊断'),
    @('Freesound apiv2', 'https://freesound.org/apiv2/search/text/?query={keyword}&page_size={limit}', 'auth', '需要 token'),
    @('Freesound 搜索页', 'https://freesound.org/search/?q={keyword}', 'html', '网页搜索'),
    @('SoundCloud 搜索页', 'https://soundcloud.com/search?q={keyword}', 'html', '网页搜索'),
    @('Bandcamp 搜索页', 'https://bandcamp.com/search?q={keyword}', 'html', '网页搜索'),
    @('Mixcloud 网页搜索', 'https://www.mixcloud.com/search/?q={keyword}&type=cloudcast', 'html', '网页搜索'),
    @('hearthis.at api', 'https://hearthis.at/api/search?q={keyword}', 'search', '独立音乐平台'),
    @('Audiomack 搜索页', 'https://audiomack.com/search?q={keyword}', 'html', '网页搜索'),
    @('ReverbNation 搜索页', 'https://www.reverbnation.com/search?q={keyword}', 'html', '网页搜索'),
    @('Wikimedia Commons', 'https://commons.wikimedia.org/w/api.php?action=query&list=search&srsearch={keyword}&format=json&srlimit={limit}', 'search', '文件元数据')
)
foreach ($n in $r8) { Add-Node 8 'open-music-archives' $n[0] $n[1] $n[2] $n[3] }

# 第 9 轮：中文/区域音乐搜索入口。仅记录公开可访问的路径形态，
# 不假设其授权、版权或长期稳定性。
$r9 = @(
    @('网易云 web 搜索', 'https://music.163.com/api/search/get/web?s={keyword}&type=1&limit={limit}', 'search', '参数 s'),
    @('网易云 cloudsearch', 'https://music.163.com/api/cloudsearch/pc?s={keyword}&type=1&limit={limit}', 'search', '参数 s'),
    @('QQ 音乐 client_search', 'https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w={keyword}&format=json&n={limit}', 'search', '参数 w'),
    @('QQ 音乐 search_for_qq', 'https://c.y.qq.com/soso/fcgi-bin/search_for_qq_cp?w={keyword}&format=json&n={limit}', 'search', '参数 w'),
    @('酷狗 m 站', 'https://m.kugou.com/api/v3/search/song?keyword={keyword}&page=1&pagesize={limit}', 'search', '参数 keyword'),
    @('酷狗 mobilecdn', 'https://mobilecdn.kugou.com/api/v3/search/song?keyword={keyword}&page=1&pagesize={limit}', 'search', '参数 keyword'),
    @('酷狗 trackercdn', 'https://trackercdn.kugou.com/api/v3/search/song?keyword={keyword}&page=1&pagesize={limit}', 'search', '参数 keyword'),
    @('咪咕音乐', 'https://music.migu.cn/v3/api/music/search?keyword={keyword}', 'search', '参数 keyword'),
    @('虾米 api', 'https://api.xiami.com/web?v=2.0&app_key=demo&q={keyword}', 'auth', '旧接口，可能失效'),
    @('虾米搜索页', 'https://www.xiami.com/search?key={keyword}', 'html', '网页搜索'),
    @('iHeart api', 'https://api.iheart.com/api/v3/search/all?q={keyword}', 'search', '参数 q'),
    @('iHeart us api', 'https://us.api.iheart.com/api/v3/search/all?q={keyword}', 'search', '区域入口'),
    @('JioSaavn api', 'https://www.jiosaavn.com/api.php?__call=search.getResults&q={keyword}', 'search', '参数 q'),
    @('Gaana 搜索页', 'https://gaana.com/search?q={keyword}', 'html', '网页搜索'),
    @('Saavn api', 'https://www.saavn.com/api.php?__call=search.getResults&q={keyword}', 'search', '旧 Saavn 入口')
)
foreach ($n in $r9) { Add-Node 9 'regional-music-apis' $n[0] $n[1] $n[2] $n[3] }

# 第 10 轮：同一批自建音乐服务的变体路径，用来验证参数名、认证和 XML/JSON 差异。
$r10 = @(
    @('Navidrome songCount only', 'https://demo.navidrome.org/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}&albumCount=0&artistCount=0', 'search', '只保留歌曲结果'),
    @('Navidrome albumCount only', 'https://demo.navidrome.org/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount=0&albumCount={limit}', 'search', '只保留专辑结果'),
    @('Subsonic songCount only', 'https://demo.subsonic.org/rest/search3?u=guest4&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}&albumCount=0&artistCount=0', 'search', '只保留歌曲结果'),
    @('Subsonic guest5 search2', 'http://demo.subsonic.org/rest/search2?u=guest5&p=guest&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '旧接口 + 另一账号'),
    @('music.install.so search2', 'http://music.install.so/rest/search2?u=demo&p=demo&v=1.16.1&c=MVP&f=json&query={keyword}&songCount={limit}', 'search', '旧接口'),
    @('Ampache xml album', 'https://demo.ampache.dev/server/xml.server.php?action=search&type=album&filter={keyword}&limit={limit}', 'search', 'XML 专辑'),
    @('Ampache xml artist', 'https://demo.ampache.dev/server/xml.server.php?action=search&type=artist&filter={keyword}&limit={limit}', 'search', 'XML 歌手'),
    @('Ampache rest search3 JSON', 'https://demo.ampache.dev/rest/search3?query={keyword}&songCount={limit}&f=json', 'search', 'Subsonic 兼容 JSON'),
    @('Ampache Jellyfin Recursive', 'https://demo.ampache.dev/jellyfin/Items?searchTerm={keyword}&Recursive=true&Limit={limit}', 'search', 'Jellyfin 兼容层'),
    @('Jellyfin Audio Recursive', 'https://demo.jellyfin.org/Items?searchTerm={keyword}&IncludeItemTypes=Audio&Recursive=true&Limit={limit}', 'search', '递归 + Audio'),
    @('Jellyfin Hints Audio', 'https://demo.jellyfin.org/Search/Hints?searchTerm={keyword}&IncludeItemTypes=Audio&Limit={limit}', 'search', '联想 + Audio'),
    @('Funkwhale demo tracks sorted', 'https://demo.funkwhale.audio/api/v1/tracks/?q={keyword}&page_size={limit}&ordering=-creation_date', 'search', '排序变体'),
    @('Funkwhale open tracks sorted', 'https://open.audio/api/v1/tracks/?q={keyword}&page_size={limit}&ordering=-creation_date', 'search', '排序变体'),
    @('AzuraCast station search limit', 'https://demo.azuracast.com/api/station/1/search?q={keyword}&limit={limit}', 'search', '搜索 + limit'),
    @('AzuraCast global search limit', 'https://demo.azuracast.com/api/search?q={keyword}&limit={limit}', 'search', '全局搜索 + limit')
)
foreach ($n in $r10) { Add-Node 10 'self-hosted-variants' $n[0] $n[1] $n[2] $n[3] }

$roundCounts = for ($r = 1; $r -le 10; $r++) {
    [pscustomobject]@{
        round = $r
        count = @($script:nodes | Where-Object { $_.round -eq $r }).Count
    }
}

$siteKeys = @($script:nodes | ForEach-Object { $_.host } | Where-Object { $_ } | Sort-Object -Unique)
$endpointKeys = @($script:nodes | ForEach-Object { $_.baseUrl.TrimEnd('/').ToLowerInvariant() } | Sort-Object -Unique)

$doc = [ordered]@{
    schemaVersion = 1
    mediaType = 'music'
    generatedAt = $accessed
    note = '节点来自公开文档、公开实例页和公开 API 文档；只保存接口形态，不保存歌曲标题与播放地址。公开 demo 凭据已用 {user}/{password} 占位，真实值只留在 probe 测试脚本或本地配置里。'
    rounds = $roundCounts
    stats = [ordered]@{
        total = $script:nodes.Count
        siteCount = $siteKeys.Count
        endpointCount = $endpointKeys.Count
    }
    nodes = $script:nodes
}

$json = $doc | ConvertTo-Json -Depth 6
$dir = Split-Path -Parent $outFile
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$json | Set-Content -LiteralPath $outFile -Encoding utf8
Write-Output "wrote $outFile"
$roundCounts | Format-Table -AutoSize
