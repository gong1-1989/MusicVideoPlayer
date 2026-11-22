# MusicVideoPlayer 第二轮只读复审

## 1. 审查基线

- 项目根：`F:\QTDocuments\MusicVideoPlayer`
- 分支：`master`
- HEAD：`aa792a0 补齐 P3 缓存配额与代理线程回收`
- 工作区：干净
- 本轮未修改任何源码。
- 回归基线：`ctest` 4/4 通过（`mvp_core_logic`、`mvp_integration`、`mvp_storage`、`mvp_ui_logic`，总耗时约 9.54 秒）。
- 复审方式：逐文件阅读，重点核对 P1/P2/P3 修复后的调用链、资源生命周期、持久化路径和网络凭据边界。

## 2. 总体结论

本轮没有发现新的 P0 崩溃或可直接远程利用的漏洞，但发现 **2 个 P1 级数据一致性风险**、**6 个 P2 级问题** 和若干 P3 级边角问题。上一轮 P3 的大部分修复方向正确，但有三处需要继续收紧：

1. “试听不落盘”只做到了不主动 `save()`，退出时仍会整体保存。
2. `StreamProxy` 的新缓存配额只统计“已收到数据”，没有统计“已登记但未下载完的 total”，并发占位可突破 256 MB 配额。
3. 播放列表读取错误和空列表没有区分，存在“读取失败后被空模型覆盖”的路径。

## 3. P1 问题

### P1-1 缺少单实例保护，多个进程会整文件覆盖播放列表/下载状态/源配置

**位置**

- `src/core/PlaylistStore.cpp:150-171`（`saveAll()` 全量写 `playlists.json`）
- `src/net/SourceRegistry.cpp:337-381`（`save()` 全量写 `sources.json`）
- `src/download/DownloadManager.cpp:409-432`（`saveNow()` 全量写 `downloads.json`）
- `src/app/main.cpp`：应用启动流程；全项目未命中 `QLockFile`、`QSharedMemory`、`QLocalServer` 单实例实现。

**触发**

用户双击两次程序，或从任务栏/开始菜单重复启动，两个实例同时运行。

**后果**

两个实例各自在启动时把三个 JSON 全量读入内存，关闭或定时保存时再全量覆盖：

- A 实例新增播放列表条目并退出；
- B 实例仍持有旧列表，稍后退出时用旧列表覆盖，A 的修改丢失；
- `sources.json`、`downloads.json` 同理，后退出者覆盖先退出者。

这不是“文件暂时打不开”的概率问题，而是确定性的并发写入丢失。双开是普通用户很容易触发的操作，因此按 P1 记录。

**建议**

- 在 `main.cpp` 启动时用 `QLockFile`（放在 `Data/` 或程序目录）做单实例互斥；
- 第二个实例通过 `QLocalServer` 把已有窗口激活后退出；
- 若不希望做 IPC，至少要在检测到已有实例时提示并退出，禁止两个写者同时存在。

### P1-2 播放列表“读取失败”被当成“空列表”，退出时会用空模型覆盖原文件

**位置**

- `src/core/PlaylistStore.cpp:84-131`：`readPlaylistFile()` 对“打开失败”和“文件不存在/损坏”都返回 `false`。
- `src/core/PlaylistStore.cpp:142-148`：`load()` 对任何 `false` 都返回空 `QVector`。
- `src/ui/PlaylistController.cpp:101-104`：`restore()` 直接把空结果装进模型。
- `src/ui/MainWindow.cpp:355`：`closeEvent()` 无条件 `while (!m_playlists->save())`。

**触发**

`playlists.json` 存在但读取阶段失败，例如：

- 文件被安全软件/同步工具短暂占用；
- 目录 ACL 允许写但拒绝读；
- 文件被替换成目录或异常文件；
- 便携盘/网络盘短暂不可读。

**后果**

`restore()` 得到空模型，用户看到空播放列表；退出时 `save()` 把空模型写回主文件，原播放列表被覆盖。损坏备份只覆盖 JSON 解析失败和超过 32 MB 的情况，**打开失败不会备份**，因此这条路径没有恢复副本。

**建议**

- 把读取结果改成三态：`Ok`、`NotFound`、`ReadError`；
- `NotFound` 可以按空列表处理并允许创建新文件；
- `ReadError` 必须保留“未成功读取”的状态，`closeEvent()` 遇到该状态时禁止覆盖，提示用户重试/另存；
- 或者为播放列表增加 `playlists.json.bak`，读取失败时先尝试备份，再提示恢复。

## 4. P2 问题

### P2-1 搜索结果“试听”仍会写入播放列表

**位置**

- `src/ui/PlaylistController.cpp:209-225`：`playItem()` 把搜索条目 `model->addItem(item)`，只注释“不 save”。
- `src/ui/PlaylistController.cpp:80-83`：`currentItemPositionUpdated` 在播放列表等于持久模型时调用 `save()`。
- `src/ui/MainWindow.cpp:355`：关闭时无条件保存。

**问题**

`playItem()` 没有立即 `save()`，但：

- 视频条目停止/切歌会触发 `currentItemPositionUpdated`，该连接发现当前播放列表就是 `m_videoModel`，立即 `save()`；
- 音乐条目虽然不一定触发该信号，但退出时 `closeEvent()` 无条件保存整个模型。

因此试听条目最终仍会落盘，P3 #16 只解决了“点击试听时立刻写盘”，没有解决“试听污染播放列表”。搜索结果的临时直链、签名 URL 也会被持久化。

**建议**

- 试听使用独立临时模型，或在 `MediaItem` 上增加 `preview` 标记，保存时过滤；
- 或者 `playItem()` 不把条目加入持久模型，播放器使用临时 `QMediaPlaylist`；
- 至少要在 `saveAll()` 前剔除仅用于试听的条目。

### P2-2 外部解析器结果的 `sourceOrigin` 取远程 `webUrl`，源 Cookie 可能被带到第三方

**位置**

- `src/net/ExternalSearchProvider.cpp:221-236`：`item.sourceOrigin = originOnly(item.webUrl)`，随后把 `m_config.headers` 和 Cookie 写入条目。
- `src/net/SourceRegistry.cpp:413-435`：`mediaItemWithCredentials()` 在 `sourceOrigin` 缺失时退回 `config.urlTemplate` 或 `item.webUrl`，随后重新注入配置 Cookie。
- `src/net/HeaderPolicy.h:143-176`：`shouldStripSensitive()` 只比较 host/port/scheme（https→http），同 host 即保留敏感头。

**触发**

外部解析器输出（或它抓取的远程页面）把 `webUrl` 指向第三方 host，例如：

```json
{
  "webUrl": "https://evil.example/page",
  "streamUrl": "https://evil.example/a.mp4"
}
```

**后果**

`item.sourceOrigin` 变成 `evil.example`，`item.headers` 里已经带有源站 Cookie；`SourceRegistry` 播放前又补一次配置 Cookie；`StreamProxy` 用 `Referer/Origin` 推断源站时也得到 `evil.example`，于是把 Cookie 发给第三方。

这不是“外部解析器本身可信”就能完全覆盖的问题：解析器通常只是转发站点返回的 `webUrl`，远程页面可以影响这个字段。

**建议**

- 对外部解析器结果，`sourceOrigin` 应固定取配置源站（`m_config.urlTemplate` 的 origin），不要信任远程 `webUrl`；
- 如果外部源没有 `urlTemplate`，默认不注入 Cookie/Authorization，或要求用户显式配置 `credentialOrigin`；
- `mediaItemWithCredentials()` 的 `webUrl` 兜底也应只接受与配置源站同 host 的地址，否则按空 origin 处理并剥离敏感头。

### P2-3 相同 `mediaIdentity` 的条目不会刷新 URL，在线签名直链过期后重搜仍播放旧地址

**位置**

- `src/core/PlaylistModel.cpp:69-79`：`addItem()` 遇到相同 `mediaIdentity` 直接返回 `false`，不更新旧条目。
- `src/core/PlaylistModel.cpp:135-143`：`indexOf()` 按 `mediaIdentity` 查找。
- `src/ui/PlaylistController.cpp:214-223`：`playItem()` 忽略 `addItem()` 的 `false`，直接定位旧行播放。

**触发**

用户搜索同一首歌/同一集，第一次试听后直链签名过期；再次搜索拿到新的 `streamUrl`，`mediaIdentity` 相同。

**后果**

`addItem()` 拒绝新条目，`indexOf()` 命中旧行，播放器继续使用旧签名 URL，表现为“重新搜索了但还是 404/无法播放”。视频进度、标题等也没有机会更新。

**建议**

- 提供 `PlaylistModel::upsertItem()`：identity 相同但 URL/headers/quality 变化时替换可变字段；
- 试听路径至少要把新 URL 临时覆盖到播放副本，而不是直接播放旧行；
- 已在持久列表中的条目可以保留用户进度，只刷新 `url`、`alternateUrls`、`headers`、`posterUrl`。

### P2-4 播放列表损坏备份后没有恢复入口，第二个分区会得到空列表

**位置**

- `src/core/PlaylistStore.cpp:59-82`：损坏文件 `rename` 成 `.corrupt-*`，只保留最近 5 份。
- `src/core/PlaylistStore.cpp:108-115`：解析失败时备份并返回 `false`。
- `src/ui/PlaylistController.cpp:101-104`：音乐、视频各调用一次 `load()`。

**问题**

第一次 `load(Music)` 发现损坏并把文件改名备份；第二次 `load(Video)` 看到主文件已不存在，返回空列表。原内容只存在于 `.corrupt-*`，但：

- 当前没有“从备份恢复”的 UI 或启动提示；
- 用户只看到空列表，容易误以为列表被清空；
- 下一次保存会创建新的空 `playlists.json`，备份不会自动回灌。

这是 P3 #7 修复 rename 后的副作用。

**建议**

- 在 `backupCorrupt()` 返回备份路径后，把它记录到 `PlaylistController`，启动时提示“已检测到损坏，是否从备份恢复”；
- 或者 `load()` 在备份成功后尝试读取备份内容；
- 至少要给用户可见的警告，而不是静默空列表。

### P2-5 `closeEvent()` 在保存失败弹窗前已经停止播放并退出全屏

**位置**

- `src/ui/MainWindow.cpp:349-366`：先 `m_fullscreen->exitFullScreen()`、`m_player->stop()`，然后进入 `while (!m_playlists->save())` 的 Retry/Cancel 循环。

**问题**

用户在保存失败对话框点 `Cancel` 取消退出后，窗口还在，但：

- 播放已经停止；
- 全屏已经退出。

这是明确的 UX 副作用，用户会以为“取消退出”应该恢复到关闭前状态。

**建议**

- 把 `stop()`、`exitFullScreen()` 放到保存成功之后；
- 或者在 `Cancel` 分支恢复播放状态和全屏状态；
- 下载任务提示仍在保存前，可保留现状。

### P2-6 `StreamProxy` 缓存配额不统计 pending 占位，多个未完成条目可突破 256 MB

**位置**

- `src/stream/StreamProxy.cpp:1701-1712`：`cacheHasRoom()` 只把 `bytes` 参数加到 `StreamCacheStore::bytes(entry)` 上。
- `src/stream/StreamCacheStore.cpp:3-8`：`bytes()` 只统计 `entry.data.size()` 和 `rangeParts`，不统计尚未收到的 `entry.total`。
- `src/stream/StreamProxy.cpp:489-500`、`:586`：新条目在数据到达前先把 `entry->total` 登记进 `m_cache`。

**问题**

一个刚建立的缓存条目 `data` 为空、`total` 可能是 128 MB，但 `cacheHasRoom()` 计算已有条目时按 0 字节计。于是：

- 先建立 4 个 pending 预取条目，每个 `total=128 MB`，实际已占 0；
- 第五个请求仍能通过 `totalBytes + 128 MB <= 256 MB` 的判断；
- 等数据陆续到达，缓存峰值可远超 256 MB，最多接近 `kMaxCacheEntries(24) × 128 MB`。

这是本轮 P3 缓存配额改造引入的资源回归。虽然 `pruneCache()` 在数据到达后会尝试回收，但回收前的高峰已经发生。

**建议**

- `cacheHasRoom()` 把每个条目的 `max(bytes(entry), entry->total)` 计入配额，或者为待完成条目单独维护 `reservedBytes`；
- `pruneCache()` 同样按 `entry->total` 统计 pending 占用；
- 如果全部条目都活跃，直接拒绝新缓存并走直连，而不是继续叠加占位。

## 5. P3 问题清单

| # | 问题 | 位置 | 说明 |
|---|---|---|---|
| 1 | `saveNow()` 不校验 `write()` 返回值/写入长度 | `src/download/DownloadManager.cpp:424-431` | 磁盘满或短写时可能提交截断的 `downloads.json`；`PlaylistStore`/`SourceRegistry` 都有长度校验 |
| 2 | `downloads.json` 读取没有大小上限 | `src/download/DownloadManager.cpp:319-327` | 直接 `readAll()`；播放列表 32 MB、源配置 16 MB 都有上限，下载状态没有 |
| 3 | `load()` 重新分配冲突路径后不清理旧续传点/旧分片 | `src/download/DownloadManager.cpp:377-403` | 旧路径的 `.mvpdownload` 和旧分片留下孤儿半成品 |
| 4 | `scheduleRemoveFile()` 延迟重试可能删掉新任务复用同名路径的文件 | `src/download/DownloadManager.cpp:446-461` | 25/50/75 ms 窗口内用户移除后立刻重新下载，旧重试可能命中新文件 |
| 5 | `SettingsController::apply()` 用“当前音量等于旧默认值”判断用户是否手调 | `src/ui/SettingsController.cpp:130-135` | 用户手动调到恰好等于旧默认值后再改设置，仍会被新默认值覆盖 |
| 6 | `SourceRegistry::backupCorruptFile()` 秒级时间戳 + `QFile::copy` 不重试 | `src/net/SourceRegistry.cpp:173-183` | 同一秒已有备份时 `copy` 失败，随后提示“无法备份、未覆盖原文件” |
| 7 | `HlsPlaylist::sameScope()` 只认有限变体目录名 | `src/stream/HlsPlaylist.cpp:141-158` | `chunklist/`、`v1/` 等真实目录不会触发预取回退；只影响深拖动预取性能，不影响播放 |
| 8 | `ExternalSearchProvider` 不按 `limit` 截断结果 | `src/net/ExternalSearchProvider.cpp:202-244` | 2 MB 输出可产生数万条结果，`SearchCoordinator` 合并出口也没有总数上限 |
| 9 | `MediaRemux::cancel()` 最多阻塞 UI 3 秒 | `src/download/MediaRemux.cpp:42-55` | 为保证 Windows 删除临时文件而 `waitForFinished(3000)`，取消下载时会有可感知卡顿 |
| 10 | `DownloadTask` 对缺少 `#EXT-X-ENDLIST` 的列表直接拒绝 | `src/download/DownloadTask.cpp:689-695` | 对“服务器省略 ENDLIST 的有限 VOD”兼容性不足；当前是安全取舍，是否支持需产品确认 |
| 11 | `cacheStats()` 不统计 pending 占位 | `src/stream/StreamProxy.cpp:1774-1782` | 诊断数字偏小，与 P2-6 同源，但不影响实际运行 |

## 6. 复核后不成立或不应按缺陷上报的点

以下问题在本轮复审中被重新检查，结论是不成立，或属于已接受的设计：

- `SourceManagerDialog::onImport()` 对导入文件内部的重复 `id` 会依次合并，最终不会在 `m_drafts` 留下两个同 id 条目；只有计数文案可能偏大，不构成功能缺陷。
- `HttpClient::fetchInternal()` 的手动 302 跟随仍逐跳调用 `sanitizedForTarget()`；跨 host、https→http 仍会剥离敏感头。
- `HttpClient::isAllowedScheme()` 仅允许 http/https；`file://` 在播放代理路径会被拒绝，不会进入 `StreamProxy` 上游请求。
- `HlsPlaylist::rewrite()` 仍拒绝非 HTTP(S) 的 `URI=` 和 `#EXT-X-BASE`。
- `MediaItem` 持久化仍通过 `HeaderPolicy::persistedHeaders()` 去掉敏感头；读取时也会丢弃旧版明文 Cookie/Authorization。
- `StreamProxy` 的 token 仍是 128 位十六进制字符串；非根目录 token 的 suffix 仍有同 authority 校验。
- `DownloadTask::commitFile()` 提交前会重新检查同名文件并通过 `m_pathResolver` 改名，不会直接删除用户后建的同名文件。
- `DownloadManager::startRemux()` 只在源 TS 大小等于记录值时删除源文件，方向正确；普通路径不会误删用户文件。
- `LyricsController` 对 `online` 条目的本地歌词有拦截；`MusicVisualWidget` 只对 `!online` 的本地封面读取，未发现远程条目读取本地封面的路径。

## 7. 需要产品确认，不计入缺陷

1. **在线音乐封面**：`MusicVisualWidget::setTrack()` 只加载 `!item.online && posterUrl.isLocalFile()` 的封面，在线搜索结果的 `posterUrl` 没有使用。若产品希望在线音乐显示远程封面，这是功能缺口；若有意只显示本地封面，则保持现状。
2. **无 ENDLIST 的 HLS 下载**：当前实现把缺少 `#EXT-X-ENDLIST` 视为直播/未完成列表并拒绝。对于服务器省略 ENDLIST 的有限 VOD，用户无法下载；是否放宽需要产品明确。
3. **导出明文凭据**：P3 #10 的“导出源配置写明文 Cookie/Authorization”仍是产品/安全行为变化，尚未看到用户明确接受或拒绝。

## 8. 回归与安全边界复核

### 已复核通过

- `HttpClient::get()`/`postJson()` 使用 `SameOriginRedirectPolicy`，不再自动跨 host 跟随。
- `fetchInternal()` 每跳都重新执行 `isAllowedScheme()` 和 `sourceOrigin` 裁剪。
- `StreamProxy::proxyUrlFor()` 只登记 http/https 上游；`HttpClient` 会在拉取前拒绝非 http/https。
- `HlsPlaylist::rewrite()` 对播放列表内所有 `URI=` 属性做 scheme 校验和代理改写。
- `SourceRegistry::load()` 对 `sources.json` 有 16 MB 上限，对重复 `sourceId` 有去重。
- `PlaylistStore` 对 `playlists.json` 有 32 MB 上限和损坏备份数量上限。
- `DownloadTask` 对 Range 续传校验 `Content-Range` 起点，对分片 Range 校验 206 和返回区间。
- `DownloadTask` 对加密 HLS（`EXT-X-KEY`/`SESSION-KEY`）直接拒绝，不生成损坏文件。
- `PlaybackController` 只在 `online` 条目走本地代理；本地文件直接 `file://` 播放。
- 设置页源管理改动在取消时仍有回滚路径。

### 仍需补测

- 双实例同时启动、交替修改三个 JSON 的覆盖行为。
- `playlists.json` 打开失败但可写时，关闭程序是否覆盖空列表。
- 搜索结果试听后重启，播放列表是否出现试听条目。
- 外部解析器返回第三方 `webUrl` 时 Cookie 的实际发送 host。
- 并发深拖动/预取下 `StreamProxy` 的 RSS 峰值是否超过 256 MB。
- `downloads.json` 短写/磁盘满时是否留下截断 JSON。
- 无 ENDLIST 的真实 VOD 列表下载行为。
- `SettingsController` 音量“等于旧默认值”的手动调整场景。

## 9. 建议修复顺序

1. **P1-1**：先加单实例锁，这是最容易触发、后果最直接的数据丢失入口。
2. **P1-2**：把播放列表读取改成三态，禁止读取失败后静默覆盖。
3. **P2-2**：收紧外部解析器的 `sourceOrigin` 和 Cookie 注入，避免凭据带到第三方。
4. **P2-6**：缓存配额按 `entry->total` 统计 pending 占位，避免配额名存实亡。
5. **P2-1 / P2-3 / P2-4**：试听隔离、条目 URL 刷新、损坏备份恢复入口。
6. **P2-5**：调整关闭顺序，取消退出时恢复播放/全屏状态。
7. **P3**：按表清理，优先处理 `saveNow()` 写入校验、`downloads.json` 大小上限、延迟删除竞态。

## 10. 交付说明

- 本报告是只读复审产物，未修改任何源码，未提交 Git。
- 回归测试结果仅代表当前 HEAD `aa792a0` 的现有测试基线，不代表上述 P1/P2 已被测试覆盖。
- 上一轮 `CODE_REVIEW.md` 的 P3 清单仍可作为历史对照；本报告不改变其内容。
