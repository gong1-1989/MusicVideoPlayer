# 收集影视站点/采集接口候选节点，按 10 轮输出。
# 只记录公开页面上的接口地址与域名，不保存影片标题、播放地址等内容。
# 用法：pwsh -File tools/source-probe/collect_video_nodes.ps1

$ErrorActionPreference = 'Stop'
$outFile = Join-Path $PSScriptRoot '..\..\docs\source-probe\video-nodes.json'
$accessed = '2026-11-23'

$script:nodes = [System.Collections.Generic.List[object]]::new()
$script:seen = @{}

function Add-Node {
    param(
        [int]$Round,
        [string]$Source,
        [string]$Name,
        [string]$BaseUrl,
        [string]$Kind = 'standard'
    )
    $key = $BaseUrl.TrimEnd('/').ToLowerInvariant()
    if ($script:seen.ContainsKey($key)) { return }
    $script:seen[$key] = $true
    $script:nodes.Add([pscustomobject]@{
            round   = $Round
            source  = $Source
            name    = $Name
            baseUrl = $BaseUrl
            kind    = $Kind
            accessed = $accessed
        })
}

# 第 1 轮：绿联 NAS 社区公开的 39 条 omnibox 影视源 JSON。
# https://club.ugnas.com/forum.php?action=printable&mod=viewthread&tid=1712
$r1 = @(
    @('豪华资源', 'https://hhzyapi.com/api.php/provide/vod/'),
    @('非凡影视', 'http://ffzy5.tv/api.php/provide/vod/'),
    @('极速资源', 'https://jszyapi.com/api.php/provide/vod/'),
    @('如意资源', 'http://cj.rycjapi.com/api.php/provide/vod/'),
    @('电影天堂资源', 'https://caiji.dyttzyapi.com/api.php/provide/vod/'),
    @('暴风资源', 'https://bfzyapi.com/api.php/provide/vod/'),
    @('天涯资源', 'https://tyyszy.com/api.php/provide/vod/'),
    @('360资源', 'https://360zy.com/api.php/provide/vod/'),
    @('卧龙资源', 'https://wolongzyw.com/api.php/provide/vod/'),
    @('豆瓣资源', 'https://dbzy.tv/api.php/provide/vod/'),
    @('魔爪资源', 'https://mozhuazy.com/api.php/provide/vod/'),
    @('魔都资源', 'https://www.mdzyapi.com/api.php/provide/vod/'),
    @('最大资源', 'https://api.zuidapi.com/api.php/provide/vod/'),
    @('樱花资源', 'https://m3u8.apiyhzy.com/api.php/provide/vod/'),
    @('百度云资源', 'https://api.apibdzy.com/api.php/provide/vod/'),
    @('无尽资源', 'https://api.wujinapi.me/api.php/provide/vod/'),
    @('旺旺短剧', 'https://wwzy.tv/api.php/provide/vod/'),
    @('iKun资源', 'https://ikunzyapi.com/api.php/provide/vod/'),
    @('量子资源站', 'https://cj.lziapi.com/api.php/provide/vod/'),
    @('茅台资源', 'https://caiji.maotaizy.cc/api.php/provide/vod/'),
    @('卧龙资源2', 'https://collect.wolongzyw.com/api.php/provide/vod/'),
    @('速播资源', 'https://subocaiji.com/api.php/provide/vod/'),
    @('索尼资源', 'https://suoniapi.com/api.php/provide/vod/'),
    @('虎牙资源', 'https://www.huyaapi.com/api.php/provide/vod/'),
    @('金鹰资源', 'https://jyzyapi.com/api.php/provide/vod/'),
    @('闪电资源', 'https://sdzyapi.com/api.php/provide/vod/'),
    @('飘零资源', 'https://p2100.net/api.php/provide/vod/'),
    @('1080资源库', 'https://api.1080zyku.com/inc/api_mac10.php/'),
    @('CK资源', 'https://ckzy.me/api.php/provide/vod/'),
    @('U酷资源', 'https://api.ukuapi.com/api.php/provide/vod/'),
    @('丫丫点播', 'https://cj.yayazy.net/api.php/provide/vod/'),
    @('步步高资源', 'https://api.yparse.com/api/json'),
    @('光速资源', 'https://api.guangsuapi.com/api.php/provide/vod/'),
    @('新浪点播', 'https://api.xinlangapi.com/xinlangapi.php/provide/vod/'),
    @('牛牛点播', 'https://api.niuniuzy.me/api.php/provide/vod/'),
    @('红牛资源', 'https://www.hongniuzy2.com/api.php/provide/vod/'),
    @('快车资源', 'https://caiji.kuaichezy.org/api.php/provide/vod/')
)
foreach ($n in $r1) { Add-Node 1 'club.ugnas.com/tid=1712' $n[0] $n[1] }

# 第 2 轮：空壳影视“主流采集资源站一览”。
# https://mac-cms.com/cms/cms-share
$r2 = @(
    @('量子资源', 'http://cj.lziapi.com/api.php/provide/vod/'),
    @('非凡资源', 'http://cj.ffzyapi.com/api.php/provide/vod/'),
    @('暴风资源', 'https://bfzyapi.com/api.php/provide/vod/'),
    @('光速资源', 'https://api.guangsuapi.com/api.php/provide/vod/'),
    @('快车资源', 'https://caiji.kczyapi.com/api.php/provide/vod/'),
    @('天空资源', 'http://m3u8.tiankongapi.com/api.php/provide/vod/'),
    @('红牛资源', 'https://www.hongniuzy2.com/api.php/provide/vod/'),
    @('卧龙资源', 'https://collect.wolongzyw.com/api.php/provide/vod/'),
    @('极速资源', 'https://jszyapi.com/api.php/provide/vod/'),
    @('豪华资源', 'https://hhzyapi.com/api.php/provide/vod/'),
    @('索尼资源', 'https://suoniapi.com/api.php/provide/vod/'),
    @('无尽资源', 'https://api.wujinapi.com/api.php/provide/vod/'),
    @('魔都资源', 'https://caiji.moduapi.cc/api.php/provide/vod/'),
    @('百度资源', 'https://api.apibdzy.com/api.php/provide/vod/'),
    @('1080资源', 'http://api.1080zyku.com/inc/api.php/provide/vod/'),
    @('360资源', 'https://360zy.com/api.php/provide/vod/'),
    @('电影天堂资源', 'http://caiji.dyttzyapi.com/api.php/provide/vod/'),
    @('量子优化接口', 'http://cj.lziapi.com/api.php/provide/vod/from/lzm3u8/'),
    @('非凡优化接口', 'http://cj.ffzyapi.com/api.php/provide/vod/from/ffm3u8/'),
    @('暴风优化接口', 'https://bfzyapi.com/api.php/provide/vod/from/bfzym3u8/'),
    @('光速优化接口', 'https://api.guangsuapi.com/api.php/provide/vod/from/gsm3u8/'),
    @('快车优化接口', 'https://caiji.kczyapi.com/api.php/provide/vod/from/kcm3u8/'),
    @('红牛优化接口', 'https://www.hongniuzy2.com/api.php/provide/vod/from/hnm3u8/'),
    @('极速优化接口', 'https://jszyapi.com/api.php/provide/vod/from/jsm3u8/'),
    @('豪华优化接口', 'https://hhzyapi.com/api.php/provide/vod/from/hhm3u8/'),
    @('索尼优化接口', 'https://suoniapi.com/api.php/provide/vod/from/snm3u8/'),
    @('无尽优化接口', 'https://api.wujinapi.com/api.php/provide/vod/from/wjm3u8/'),
    @('百度优化接口', 'https://api.apibdzy.com/api.php/provide/vod/from/dbm3u8/'),
    @('电影天堂优化接口', 'http://caiji.dyttzyapi.com/api.php/provide/vod/from/dyttm3u8/')
)
foreach ($n in $r2) { Add-Node 2 'mac-cms.com/cms/cms-share' $n[0] $n[1] }

# 第 3 轮：博客园、氢设计、MacCMS 文档中的非标准路径/播放解析接口。
# https://www.cnblogs.com/HGNET/p/16185339.html
# https://www.h2sheji.com/show-160.html
# https://www.maccms.plus/tc/api/web-design.html
$r3 = @(
    @('麒麟资源', 'https://www.qilinzyz.com/api.php/provide/vod/', 'standard'),
    @('番茄资源', 'http://api.fqzy.cc/api.php/provide/vod/', 'standard'),
    @('红牛资源旧域名', 'https://www.hongniuzy1.com/inc/api.php', 'inc'),
    @('快播资源', 'http://www.kuaibozy.com/api.php/provide/vod/from/kbm3u8/', 'custom'),
    @('8090资源', 'http://zy.yilans.net:8090/api.php/provide/vod/', 'custom'),
    @('h2解析接口', 'https://api.h2api.com/vinfo/index', 'parser'),
    @('h2解析备用', 'https://www.h2sheji.com/doc/api/cj', 'parser'),
    @('U酷播放解析', 'https://api.ukubf.com/m3u8/', 'parser'),
    @('红牛播放解析', 'https://www.tutukiki.com/m3u8/', 'parser'),
    @('CDN播放解析', 'https://cdn.zyc888.top/', 'parser'),
    @('番茄播放解析', 'https://jiexi.8b5q.cn/player/jx.php', 'parser'),
    @('百度云M3U8', 'https://m3u8.apibdzy.com/api.php/provide/vod/', 'standard'),
    @('WLZY非标准接口', 'http://cj.wlzy.tv/inc/api_mac_m3u8.php', 'inc'),
    @('UU资源站', 'https://www.uuzy.net/', 'web'),
    @('黑木耳', 'https://heimuer.tv/api.php/provide/vod/', 'standard'),
    @('快帆', 'https://api.kuaifan.tv/api.php/provide/vod/', 'standard'),
    @('海外看', 'https://haiwaikan.com/api.php/provide/vod/', 'standard')
)
foreach ($n in $r3) { Add-Node 3 $($n[3] ?? '公开技术文档') $n[0] $n[1] $n[2] }

# 第 4 轮：OpenI “采集站”专题中明确列出的主域名。
# https://openi.cn/favorites/14897.html
$r4 = @(
    @('非凡资源网', 'https://www.ffzy.tv/api.php/provide/vod/'),
    @('奥斯卡资源站', 'https://aosikazy.com/api.php/provide/vod/'),
    @('光速备用A', 'https://guangsuzy.com/api.php/provide/vod/'),
    @('金鹰备用A', 'https://jyzy1.com/api.php/provide/vod/'),
    @('酷豆资源', 'https://kudouzy1.com/api.php/provide/vod/'),
    @('飞速资源', 'https://fszy1.com/api.php/provide/vod/'),
    @('乐鱼资源', 'https://leyuzy.me/api.php/provide/vod/'),
    @('量子备用A', 'https://lzizy1.com/api.php/provide/vod/'),
    @('无尽备用A', 'https://wuzy1.com/api.php/provide/vod/'),
    @('优质资源A', 'https://1080zyk1.com/api.php/provide/vod/'),
    @('U酷备用A', 'https://ukuzy0.com/api.php/provide/vod/'),
    @('闪电资源', 'https://shandianzy.com/api.php/provide/vod/'),
    @('玖玖资源', 'https://jjzyw33.com/api.php/provide/vod/'),
    @('6U资源', 'https://www.6uzy.co/api.php/provide/vod/'),
    @('香蕉资源', 'https://xiangjiaozy1.com/api.php/provide/vod/'),
    @('淘片资源', 'https://taopianzy.vip/api.php/provide/vod/'),
    @('飞速备用', 'https://feisuzy.cc/api.php/provide/vod/'),
    @('光速备用B', 'https://guangsuziyuan.com/api.php/provide/vod/'),
    @('无尽备用B', 'https://wujinzy.com/api.php/provide/vod/'),
    @('玖玖备用', 'https://99fby2.com/api.php/provide/vod/'),
    @('卧龙备用A', 'https://wlzyw1.com/api.php/provide/vod/'),
    @('淘片备用', 'https://taopianzy.net/api.php/provide/vod/')
)
foreach ($n in $r4) { Add-Node 4 'openi.cn/favorites/14897.html' $n[0] $n[1] }

# 第 5~10 轮：同一公开页公布的备用域名序列，用来验证“换域名后同一接口路径是否仍成立”。
$backupRounds = @(
    @(5, @(
            @('奥斯卡备用1', 'aosikazy1.com'), @('奥斯卡备用2', 'aosikazy2.com'),
            @('奥斯卡备用3', 'aosikazy3.com'), @('奥斯卡备用4', 'aosikazy4.com'),
            @('奥斯卡备用5', 'aosikazy5.com'), @('奥斯卡备用6', 'aosikazy6.com'),
            @('奥斯卡备用7', 'aosikazy7.com'), @('奥斯卡备用8', 'aosikazy8.com'),
            @('奥斯卡备用9', 'aosikazy9.com'), @('奥斯卡备用10', 'aosikazy10.com'),
            @('金鹰备用B', 'jyzy2.com'), @('金鹰备用C', 'jyzy3.com'),
            @('酷豆备用B', 'kudouzy2.com'), @('酷豆备用C', 'kudouzy3.com'),
            @('酷豆备用D', 'kudouzy4.com')
        )),
    @(6, @(
            @('飞速备用2', 'fszy2.com'), @('飞速备用3', 'fszy3.com'),
            @('飞速备用4', 'fszy4.com'), @('飞速备用5', 'fszy5.com'),
            @('飞速备用6', 'fszy6.com'), @('飞速备用7', 'fszy7.com'),
            @('飞速备用8', 'fszy8.com'), @('飞速备用9', 'fszy9.com'),
            @('飞速备用10', 'fszy10.com'), @('量子备用2', 'lzizy2.com'),
            @('量子备用3', 'lzizy3.com'), @('量子备用4', 'lzizy4.com'),
            @('量子备用5', 'lzizy5.com'), @('量子备用6', 'lzizy6.com'),
            @('量子备用7', 'lzizy7.com')
        )),
    @(7, @(
            @('无尽备用2', 'wuzy2.com'), @('无尽备用3', 'wuzy3.com'),
            @('无尽备用4', 'wuzy4.com'), @('无尽备用5', 'wuzy5.com'),
            @('无尽备用6', 'wuzy6.com'), @('无尽备用7', 'wuzy7.com'),
            @('无尽备用8', 'wuzy8.com'), @('无尽备用9', 'wuzy9.com'),
            @('无尽备用C', 'wujinzy.net'), @('无尽备用D', 'wujinzy.co'),
            @('无尽备用E', 'wujinzy.cc'), @('优质资源2', '1080zyk2.com'),
            @('优质资源3', '1080zyk3.com'), @('优质资源4', '1080zyk4.com'),
            @('优质资源5', '1080zyk5.com')
        )),
    @(8, @(
            @('U酷备用1', 'ukuzy1.com'), @('U酷备用2', 'ukuzy2.com'),
            @('U酷备用3', 'ukuzy3.com'), @('U酷备用4', 'ukuzy4.com'),
            @('U酷备用5', 'ukuzy5.com'), @('U酷备用6', 'ukuzy6.com'),
            @('U酷备用7', 'ukuzy7.com'), @('U酷备用8', 'ukuzy8.com'),
            @('U酷备用9', 'ukuzy9.com'), @('闪电备用B', 'shandianzy.net'),
            @('闪电备用C', 'shandianzy.cc'), @('卧龙备用2', 'wlzyw2.com'),
            @('卧龙备用3', 'wlzyw3.com'), @('卧龙备用5', 'wlzyw5.com'),
            @('卧龙备用6', 'wlzyw6.com')
        )),
    @(9, @(
            @('香蕉备用2', 'xiangjiaozy2.com'), @('香蕉备用3', 'xiangjiaozy3.com'),
            @('香蕉备用4', 'xiangjiaozy4.com'), @('香蕉备用5', 'xiangjiaozy5.com'),
            @('香蕉备用6', 'xiangjiaozy6.com'), @('香蕉备用7', 'xiangjiaozy7.com'),
            @('香蕉备用8', 'xiangjiaozy8.com'), @('香蕉备用9', 'xiangjiaozy9.com'),
            @('香蕉备用10', 'xiangjiaozy10.com'), @('天空备用1', 'tkzy1.com'),
            @('天空备用2', 'tkzy2.com'), @('天空备用3', 'tkzy3.com'),
            @('天空备用4', 'tkzy4.com'), @('天空备用5', 'tkzy5.com'),
            @('天空备用6', 'tkzy6.com')
        )),
    @(10, @(
            @('天空备用7', 'tkzy7.com'), @('天空备用8', 'tkzy8.com'),
            @('天空备用9', 'tkzy9.com'), @('天空备用10', 'tkzy10.com'),
            @('天空备用11', 'tkzy11.com'), @('天空备用12', 'tkzy12.com'),
            @('天空备用13', 'tkzy13.com'), @('天空备用14', 'tkzy14.com'),
            @('天空备用15', 'tkzy15.com'), @('天空备用16', 'tkzy16.com'),
            @('天空备用17', 'tkzy17.com'), @('天空备用18', 'tkzy18.com'),
            @('天空备用19', 'tkzy19.com'), @('天空备用20', 'tkzy20.com'),
            @('天空备用21', 'tkzy21.com')
        ))
)
foreach ($round in $backupRounds) {
    $index = [int]$round[0]
    foreach ($n in $round[1]) {
        Add-Node $index 'openi.cn/favorites/14897.html' $n[0] ("https://{0}/api.php/provide/vod/" -f $n[1])
    }
}

# 输出前按轮次排序，并统计每轮数量，确保每轮不少于 15 个。
$sorted = $script:nodes | Sort-Object round, name
$roundStats = $sorted | Group-Object round | ForEach-Object {
    [pscustomobject]@{ round = [int]$_.Name; count = $_.Count }
}
$short = @($roundStats | Where-Object { $_.count -lt 15 })
if ($short.Count -gt 0) {
    $roundStats | Format-Table -AutoSize | Out-String | Write-Output
    throw ('存在少于 15 个候选节点的轮次：' + ($short | ForEach-Object { "第$($_.round)轮=$($_.count)" }) -join '，')
}

$document = [pscustomobject]@{
    generatedAt = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ssK')
    note = '公开网络收集的影视站点/采集接口候选；不含影片标题与播放地址。'
    rounds = $roundStats
    nodes = $sorted
}
$document | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $outFile -Encoding utf8
"写入 $($sorted.Count) 个节点：$outFile"
$roundStats | Format-Table -AutoSize
