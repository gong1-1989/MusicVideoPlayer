# 音乐网站/API 节点采集与验证报告

- 日期：2026-11-23
- 项目：MusicVideoPlayer
- 数据：`music-nodes.json`、`music-probe-results.json`
- 脚本：`collect_music_nodes.ps1`、`probe_music_nodes.ps1`
- 代码：`src/net/SourceProbe.cpp`、`src/net/SourceProbe.h`
- 规则表：`docs/source-probe/探查规则表.md`

## 1. 口径

与影视探查保持一致：一个“节点”是一条接口 URL，不是站点首页；同一站点不同接口路径算不同节点。共 10 轮、每轮 15 个，合计 150 个候选。候选里既有带 `{keyword}` 的搜索接口，也有 AzuraCast/Xiph/网页入口这类 catalog-only、XML 或 HTML 节点，用来验证“能不能搜”而不是只验证“能不能列目录”。

统计口径拆成三层：

- **候选节点 150**：脚本实际跑过的 150 条接口；
- **唯一接口 148**：按接口 URL 归并后；两组只是同一路径换 demo 账号/错误密码，不重复计入源；
- **唯一站点 74**：按 host 归并，不含端口差异。进 `SourceRegistry` 只按唯一接口 URL，不按站点首页批量入源。

验证是逐节点真实 HTTP 请求，不靠文档推断：

1. 首轮用 `love` 搜 10 条；
2. 二次用 `jazz` 搜 10 条；
3. 两轮标题集合完全相同，判定“接口忽略关键词”，归为 catalog-only；
4. 首轮有结果、二次无结果或请求失败，保留首轮（避免网络抖动误杀）；
5. 有结果数组 + 标题字段 + 可播放字段，才归为“可搜索且有直链”。

结果文件只保存状态、数组路径、字段名、计数、耗时，不保存歌曲标题、播放地址或任何账号凭据。候选节点文件也只保留接口形态，公开 demo 的 `u/p` 已替换为 `{user}` / `{password}` 占位。

## 2. 总览

| 指标 | 数量 |
| --- | --- |
| 候选节点（实际请求） | 150 |
| 唯一站点（按 host） | 74 |
| 唯一接口（按 URL，归并账号差异） | 148 |
| HTTP 2xx | 64 |
| 可解析 JSON | 36 |
| 401/403 或认证失败 | 21 |
| 连接失败/DNS/超时 | 39 |
| 返回 HTML/播放器 | 18 |
| 返回 XML | 4 |
| JSON 无结果数组 | 4 |
| 有结果但无直链 | 16 |
| 5xx | 2 |
| 404 | 20 |
| 2xx 空正文 | 9 |
| 非 JSON/XML/HTML | 1 |
| 二次关键词校验通过 | 24 |
| 有 http(s) 直链字段 | 16 |
| 有结果但无直链（catalogOnly 口径） | 16 |

分类关系：

- 24 个可搜索节点中，9 个有直链、15 个只能列目录/无播放字段；
- 16 个有直链节点中，9 个可搜索、7 个是 AzuraCast catalog-only；
- `catalogOnly=16` 是脚本口径“`itemCount>0` 且 `streamField` 为空”，包含 15 个可搜索节点和 1 个 AzuraCast `station requests`；不要把 16 个 AzuraCast catalog 节点重复算进去。

## 3. 每轮统计

格式：测试数/可搜索/仅目录/有直链。

| 轮次 | 测试 | 可搜索 | 仅目录 | 有直链 |
| --- | --- | --- | --- | --- |
| 1 | 15 | 5 | 0 | 5 |
| 2 | 15 | 4 | 4 | 0 |
| 3 | 15 | 0 | 0 | 0 |
| 4 | 15 | 0 | 1 | 7 |
| 5 | 15 | 0 | 0 | 0 |
| 6 | 15 | 4 | 0 | 4 |
| 7 | 15 | 4 | 4 | 0 |
| 8 | 15 | 0 | 0 | 0 |
| 9 | 15 | 4 | 4 | 0 |
| 10 | 15 | 3 | 3 | 0 |

## 4. 真正可搜索且有直链的节点

| 轮次 | 节点 | 接口 | 数组/标题/直链 | 首轮→二次 |
| --- | --- | --- | --- | --- |
| 1 | Audius | `https://api.audius.co/v1/tracks/search?query={keyword}&limit={limit}` | `data` / `title` / `stream.url` | 5→4 |
| 1 | Audius Discovery | `https://discoveryprovider.audius.co/v1/tracks/search?query={keyword}&limit={limit}` | `data` / `title` / `stream.url` | 5→4 |
| 1 | ccMixter | `https://ccmixter.org/api/query?f=json&search={keyword}&limit={limit}` | 根数组 / `upload_name` / `files[0].download_url` | 5→5 |
| 1 | iTunes | `https://itunes.apple.com/search?term={keyword}&media=music&limit={limit}` | `results` / `trackName` / `previewUrl` | 5→5 |
| 1 | Radio Browser DE | `https://de1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}` | 根数组 / `name` / `url_resolved` | 5→5 |
| 6 | Radio Browser byname | `https://de1.api.radio-browser.info/json/stations/byname/{keyword}?limit={limit}` | 根数组 / `name` / `url_resolved` | 5→5 |
| 6 | Radio Browser bytag | `https://de1.api.radio-browser.info/json/stations/bytag/{keyword}?limit={limit}` | 根数组 / `name` / `url_resolved` | 5→5 |
| 6 | Radio Browser DE hidebroken | `.../stations/search?name={keyword}&limit={limit}&hidebroken=true` | 根数组 / `name` / `url_resolved` | 5→5 |
| 6 | Radio Browser DE2 hidebroken | `https://de2.api.radio-browser.info/...` | 根数组 / `name` / `url_resolved` | 5→5 |

这 9 个是本轮唯一“匿名搜索 + 有可请求直链”的组合。iTunes 的 `previewUrl` 是试听片段，不是完整曲目；Radio Browser 是电台流，不是单曲。

## 5. 可搜索但只能列目录/无播放字段

| 轮次 | 节点 | 标题字段 | 首轮→二次 | 原因 |
| --- | --- | --- | --- | --- |
| 2 | Navidrome search3 JSON | `title` | 5→1 | 只有歌曲 id，没有 stream 字段 |
| 2 | Navidrome search2 JSON | `title` | 5→1 | 同上 |
| 2 | Navidrome 只搜歌曲 | `title` | 5→1 | 同上 |
| 2 | Subsonic guest5 search3 | `title` | 1→0 | 二次无结果，按首轮接受；无直链 |
| 7 | AltPress | `title` | 24→24 | WordPress 文章搜索结果，不是音频 |
| 7 | BrooklynVegan | `title` | 24→24 | 同上 |
| 7 | Dancing Astronaut | `title` | 5→5 | 同上 |
| 7 | Rolling Stone | `title` | 5→5 | 同上 |
| 9 | 网易云 cloudsearch | `al.name` | 5→5 | 标题字段嵌套，无 stream |
| 9 | 网易云 web 搜索 | `album.artist.name` | 5→5 | 标题字段嵌套错误案例，无 stream |
| 9 | QQ 音乐 client_search | `songname` | 5→5 | 有歌名，无可用直链 |
| 9 | 酷狗 mobilecdn | `group[0].songname` | 5→5 | 标题嵌套，无 stream |
| 10 | Navidrome albumCount only | `artists[0].name` | 2→0 | 只搜专辑，且把艺术家名当标题 |
| 10 | Navidrome songCount only | `title` | 5→1 | 只搜歌曲，无直链 |
| 10 | Subsonic guest5 search2 | `title` | 1→0 | 无直链 |

关键问题：Navidrome/Subsonic 的搜索结果结构是 `subsonic-response.searchResult3.song`，歌曲对象只有 `id`，没有可直接请求的 URL。只在 JSON 里找 `stream` 字段永远找不到；必须由代码生成 `/rest/stream?id={id}` 模板。

## 6. 只有目录/直链、不支持关键词

| 轮次 | 节点 | 标题/直链 | 结论 |
| --- | --- | --- | --- |
| 4 | AzuraCast `nowplaying` | `name` / `url` | 当前播放，无关键词 |
| 4 | AzuraCast `nowplaying/1` | `name` / `url` | 同上 |
| 4 | AzuraCast `nowplaying/<station>` | `name` / `url` | 同上 |
| 4 | AzuraCast `station/1` | `name` / `url` | 站点信息，无关键词 |
| 4 | AzuraCast `station/1/nowplaying` | `name` / `url` | 同上 |
| 4 | AzuraCast `station/<shortcode>` | `name` / `url` | 同上 |
| 4 | AzuraCast `stations` | `name` / `url` | 站点列表，无关键词 |

这些只能作为 catalog-only 展示，不能内置成搜索源；否则用户搜任何词都会得到同一批当前播放/站点。

另有一个 `AzuraCast station requests`：能返回 19 条点播记录和 `song.title`，但没有可播放字段，也不支持关键词；它计入 `catalogOnly` 的 16 个，但不在这 7 个“有直链、无关键词”的节点里。

## 7. 失败分布与原因

| 类型 | 数量 | 典型节点/原因 |
| --- | --- | --- |
| 连接失败 | 39 | Deezer、Openverse、Mixcloud、部分 Funkwhale/电台镜像 DNS 不通或 16 秒超时 |
| 认证失败 | 21 | Navidrome/Subsonic demo 缺 u/p、Jellyfin/Ampache 缺 API key、Funkwhale 匿名 401、部分 WordPress 403 |
| 404 | 20 | Funkwhale v1/v2、AzuraCast search、Jellyfin `/Search/Hints`、Ampache 路径不存在 |
| HTML | 18 | Jellyfin/Ampache 登录页、音乐站网页、Bandcamp/SoundCloud 页面 |
| 无直链 | 16 | Navidrome/Subsonic、WordPress 音乐媒体、网易云/QQ/酷狗 |
| 空正文 | 9 | 2xx 但无内容 |
| 无数组 | 4 | JSON 是对象/错误结构 |
| XML | 4 | Subsonic 默认 XML；需 `f=json` |
| 5xx | 2 | 上游临时错误 |
| 非 JSON | 1 | 返回纯文本/其他格式 |

结论：

- Funkwhale 的公开实例本轮没有匿名可搜索 JSON；`open.audio`、`demo.funkwhale.audio`、`tanukitunes`、`soultuna`、`funkwhale.duckdns` 均为 401/404/连接失败。
- Jellyfin/Ampache 的 15 个节点没有匿名搜索成功；`/Items`、`/Search/Hints`、`/Audio` 需要 API key 或登录。
- Deezer/Openverse/Mixcloud 在本网络多次超时，不能算“已验证可用”；规则只把它们作为待验证/网络受限节点。
- Radio Browser 的 `nl1/at1/fr1/uk1/us1` DNS 不通，`de1/de2` 可用；内置只用 de 系列，避免硬编码不可靠镜像。
- AzuraCast 的 `/api/station/1/search`、`/api/search` 在公开 demo 没有取得有效搜索结果，不能当搜索源。

## 8. 对 SourceProbe 的落地修改

### 8.1 内置路径

`defaultPaths(Music)` 从旧 5 条改为 16 条，全部带 `{keyword}`：

1. iTunes `/search?term={keyword}&media=music&limit={limit}`
2. Subsonic `/rest/search3?...&f=json`
3. Subsonic `/rest/search2?...&f=json`
4. Subsonic `/rest/search?...&f=json`
5. Funkwhale `/api/v1/tracks/?q=...`
6. Funkwhale `/api/v1/albums/?q=...`
7. Funkwhale `/api/v1/artists/?q=...`
8. Funkwhale `/api/v2/albums?q=...`
9. Radio Browser `/json/stations/search?name=...`
10. Radio Browser `/json/stations/byname/{keyword}`
11. ccMixter `/api/query?f=json&search=...`
12. Jellyfin `/Items?searchTerm=...&IncludeItemTypes=Audio`
13. WordPress `/wp-json/wp/v2/search?search=...`
14. 通用 `/api/search?q=...`
15. 通用 `/search?q=...`
16. 通用 `/ajax/search?keyword=...`

旧音乐路径里的 `/wp-json/wp/v2/posts` 和影视 `wd` 参数被移除；音乐不再使用 `wd`。

### 8.2 关键词参数

`appendProbeQuery` 按音乐接口族补参数：

- Subsonic：`query`、`songCount`、`f=json`；`f=xml` 强制改 `f=json`；
- Radio Browser：`name`、`limit`、`hidebroken=true`；
- ccMixter：`f=json`、`search`、`limit`；
- Funkwhale：`q`、`page_size`；
- Jellyfin：`searchTerm`、`IncludeItemTypes=Audio`、`Limit`；
- WordPress：`search`、`per_page`；
- 通用：`q`；已有 `term/search/query/keyword` 时不再补 `q`，避免 iTunes 同时收到 `term` 和 `q`。

### 8.3 字段识别

`collectSamples` 从“只收顶层标量字段”改为按路径展开嵌套对象/数组，能拿到：

- `files[0].download_url`（ccMixter）
- `stream.url`（Audius）
- `album.name`、`artists[0].name`（Navidrome/网易云）
- `group[0].songname`（酷狗）

标题匹配先顶层精确、再嵌套包含；`name` 不会再被 `artists[0].name` 抢先。播放字段增加 `url_resolved`、`stream.url`、`previewUrl`、`download_url`、`listen_url` 识别，但音乐只接受 http(s) 直链。同一对象同时有 `url` 和 `url_resolved` 时优先 `url_resolved`；电台的 `url` 可能只是跳转地址。真实 Radio Browser 样本里部分流是 `.m3u8`，用户已选音乐时不能仅凭 HLS 后缀改判影视，避免把电台源存成视频源。

### 8.4 Subsonic id→stream 模板

搜索 URL 是 `/rest/search3?u=...&p=...`、结果只有 `id` 时，生成：

```text
@https://host/rest/stream?id={id}&u=...&p=...&v=...&c=...
```

`SourceConfig.fields["streamUrl"]` 支持 `@` 模板，`ConfigMapper` 在运行时用歌曲 `id` 替换 `{id}`。公开 demo 凭据只放在测试里，不写进内置配置。

模板只对 `subsonic-response.searchResult*.song` 生效：`searchResult*.album` 里的 `id` 是专辑 id，拼成 `/rest/stream` 会得到不可播放地址，因此不会生成模板。

### 8.5 试搜与失败提示

- 音乐首轮基准词改为 `love`，二次改为 `jazz`，与真实验证脚本一致；影视仍是 `电影`/`电视剧`。
- 失败提示增加音乐分支：401/403 先说账号/API key，Subsonic `status=failed` 单独识别，XML 提示加 `f=json`，无直链提示用 `/rest/stream` 或 `/Audio/{id}/stream`。
- 音乐总预算从影视的 24 秒提高到 32 秒，自动识别 40 秒；音乐路径更多，但多数 404 很快返回。

### 8.6 内置源与凭据边界

- 不把 150 个候选节点全量写进 `SourceRegistry`；只保留 4 个公开可验证候选：iTunes、Audius、Radio Browser、ccMixter。
- 这 4 个候选全部 `enabled=false`，且 `builtin=false`，作为可选模板，不默认启用，也不阻止用户删除。
- 认证类音乐源（Subsonic/Navidrome、Jellyfin、Ampache）默认不进种子；需要凭据时由用户自己建源并填 Cookie/Authorization 等头。
- `music-nodes.json` 里的公开 demo 账号已脱敏为 `{user}` / `{password}` 占位；真实测试凭据只留在 `probe_music_nodes.ps1` 的测试表，不写进正式配置。
- `HttpClient::redactUrl` 增加对 Subsonic 单字母参数 `u/p/t/s` 和 URL userInfo 的脱敏，避免日志再次泄漏账号或 token。

### 8.7 试听/电台/完整曲目

`previewOnly` 只表示“本次运行临时条目、不写播放列表”，不再借它表示音频内容范围。新增 `MediaItem.streamScope`：

- `preview`：字段名为 `previewUrl`，如 iTunes，列表显示“试听”；
- `radio`：字段名为 `url_resolved`，如 Radio Browser，列表显示“电台”；
- 空：Audius `stream.url`、ccMixter `download_url` 等完整曲目，不强行标“完整”。

该字段随播放列表序列化，重启后仍保留；旧列表没有该字段时按完整曲目处理。

## 9. 真实运行验证

| 验证 | 结果 |
| --- | --- |
| 单元/集成测试 | CTest 4/4 通过 |
| 新增回归 | `streamScope` 试听/电台/完整曲目标签与序列化通过；可选种子默认关闭通过 |
| iTunes 真实探测 | 通过：`love` 11 条、`jazz` 10 条，`trackName` / `previewUrl` |
| Navidrome 真实探测 | 通过：`love` 10 条、`jazz` 1 条，生成 `@.../rest/stream?id={id}`；日志显示 `u=***&p=***` |
| Radio Browser 真实探测 | 通过：`love` 10 条、`jazz` 10 条，根数组 / `name` / `url_resolved`；`.m3u8` 电台仍识别为音乐 |
| 构建 | Qt 6.10.1 MinGW Debug 构建通过 |
| 应用冒烟 | `-platform offscreen` 启动 6 秒仍存活，stderr 为空 |

Navidrome 测试暴露并修复了一个实际缺陷：歌曲对象的 `path` 是本地文件路径（如 `Artist/Album/01.mp3`），曾被当作播放地址，导致不生成 stream 模板。修复后音乐类型只认 http(s) 直链，`path`/`orig_filename` 不再命中。

## 10. 限制与后续

- iTunes `previewUrl` 是试听片段，不是完整曲目；当前只保证“可请求”，不保证时长。
- 中文音乐 API（网易云/QQ/酷狗）只能列目录，没有可直接播放的授权直链；不内置为可搜索源。
- Funkwhale/Jellyfin/Ampache 需要凭据；内置路径只作为“带凭据用户”的候选，不保证匿名可用。
- XML 不解析；Subsonic 必须 `f=json`。
- Deezer/Openverse/Mixcloud 本网络不可达，未纳入已验证可用源；后续网络恢复后可重新跑第 1 轮验证。
- Radio Browser 镜像会变；后续可改为运行时发现 `all.api.radio-browser.info`，避免硬编码 de1/de2。
- 4 个可选音乐种子是 `builtin=false`，已有用户的 `sources.json` 不会自动补入；需要在源管理里新增或复制模板，避免程序擅自改用户已有配置。
- `streamScope` 只按字段名保守识别；如果用户把试听地址填进普通 `streamUrl`，仍会被当作完整曲目，需要源配置明确使用 `previewUrl`/`url_resolved` 字段。

## 11. 复现

```powershell
# 生成 10 轮音乐候选（脚本用 ForEach-Object -Parallel，需要 PowerShell 7）
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\source-probe\collect_music_nodes.ps1

# 真实 HTTP 验证（可只跑第 1 轮）
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\source-probe\probe_music_nodes.ps1 -Rounds '1' -Throttle 4

# 或全量
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\source-probe\probe_music_nodes.ps1 -Throttle 4
```

真实 C++ 回归：

```powershell
$env:MVP_LIVE_PROBE='1'
build\Desktop_Qt_6_10_1_MinGW_64_bit_Debug\mvp_tests.exe sourceProbeLiveMusicSearch
build\Desktop_Qt_6_10_1_MinGW_64_bit_Debug\mvp_tests.exe sourceProbeLiveSubsonicStreamTemplate
build\Desktop_Qt_6_10_1_MinGW_64_bit_Debug\mvp_tests.exe sourceProbeLiveRadioBrowserSearch
```
