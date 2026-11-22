# MusicVideoPlayer 只读代码审查报告

## 1. 审查基线

- 项目根：`F:\QTDocuments\MusicVideoPlayer`
- 分支：`master`
- 当前 HEAD：`d54657b 补充下载路径归属端到端回归`
- 工作区：干净（`git status --short` 无输出）
- 审查方式：只读代码核查，未修改源码，未新增 Git 提交，未重新编译
- 审查范围：当前 HEAD 全部核心模块，重点为 `571d015..d54657b` 的下载、网络、代理、播放列表、设置与搜索源改动
- 当前构建：`build\Desktop_Qt_6_10_1_MinGW_64_bit_Debug`
- 现有构建测试：补齐 `D:\Qt\Tools\mingw1310_64\bin` 与 `D:\Qt\6.10.1\mingw_64\bin` 后，`ctest` 4/4 通过，总 9.00 秒
  - `mvp_core_logic` 2.09s
  - `mvp_integration` 4.98s
  - `mvp_storage` 0.27s
  - `mvp_ui_logic` 1.52s
- 环境限制：无 `cppcheck`、`clang-tidy`、`clang-analyzer`、`clazy-standalone`；本轮结论全部来自源码审查与现有测试，不依赖第三方静态分析
- 未执行项：未重新编译；未用真实多码率 HLS、真实恶意播放列表、真实跨 host 302 做端到端复验

> 说明：第一次直接运行 `ctest` 返回 `0xc0000135`，原因是测试进程缺少 Qt/MinGW DLL，不是测试失败；补 PATH 后 4/4 通过。

## 2. 结论摘要

当前项目在“本地文件 / 可信源 / 正常网络环境”下可运行，现有回归测试通过；但作为“可加载第三方搜索源、可播放远程 HLS”的联网播放器，仍有 3 个 P1 级安全边界问题必须先修：

1. `HttpClient::get()` / `postJson()` 使用 Qt 默认自动重定向，未走项目已有的跨 host 凭据裁剪；配置型源站的 Cookie / Authorization 可能被 302 带到第三方。
2. 远程 HLS 播放列表中的非 HTTP URI、未加引号 URI、`#EXT-X-BASE` 会原样交给播放器，绕过 `StreamProxy`，可让 FFmpeg 打开本机文件或 UNC/SMB 路径。
3. 远程搜索源可控的歌词 / 封面 `file://` / UNC 地址会被本地同步读取，造成本机文件读取、SMB/NTLM 认证泄漏和 UI 线程卡死。

P2 主要集中在：`sourceOrigin` / Referer / 直链明文落盘与跨 host 外发、日志漏脱敏 `apiKey`、HLS 下载短写被当成功、HLS 播放列表拉取无接收上限、设置保存失败不回滚、提交改名冲突后的“旧文件误判完成”、签名 HLS 的 `octet-stream` / `text/plain` 识别与 Content-Type 透传、以及自定义会话头跨 host 保留。

P3 主要是资源回收、状态一致性、UI 空转、损坏状态文件恢复和少量低概率数据归属问题，不阻断当前使用，但建议排期清理。

**总体建议**：在修复 P1 前，不应把该版本作为“可加载不可信第三方源”的联网版本发布；只播放本地文件或完全可信源时风险可控。

## 3. P1 问题

### P1-1 `HttpClient::get()` / `postJson()` 自动重定向绕过凭据裁剪

**位置**

- `src/net/HttpClient.cpp:136-145`：`get()` 直接 `m_nam->get(request)`
- `src/net/HttpClient.cpp:154-165`：`postJson()` 直接 `m_nam->post(request, body)`
- `src/net/HttpClient.cpp:236-251`：只有 `fetchInternal()` 设置了 `ManualRedirectPolicy`
- `src/net/HttpClient.cpp:288`：只有 `fetchInternal()` 在跳转时调用 `headersForRedirect()`
- `src/net/ConfigSearchProvider.cpp:105-127`：搜索 GET/POST 带 `Cookie` / 自定义头 / `Authorization`
- `src/net/ConfigSearchProvider.cpp:376-383`：详情 GET 同样带凭据

**触发条件**

配置型搜索源的搜索 URL 或详情 URL 返回 301/302/303/307/308，且 `Location` 指向另一个 host。`QNetworkAccessManager` 默认策略是 `NoLessSafeRedirectPolicy`，会自动跟随；项目只在 `fetchInternal()` 里做了手动跳转和 `HeaderPolicy::sanitizedForTarget()`，`get()` / `postJson()` 没有。

**影响**

- 源站的 `Cookie`、`Authorization`、`X-Api-Key`、`X-Auth-Token` 等可能被自动带到第三方 host。
- Qt 官方未解决 bug `QTBUG-86385`（“QNetworkAccessManager will pass Authorization header to a different origin”）明确说明跨 origin 时 `Authorization` 不会被移除；项目不能假设 Qt 默认策略会替自己裁剪。
- 这直接削弱了上一轮“源站凭据作用域”加固的效果；`fetch()` 的测试通过，不代表 `get()` / `post()` 安全。

**复现建议**

用一个受控 HTTP 服务作为源站：第一次请求返回 `302 Location: http://127.0.0.1:<other-port>/collect`，第二个服务记录请求头。配置源使用 `GET` 或 `POST`，带 `Cookie: a=b` 和 `Authorization: Bearer x`。当前实现第二个服务应能收到这些头；`fetch()` 的同类测试则会被剥离。

**修复方向**

- 对 `get()` / `postJson()` 也强制 `ManualRedirectPolicy`，复用 `headersForRedirect()` 手动跟随；或禁用自动重定向并把 3xx 作为明确错误返回。
- 若支持 POST 307/308，需要明确是否保留 body；至少不能把凭据跨 host 带过去。
- 补测试：`get` 跨 host 302 剥离 `Cookie` / `Authorization`；同 host 301 保留；`postJson` 307 跨 host 剥离；https→http 降级剥离。

### P1-2 远程 HLS 非 HTTP URI / 未加引号 URI / `#EXT-X-BASE` 绕过代理

**位置**

- `src/stream/HlsPlaylist.cpp:141-151`：分片 URI 只把 `http/https` 映射到本地代理，其它 scheme 直接 `return raw`
- `src/stream/HlsPlaylist.cpp:176`：`URI="([^"]*)"` 只匹配双引号属性
- `src/stream/HlsPlaylist.cpp:187-195`：`#EXT-X-BASE` 只更新内部 `base`，该行仍原样输出
- `src/stream/StreamProxy.cpp:1362-1381`：改写后的播放列表原样回给 `QMediaPlayer` / FFmpeg
- `src/stream/HlsPlaylist.cpp:60-64, 87`：`parse()` 只把非 HTTP 标记为 `hasNonHttp` 并关闭预取，不拒绝播放列表

**触发条件**

远程 m3u8 中写入：

- `file:///C:/Users/.../secret.mp4`
- `file://server/share/...`
- `smb://...`
- `data:...`
- `#EXT-X-KEY:URI=file:///...`（未加引号）
- `#EXT-X-MAP:URI=file:///...`（未加引号）
- `#EXT-X-BASE:file:///C:/`

播放该 HLS 时，`StreamProxy` 只负责把 HTTP 分片改写成 `127.0.0.1` token URL；非 HTTP 值保持原样，FFmpeg 会收到并尝试解析。是否最终打开取决于 Qt FFmpeg 后端的协议白名单，但项目自身没有拒绝，不能把安全边界交给下层。

**影响**

- 本机文件读取：如果目标文件是合法媒体，可能被播放；即使不是媒体，错误行为也可用于探测文件存在/类型。
- UNC/SMB：Windows 上访问 `file://server/share/...` 可能触发 SMB/NTLM 认证，泄露本机凭据或触发横向认证。
- 未加引号的 `URI=` 属性同样绕过 `uriAttribute` 正则；`#EXT-X-BASE` 行原样输出，后续相对分片会被解析成 `file://`。
- 这与 `StreamProxy` 的“只允许 HTTP(S) 上游”假设冲突，是实际播放边界，不只是预取问题。

**复现建议**

本地起 HTTP 服务返回一个 m3u8：媒体分片写 `file:///C:/Windows/win.ini`，或写 `file://127.0.0.1/<有效媒体>`；通过搜索源以在线 HLS 条目播放，观察代理日志和 FFmpeg 请求。再加未加引号的 `#EXT-X-KEY:URI=file:///...` 与 `#EXT-X-BASE:file:///C:/` 各一组。

**修复方向**

- 播放列表重写阶段对分片、`EXT-X-KEY`、`EXT-X-MAP`、`EXT-X-PART`、`EXT-X-MEDIA`、`EXT-X-BASE` 统一校验：只允许 `http/https`；非 HTTP 直接判定播放列表不可用并给出明确错误。
- `URI=` 属性解析要支持带引号/不带引号，不能用只匹配双引号的正则。
- 对 `#EXT-X-BASE` 的非 HTTP 值直接拒绝，不能只更新内部 base。
- 补测试：`file://`、`\\server\share`、`smb://`、`data:`、未加引号 `URI=`、`#EXT-X-BASE:file:///`。

### P1-3 远程可控歌词 / 封面 `file://` / UNC 本地读取

**位置**

- `src/net/ConfigMapper.cpp:172-182`：`normalizeUrl()` 不限制 scheme，保留 `file://` 绝对地址
- `src/net/ConfigSearchProvider.cpp:241-256`：远程 JSON 的 `lyricUrl` / `posterUrl` 直接写入 `MediaItem`
- `src/lyrics/LyricsService.cpp:52-53`：`url.isLocalFile()` 直接 `loadLocalFile()`
- `src/lyrics/LyricsService.cpp:70-76`：`QFile::readAll()`，无大小上限
- `src/ui/MusicVisualWidget.cpp:65-68`：`QPixmap loaded(item.posterUrl.toLocalFile())`，UI 线程同步加载

**触发条件**

第三方配置源返回：

```json
{
  "lyricUrl": "file:///C:/Users/xxx/secret.txt",
  "posterUrl": "file://server/share/secret.jpg"
}
```

用户播放该条目时，歌词服务读取本机文本，封面控件同步读取本机/UNC 图片。

**影响**

- 任意本机文件读取：歌词内容会进入解析器并显示；封面会尝试解码。
- UNC/SMB 触发认证，可能泄露 NTLM 凭据。
- 超大文件在 UI 线程 `readAll()` / `QPixmap` 同步加载，可造成卡死或内存峰值。
- 这不是“本地媒体条目使用本地歌词”的正常场景；问题在于**远程源可以控制 URL**。

**修复方向**

- 歌词/封面只允许 `http/https`；只有“本地文件条目”才允许 `file://`，并且只允许用户本地选择/同目录相对路径，禁止 UNC。
- 对本地歌词/封面加文件大小上限，并改为异步读取或至少不在 UI 线程同步 `readAll()`。
- 在 `ConfigMapper::normalizeUrl()` 或 `MediaItem` 校验层做 scheme 白名单，不要依赖下游各自判断。
- 补测试：远程配置源返回 `file://` / `\\server` / `smb://` 歌词与封面时必须被拒绝或降级为空。

## 4. P2 问题

### P2-1 源 URL、`sourceOrigin`、Referer 与直链明文落盘 / 跨 host 外发

**位置**

- `src/net/SourceConfig.cpp:173`：`urlTemplate` 明文写入 `sources.json`
- `src/net/SourceConfig.cpp:195-203`：`detailUrlTemplate` 明文
- `src/download/DownloadJob.cpp:76`：媒体直链 `url` 明文写入 `downloads.json`
- `src/media/MediaItem.h:244-256`：`MediaItem.url`、`sourceOrigin` 明文写入 `playlists.json`
- `src/net/ConfigSearchProvider.cpp:246`：`sourceOrigin = m_originUrl`（完整搜索 URL）
- `src/util/HeaderPolicy.h:68-69`：`Referer` 使用完整 `origin.toString()`，包含 query
- `src/util/HeaderPolicy.h:99-106`：跨 host 只剥敏感头，不剥 Referer

**影响**

- API key 若放在搜索 URL query（`?apiKey=...`），会同时进入 `sources.json`、`playlists.json`、`downloads.json`，并作为 `Referer` 发给每个分片/CDN。
- 签名直链 token 会长期留在 `downloads.json`，即使用户删除任务记录，历史记录仍可能保留。
- 跨 host CDN 即使拿不到 Cookie/Authorization，也能拿到完整源站搜索 URL 作为 Referer，造成隐私/凭据泄漏。
- 这与“源站凭据作用域”加固方向不一致：敏感头被保护了，但 URL query 和 Referer 没有。

**修复方向**

- `sourceOrigin` 只保存 scheme + host + port（必要时加 path），不保存 query / fragment。
- `Referer` 使用去掉 query / userInfo 的源站 URL；若站点必须完整 Referer，至少显式配置开关并提示风险。
- 源配置 URL、下载直链、播放列表 URL 中的敏感 query 做 DPAPI 或单独加密；至少在持久化前剥离已知敏感参数。
- 补测试：`sourceOrigin` / `Referer` 不含 query；`downloads.json` 不含明文 `apiKey`。

### P2-2 日志脱敏漏掉 `apiKey` / `apikey` / `api_key` / `access_key`

**位置**

- `src/net/HttpClient.cpp:396-419`：敏感 query 键集合
- `src/net/HttpClient.cpp:140`、`158`、`255`、`287`：请求/重定向日志
- `src/net/ConfigSearchProvider.cpp:110`：搜索 URL 日志

**问题**

`redactUrl()` 只对 `sign`、`token`、`secret`、`auth`、`password`、`expire`、`nonce`、`session`、`credential`、`vkey`、`vuutv`、`hdnts`、`hash`、`md5`、`sha1`、`sha256`、`sig`、精确 `key`、`timestamp` 打码；不包含 `apiKey` / `apikey` / `api_key` / `access_key`。因此 `?apiKey=...` 会以明文进入 `MusicVideoPlayer.log`。

**修复方向**

- 对包含 `key` / `token` / `secret` / `sign` / `auth` 的 query 键统一做大小写不敏感打码；或维护更完整的敏感键集合。
- 补测试：`apiKey`、`apikey`、`api_key`、`access_key` 均被打码。

### P2-3 `DownloadTask::writeReplyData()` 把短写当成功

**位置**

- `src/download/DownloadTask.cpp:815-820`

```cpp
if (!m_file->write(chunk)) { ... }
m_bytesWritten += chunk.size();
m_segmentBytes += chunk.size();
```

**问题**

`QFile::write()` 返回实际写入字节数，不是 bool。磁盘满、网络盘、杀毒软件占用、配额限制时可能返回小于 `chunk.size()` 的正数；代码仍按整块累加 `m_bytesWritten` / `m_segmentBytes`，最终文件缺字节，HLS 分片长度校验也可能通过。

另外：

- `commitFile()` 的 `m_file->flush()` / `close()` 返回值被忽略（`src/download/DownloadTask.cpp:876-877`）。
- 短写不会触发 `fail()`，也不会重试。

**修复方向**

- 比较 `written == chunk.size()`；短写视为失败或循环补齐。
- 检查 `flush()` / `close()` 返回值。
- 为文件写入增加可注入的故障测试（可用测试专用 QIODevice 或磁盘配额环境）。

### P2-4 HLS 播放列表拉取没有接收上限

**位置**

- `src/download/DownloadTask.cpp:401-437`：`startPlaylistFetch()` 使用 `m_http->stream()`，`maxBytes = 0`
- `src/net/HttpClient.cpp:185-190`：`stream()` 固定 `maxBytes=0`
- `src/download/DownloadTask.cpp:332-351`：直到 `finished` 才 `readAllLimited` 检查 4 MB

**问题**

已知 HLS 任务在 `startRequest()` 直接走 `startPlaylistFetch()`（`DownloadTask.cpp:193-196`），master variant 和嵌套子列表也走这里。异常大的 m3u8 会先被 `QNetworkReply` 完整缓冲，4 MB 检查发生在缓冲之后，不能防止内存峰值。

`startDirectReply()` 的播放列表分支已经调用 `HttpClient::limitReplySize()`（`DownloadTask.cpp:256-258`），但 `startPlaylistFetch()` 没有，属于路径不一致。

**修复方向**

- `startPlaylistFetch()` 在拿到 reply 后立即 `HttpClient::limitReplySize(reply, NetLimits::kPlaylistBytes)`，或改用带 `maxBytes` 的 `get()`。
- 补测试：超大 m3u8 在接收阶段被中止，而不是先读满。

### P2-5 `SettingsDialog::accept()` 部分保存失败不回滚

**位置**

- `src/ui/SettingsDialog.cpp:302-314`：先 `replaceAll(m_stagedSources)` 写 `sources.json`
- `src/ui/SettingsDialog.cpp:316-321`：再写主题、字体、窗口、音量、搜索条数、下载上限
- `src/ui/SettingsDialog.cpp:323-333`：最后校验下载目录；`mkpath` 失败直接 `return`

**问题**

下载目录创建失败时，前面的源配置和 AppSettings 已经写入。用户随后点“取消”不会回滚这些改动；界面提示“保存失败”，但实际有一部分已生效，违反“取消整体回滚”的承诺。

**修复方向**

- 先校验下载目录，再一次性提交；或保存旧值，失败时恢复。
- 源配置写入应与其他设置保持同一事务语义：要么全部成功，要么全部不生效。
- 补测试：下载目录不可写时，源配置与主题/音量均不变化。

### P2-6 提交改名冲突失败后，重试可能把旧目标文件当成本任务完成

**位置**

- `src/download/DownloadTask.cpp:878-895`：`const QString temp = partialPath()` 在 `m_filePath = finalPath` 之前计算
- `src/download/DownloadTask.cpp:892-894`：`rename()` 失败调用 `fail()`
- `src/download/DownloadTask.cpp:919-928`：`fail()` 用新的 `m_filePath` 重新算 `partialPath()`，旧临时文件未被删
- `src/download/DownloadManager.cpp:442-452`：`startJob()` 见到 `job.filePath` 存在且 size>0 直接 `finishJob(..., Completed, ...)`

**问题**

下载期间目标路径被占用，`m_pathResolver` 返回新名字 `target (1).mp4`，`m_filePath` 改为新名字；`rename(temp, new)` 失败后，`fail()` 计算的临时路径变成 `target (1).mp4.mvpdownload`，而真正的临时文件仍是 `target.mp4.mvpdownload`，旧临时文件残留。管理器里的 `job.filePath` 仍可能是旧目标 `target.mp4`；用户重试时，旧目标文件存在且非空，`startJob()` 直接判定“已完成”，把冲突的旧文件当成自己的下载结果。

**影响**

- 下载记录指向错误文件，用户以为下载成功。
- 旧临时文件残留，无法续传。
- 触发需要“提交改名失败 + 目标路径已被占用 + 用户重试”，概率不高，但属于数据归属错误，不应按纯 UI 问题处理。

**修复方向**

- 在 `commitFile()` 中先保存旧临时路径，`fail()` 使用显式临时路径而不是重新计算。
- 给任务增加 `committed` 标记；`startJob()` 只有在本次任务确实写过临时文件或状态明确为 Completed 时才把目标文件视为完成。
- 补测试：强制 `rename` 失败后重试，不得把旧目标文件报告为完成。

### P2-7 签名 HLS 以 `application/octet-stream` / `text/plain` 返回时识别与 Content-Type 透传问题

**位置**

- `src/stream/StreamProxy.cpp:81-99`：`looksLikeMedia()` 接受 `octet-stream`
- `src/stream/StreamProxy.cpp:101-111`：`isPlaylistResponse()` 只认 `text/plain` 或 URL 元数据
- `src/stream/StreamProxy.cpp:1366-1373`：改写后的播放列表优先透传上游 `Content-Type`
- `src/download/DownloadTask.cpp:1005-1016`：`isPlaylistReply()` 同样只认 `text/plain`
- `src/download/DownloadTask.cpp:1019-1028`：`isMediaContentType()` 接受 `octet-stream`

**问题**

- 地址没有 `.m3u8` 后缀、上游返回 `application/octet-stream` 的签名 HLS，会被 `looksLikeMedia()` 当媒体透传给 FFmpeg，播放失败。
- 下载端 `isPlaylistReply()` 不认 `octet-stream`，会把 m3u8 正文写成媒体文件。
- 即使代理识别为播放列表，`attachPlaylist()` 仍把上游 `text/plain` / `octet-stream` 原样回给播放器；本地 URL 是 `/stream/<token>/`，没有 `.m3u8` 后缀，FFmpeg 可能无法选择 HLS demuxer。

**修复方向**

- 对播放列表响应强制 `Content-Type: application/vnd.apple.mpegurl`，不要透传上游 `text/plain` / `octet-stream`。
- 在代理与下载端都增加正文首包 `#EXTM3U` 嗅探；对 `octet-stream` / 空类型做小缓冲探测。
- 补测试：无 `.m3u8` 后缀、`octet-stream` 的签名 HLS 能播放/下载；代理响应头为 `application/vnd.apple.mpegurl`。

### P2-8 自定义会话头跨 host 保留、大小写与端口边界

**位置**

- `src/util/HeaderPolicy.h:14-22`：敏感头只覆盖 `Cookie` / `Authorization` / `Proxy-Authorization` / `X-Api-Key` / `X-Auth-Token`
- `src/util/HeaderPolicy.h:36-47`：`Referer` / `Origin` 精确大小写查找
- `src/util/HeaderPolicy.h:68-69`：`withSourceHeaders` 精确大小写判断
- `src/util/HeaderPolicy.h:99`：`crossHost` 只比较 host，不比较端口

**问题**

- 源配置使用 `X-Source-Session` 等自定义会话头时，跨 host 会保留；现有集成测试甚至明确断言保留。
- 配置里写小写 `referer` / `origin` 时，`originFromHeaders()` / `withSourceHeaders()` 可能无法识别源站，导致 fail-closed 剥掉 Cookie，或补出重复头。
- 同 host 不同端口被当作同源，敏感头会保留到另一个端口服务。

**修复方向**

- 允许源配置声明“敏感头”集合，或默认把所有非标准头都按同源处理。
- 头名查找统一大小写不敏感。
- `crossHost` 比较 host + 有效端口；https→http 降级继续剥离。
- 补测试：`X-Source-Session` 跨 host 剥离；小写 `referer` 能识别；同 host 不同端口剥离。

### P2-9 `StreamTokenRegistry::registerFor()` 没有显式 origin，只有 Cookie 的源可能被 fail-closed 剥掉

**位置**

- `src/stream/StreamTokenRegistry.cpp:65`：`sanitizedForTarget(upstream, headers)` 未传 origin
- `src/util/HeaderPolicy.h:90-97`：无 origin 时从 `Referer` / `Origin` 推断；推断不到就剥敏感头

**问题**

`ConfigSearchProvider` 会补 `Referer`，多数搜索源不受影响；但 `ExternalSearchProvider` 的 `urlTemplate` 为空/无 `webUrl` 时可能没有 `Referer`，`StreamProxy` / `PlaybackController` 也没有把 `sourceOrigin` 显式传给 token 注册。此时只有 `Cookie` 的源会被当作“无法判断源站”，Cookie 被剥掉，播放/下载鉴权失败。

**修复方向**

- `StreamTokenRegistry::registerFor()` 增加显式 `origin` 参数，由 `StreamProxy` / `PlaybackController` 传入条目的 `sourceOrigin`。
- 补测试：只有 Cookie、无 Referer/Origin 的源，token 仍保留 Cookie。

### P2-10 `StreamProxy` 令牌允许同 host 任意路径

**位置**

- `src/stream/StreamTokenRegistry.cpp:60`：token 是 64 位随机数
- `src/stream/StreamProxy.cpp:311-343`：只校验 scheme/host/port/userInfo，不校验路径是否仍在 token 注册 URL 的目录作用域

**问题**

token 是秘密，远程播放列表看不到改写后的 token URL，因此远程直接利用需要先获知 token；更现实的威胁是本地恶意程序/本地页面拿到 token 后，借受害者 Cookie 请求同 host 任意端点。属于加固项，不应按远程 P1 处理。

**修复方向**

- 限制 suffix 到注册 URL 的目录前缀；或对 query 变更做签名校验。
- 不需要改 token 长度即可显著降低本地 CSRF/SSRF 面。

## 5. P3 问题清单

| # | 问题 | 位置 | 说明 |
|---|---|---|---|
| 1 | `startJob()` 把已存在的最终文件直接视为完成 | `DownloadManager.cpp:443-452` | HLS 分支 remux 成功后还会删掉用户后来自建的同名 TS；概率低但有数据丢失面 |
| 2 | `downloads.json` 损坏不备份、直接清空历史 | `DownloadManager.cpp:295-299` | 临时文件仍在，但续传点/历史丢失 |
| 3 | 析构不 `saveNow()`，`closeEvent` 只在有活动下载时 `prepareForExit()` | `DownloadManager.cpp`、`MainWindow.cpp:332-358` | 最近完成/暂停状态可能未落盘 |
| 4 | `isPathReserved()` 把 Completed/Failed 记录也当占用 | `DownloadManager.cpp` | 用户删最终文件后重新下载仍得到 `(1)` 后缀 |
| 5 | `.mvpremux` 清理缺口 | `MediaRemux.cpp:42-54`、`DownloadManager::removeJob()` | 退出/移除任务可能残留大临时文件 |
| 6 | 重复任务状态消息与实际路径不一致 | `DownloadController.cpp:40-80` | 第二次下载同一条目显示一个不存在的 `(1)` 文件名 |
| 7 | `PlaylistStore` 损坏备份无数量上限、读取无大小上限、每次启动备份两次 | `PlaylistStore.cpp:58-108` | `load(Music)` + `load(Video)` 各备份一次；提示文案“主/备用”不准确 |
| 8 | `SourceRegistry::load()` 不去重 `sourceId`、无文件大小上限 | `SourceRegistry.cpp:195-307` | 重复 id 导致凭据/配置命中不确定 |
| 9 | 源导入可覆盖内置源 ID / 自定义源可声明 `builtin` | `SourceManagerDialog.cpp:915-978`、`SourceConfig.cpp:213` | 可覆盖内置实现或绕过删除限制 |
| 10 | 导出源配置的敏感头是 DPAPI 密文，跨机器导入失效 | `SourceConfig.cpp:178-190` | 表现为导入成功但 Cookie/Authorization 不可用 |
| 11 | 日志轮转先删旧备份再 rename | `main.cpp:33-40` | rename 失败会丢旧 `.1` |
| 12 | `postJson()` 的 `Content-Type` 未二次校验 | `HttpClient.cpp:160-164` | 配置值含 CRLF 时循环跳过，但 `postJson` 仍设置原值 |
| 13 | `limitReplySize()` 依赖调用方未提前消费正文 | `HttpClient.cpp:206-224` | 未来流式调用方若先 `readAll()`，`bytesAvailable` 归零，上限可能失效 |
| 14 | `MediaItem::isHlsUrl()` / `PlaylistSniffer` 只按字符串包含 `.m3u8` | `MediaItem.h:166-171`、`PlaylistSniffer.cpp:5-11` | query 里出现 `.m3u8` 的 MP4 会被误判为 HLS |
| 15 | `HlsPlaylist::sameScope()` 目录前缀比较过宽 | `HlsPlaylist.cpp:112-125` | `/videos/2024/` 与 `/videos/2024/other/` 视为同目录，预取可能拿错片 |
| 16 | `PlaylistController::playItem()` 预览搜索条目即持久化 | `PlaylistController.cpp:209-226` | 试听会污染播放列表 |
| 17 | `PlaylistModel::updateItem()` 不做 identity 去重 | `PlaylistModel` | 当前调用方未触发，但接口可制造重复行 |
| 18 | `SettingsController::apply()` 用新默认音量覆盖当前音量 | `SettingsController.cpp:123-136` | 从设置页返回时当前播放音量被改 |
| 19 | `PlaybackUiController::m_userSeeking` 停止/切歌不复位 | `PlaybackUiController.cpp` | 拖动中停止/切歌后进度条不再更新，直到再次拖动 |
| 20 | `SearchCoordinator::cancel()` 发 `searchFinished` 不发 `searchSucceeded` | `SearchCoordinator.cpp:326-352` | 取消后部分结果仍留在 UI |
| 21 | `SearchItemDelegate` 40 ms 定时器在列表可见时空转 | `SearchPanelController.cpp:62-64` | 无滚动标题也每 40 ms 计时 |
| 22 | `MusicVisualWidget` 暂停时仍 33 ms 全窗口重绘、封面每帧缩放 | `MusicVisualWidget.cpp:46-53, 112-125, 224-258` | 音乐区常驻时暂停仍占 CPU/GPU |
| 23 | `PanelOverlayController` 120 ms 边缘检测在最小化/隐藏时仍运行 | `PanelOverlayController.cpp:25-28, 82-124` | 最小化后仍每 120 ms 取光标/算几何 |
| 24 | `VideoSurface::resumeFrames()` 不主动恢复原生视频窗口 | `VideoSurface.cpp:47-59` | 暂停中切回且无新帧时窗口保持隐藏 |
| 25 | `MainWindow::closeEvent()` 播放列表保存失败不提示 | `MainWindow.cpp:332-358` | 磁盘满/目录不可写时直接退出，无法重试 |
| 26 | `StreamProxy` 缓存淘汰在全部条目活跃时失效 | `StreamProxy.cpp:1664-1721` | 最坏可接近 24×128 MB，而非 256 MB |
| 27 | `StreamTokenRegistry` 64 位 token + 无路径作用域 | `StreamTokenRegistry.cpp:60`、`StreamProxy.cpp:311-343` | 见 P2-10；token 足够随机，但本地拿到 token 后可越权同 host |
| 28 | `PlaybackController` 析构在线程 3 秒未退时摘父子关系 | `PlaybackController.cpp:124-163` | 有意取舍，不是 UAF，但极端情况留下线程/代理对象 |
| 29 | `SourceProbe` 的 id 只用 host 生成、`cancel()` 不中止 validator | `SourceProbe.cpp:318-323, 565` | 同 host 多 API 可能同 id；旧 validator 继续到超时 |
| 30 | `DownloadTask` 的 HLS 解析不处理 `#EXT-X-BASE`、未加引号 `MAP`、无 `ENDLIST` | `DownloadTask.cpp` 解析分支 | 相对分片可能按错误 base 解析；fMP4 缺 init 段；live 列表可能保存不完整 |
| 31 | `MediaRemux::cancel()` 先删 `.mvpremux` 再杀进程 | `MediaRemux.cpp:42-54` | Windows 上删除可能失败且无重试 |
| 32 | `AppSettings::playbackMode()` 未 clamp | `AppSettings.cpp` | 消费方 `restore()` 有 `qBound`，但设置值本身可越界 |
| 33 | `StreamProxy` 不限制客户端 HTTP 方法 | `StreamProxy.cpp:274-390` | 本地客户端 POST/PUT 仍获响应，上游实际 GET；代理语义不严格 |
| 34 | `MaccmsWebProvider::encodeMediaUrl()` 不校验 scheme | `MaccmsWebProvider.cpp:94-102` | 远程 HTML 可产出 `file://server/share/x.mp4`；正常代理会 502，但回退/歌词/封面仍有边界问题 |
| 35 | `SourceConfig` 的 `external` 只校验命令非空 | `SourceConfig.cpp:57-87` | 外部解析器属于本地显式配置信任边界；是否需要白名单/签名为产品问题 |
| 36 | `DownloadManager` 保留最新 100 条完成记录但不校验路径冲突 | `DownloadManager.cpp` | 手工编辑状态文件可产生重复/同路径记录 |

## 6. 已核对、不应再作为缺陷上报的点

以下问题在上一轮或本轮早期曾被怀疑，复核后不成立，或属于有意设计：

- 不存在 `DownloadJob::Status::Remuxing`；HLS 转 MP4 期间任务仍是 `Status::Downloading`。退出后 `startJob()` 会从 `.ts` 重新 `startRemux()`，不会永久卡死。
- `DownloadController::m_dialog` 是 `QPointer<DownloadManagerDialog>`，对话框 `WA_DeleteOnClose` 关闭后自动置空，重开不会 use-after-free。
- HLS 下载写的是 `filePath + ".mvpdownload"`，不是半截 `.ts`；重启会按分片序号续传，不会把半截 TS 当完整文件。
- `DownloadTask::fail()` 先置 `m_done` 再 abort，避免同步 `finished` 二次失败；失败默认保留临时文件。
- `PlaylistController::removeRow` 删除当前行前，`PlaybackController::onPlaylistRowRemoved` 会正确调整索引或停止播放。
- `DownloadManager::saveNow()` 使用 `QSaveFile` 并检查 `commit()`；`shutdownForExit()` 在设置 `m_shuttingDown` 后仍会保存。
- `StreamProxy` 请求头有 16 KB 上限；`attachPlaylist` 已在接收阶段调用 `HttpClient::limitReplySize()`。
- `MaccmsWebProvider` 在构造函数 `Q_UNUSED(http)` 后使用独立 `HttpClient`，源码注释说明是为了独立 `abortAll()`，属于有意设计，不按“统一 Cookie 会话失效”报。
- `FullscreenController::exitFullScreen()` 未显式 `removeWidget` 不构成缺陷：Qt `QLayout::addChildWidget` 会移除旧布局项。
- `SettingsDialog` 主题/字体没有实时预览连接，取消不会改主题；真正的问题是 `accept()` 的写入顺序/回滚。
- `DownloadManagerDialog::removeSelected()` 先取 `job` 指针再调用 `removeJob(job->id)`，`job->id` 在 vector 变更前求值，未发现 UAF。
- `HttpClient::fetchInternal()` 的手动重定向和跨 host 裁剪是正确的；问题只在 `get()` / `postJson()` 没走这套。
- `SourceProbe` 旧 validator 跨站点污染未观察到：prober 每次新建，旧对象析构断开回调；`cancel()` 只是不中止 validator，属于资源浪费。

## 7. 验证结果与测试缺口

### 已验证

- 当前 HEAD `d54657b`，工作区干净。
- 现有构建 `ctest` 4/4 通过（补 PATH 后）。
- 关键代码位置已逐项核对：`HlsPlaylist::rewrite()` 非 HTTP 原样返回、`HttpClient::get/postJson` 无手动重定向、`LyricsService` 本地 `readAll()`、`DownloadTask` 短写判断、`SettingsDialog` 先写后校验、`StreamProxy` 透传 Content-Type。
- 现有测试中下载路径归属、取消保留最终文件、同名分配等新增用例通过。

### 未验证 / 建议补测

- `get()` / `postJson()` 跨 host 302 的凭据泄漏：没有测试；建议用双 HTTP 服务做端到端断言。
- HLS 非 HTTP / 未加引号 URI / `#EXT-X-BASE`：没有测试；`hlsPlaylistRewrite...` 只测 HTTP/带引号 URI。
- 歌词/封面 `file://` / UNC：没有测试。
- HLS 播放列表接收上限：没有测试。
- 下载短写 / `flush` / `close` 失败：没有故障注入测试。
- 设置下载目录失败回滚：没有测试。
- 提交改名冲突后重试：没有测试。
- 签名 HLS `octet-stream` / `text/plain` 播放与下载：没有真实站点复验。
- 真实多码率 HLS master、`BYTERANGE`、`EXT-X-KEY`、`EXT-X-MAP`：没有真实站点复验。
- UI 层 `MainWindow` 全部信号连接、`ThemeManager` 全窗口样式：本轮未逐一深入。
- `StreamProxy::cacheStats()` 是否有跨线程调用：当前 `src` 内未发现调用方，未验证外部调用。

## 8. 建议修复顺序

1. **P1-1**：先修 `get()` / `postJson()` 自动重定向，补跨 host 凭据测试。这是最直接的凭据泄漏入口。
2. **P1-2 / P1-3**：统一“远程可控 URI 的文件/UNC 边界”，在 `HlsPlaylist`、`ConfigMapper`、`LyricsService`、`MusicVisualWidget` 做 scheme 白名单和大小上限。
3. **P2-1 / P2-2**：收敛 `sourceOrigin` / Referer / 直链持久化与日志脱敏，避免 API key 落盘和外发。
4. **P2-3 / P2-4 / P2-6**：修下载完整性（短写、播放列表上限、提交冲突归属），补故障注入与端到端测试。
5. **P2-5 / P2-7 / P2-8 / P2-9 / P2-10**：设置事务、HLS 识别/Content-Type、头策略、token origin/path 作用域。
6. **P3**：按表清理，优先处理状态文件损坏恢复、`.mvpremux` 残留、UI 定时器空转。

## 9. 交付说明

- 本报告是只读审查产物，未修改任何源码。
- 新增文件仅本报告；未提交 Git。
- 现有构建测试结果仅代表当前 HEAD 的回归测试通过，不代表上述 P1/P2 已被覆盖。
