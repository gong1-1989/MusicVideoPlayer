# 影视网站探查节点采集与验证报告

生成日期：2026-11-23
数据文件：`docs/source-probe/video-nodes.json`、`docs/source-probe/video-probe-results.json`

## 1. 采集口径

- 目标：为「网站探查」补充真实可验证的影视接口节点，解决内置路径过少、试搜成功但搜索词不生效的问题。
- 轮次：10 轮；每轮不少于 15 个节点。
- 范围：只收集公开可访问的影视站点/采集接口节点，不保存影片标题、播放地址和任何直链。
- 去重：按 `baseUrl` 去重，共 186 个唯一节点。
- 来源：绿联 NAS 论坛公开 JSON 源列表、空壳影视「主流采集资源站一览」、博客园接口列表、OpenI 采集站专题、氢设计解析接口、MacCMS 前端接口文档、公开 TVBox 配置，以及公开页公布的备用域名序列。
- 第 2 轮额外纳入同页公布的 `/api.php/provide/vod/from/xxx/` 优化接口；第 5~10 轮纳入 `aosikazy1..10`、`jyzy2/3`、`kudouzy2..4`、`fszy2..10`、`lzizy2..7`、`wuzy2..9`、`1080zyk2..5`、`ukuzy1..9`、`shandianzy.net/cc`、`xiangjiaozy2..10`、`tkzy1..21` 等公开备用域名。

每轮节点数：

| 轮次 | 节点数 |
| --- | --- |
| 1 | 37 |
| 2 | 20 |
| 3 | 17 |
| 4 | 22 |
| 5 | 15 |
| 6 | 15 |
| 7 | 15 |
| 8 | 15 |
| 9 | 15 |
| 10 | 15 |
| **合计** | **186** |

## 2. 真实 HTTP 验证方法

对每个节点依次尝试：

1. `?ac=detail&wd=电影`
2. `?ac=list&wd=电影`
3. `?ac=videolist&wd=电影`
4. `?ac=detail`（无关键词，用于识别只能列目录的接口）

记录状态码、最终 host、Content-Type、响应格式、`list` 条数、`vod_name`/`vod_play_url`/`vod_play_from` 是否存在、耗时和错误，不保存标题与播放地址。

关键修正：**不能只看「带 wd 的 URL 返回 200」就判定可搜索**。很多 MacCMS 会接受 `wd` 但返回固定列表，旧统计会把这类接口误算成可用。本轮对每个首轮成功的接口再发一次 `wd=电视剧`，比较两次 `vod_name` 集合：

- 第二次有结果且标题集合不同 → `SearchOk = true`，真正按关键词搜索。
- 第二次有结果但标题集合完全相同 → 接口忽略 `wd`，只返回固定列表。
- 只有无关键词的 `?ac=detail` 能返回列表 → 只能列目录，不保存为搜索源。

## 3. 验证结果

186 个节点中：

- 51 个能返回 JSON 列表并包含 `vod_name`、`vod_play_url`、`vod_play_from`。
- 23 个通过两轮关键词比对，属于**真正可搜索**接口。
- 28 个属于 catalog-only：只能列目录，`wd` 不生效。
- 0 个返回真实 XML；`/at/xml/` 仅作为诊断归类，不参与保存。
- 其余为网页（HTML）或连接失败/超时/域名失效。

分轮统计：

| 轮次 | 节点 | 可列目录 | 可搜索 | catalog-only | JSON 无播放字段 | XML | HTML | 失效 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 37 | 19 | 8 | 11 | 0 | 0 | 0 | 14 |
| 2 | 20 | 8 | 6 | 2 | 0 | 0 | 3 | 9 |
| 3 | 17 | 0 | 0 | 0 | 0 | 0 | 9 | 8 |
| 4 | 22 | 5 | 3 | 2 | 0 | 0 | 4 | 16 |
| 5 | 15 | 10 | 2 | 8 | 0 | 0 | 0 | 5 |
| 6 | 15 | 4 | 4 | 0 | 0 | 0 | 6 | 11 |
| 7 | 15 | 3 | 0 | 3 | 0 | 0 | 4 | 12 |
| 8 | 15 | 2 | 0 | 2 | 0 | 0 | 2 | 4 |
| 9 | 15 | 0 | 0 | 0 | 0 | 0 | 1 | 15 |
| 10 | 15 | 0 | 0 | 0 | 0 | 0 | 0 | 14 |
| **合计** | **186** | **51** | **23** | **28** | **0** | **0** | **29** | **103** |

## 4. 真正可搜索的 23 个节点

| 名称 | 最终 host | 基础接口 |
| --- | --- | --- |
| 360资源 | 360zy.com | `https://360zy.com/api.php/provide/vod/` |
| 百度云资源 | api.apibdzy.com | `https://api.apibdzy.com/api.php/provide/vod/` |
| 非凡影视 | ffzy5.tv | `http://ffzy5.tv/api.php/provide/vod/` |
| 豪华资源 | hhzyapi.com | `https://hhzyapi.com/api.php/provide/vod/` |
| 红牛资源 | www.hongniuzy2.com | `https://www.hongniuzy2.com/api.php/provide/vod/` |
| 魔都资源 | www.mdzyapi.com | `https://www.mdzyapi.com/api.php/provide/vod/` |
| 最大资源 | api.zuidapi.com | `https://api.zuidapi.com/api.php/provide/vod/` |
| CK资源 | ckzy.me | `https://ckzy.me/api.php/provide/vod/` |
| 百度优化接口 | api.apibdzy.com | `https://api.apibdzy.com/api.php/provide/vod/from/dbm3u8/` |
| 豪华优化接口 | hhzyapi.com | `https://hhzyapi.com/api.php/provide/vod/from/hhm3u8/` |
| 红牛优化接口 | www.hongniuzy2.com | `https://www.hongniuzy2.com/api.php/provide/vod/from/hnm3u8/` |
| 魔都资源 | caiji.moduapi.cc | `https://caiji.moduapi.cc/api.php/provide/vod/` |
| 无尽优化接口 | api.wujinapi.com | `https://api.wujinapi.com/api.php/provide/vod/from/wjm3u8/` |
| 无尽资源 | api.wujinapi.com | `https://api.wujinapi.com/api.php/provide/vod/` |
| 非凡资源网 | www.ffzy.tv | `https://www.ffzy.tv/api.php/provide/vod/` |
| 金鹰备用A | jyzy1.com | `https://jyzy1.com/api.php/provide/vod/` |
| 量子备用A | lzizy1.com | `https://lzizy1.com/api.php/provide/vod/` |
| 金鹰备用B | jyzy2.com | `https://jyzy2.com/api.php/provide/vod/` |
| 金鹰备用C | jyzy3.com | `https://jyzy3.com/api.php/provide/vod/` |
| 量子备用2 | lzizy2.com | `https://lzizy2.com/api.php/provide/vod/` |
| 量子备用3 | lzizy3.com | `https://lzizy3.com/api.php/provide/vod/` |
| 量子备用4 | lzizy4.com | `https://lzizy4.com/api.php/provide/vod/` |
| 量子备用5 | lzizy5.com | `https://lzizy5.com/api.php/provide/vod/` |

路径分布：19 个标准 `/api.php/provide/vod/`，4 个优化 `/api.php/provide/vod/from/.../`；所有 23 个的有效查询都是 `?ac=detail&wd=<关键词>`。

只返回固定列表、不响应关键词的代表节点：`api.yparse.com`、`api.guangsuapi.com`、`www.huyaapi.com`、`jszyapi.com`、`jyzyapi.com`、`caiji.maotaizy.cc`、`api.niuniuzy.me`、`sdzyapi.com`、`tyyszy.com`、`api.wujinapi.me`、`cj.yayazy.net`、`aosikazy1..9.com`、`wuzy2..4.com`、`shandianzy.net/cc`。

## 5. 对探查器的改造

### 5.1 根因

旧 `SourceProbe::defaultPaths(Video)` 只有 5 条，其中一条是 `/api.php/provide/vod/?ac=list`，**不带关键词占位符**。试搜时请求固定列表，当然“有结果”，但搜索词完全不生效；用户看到探测成功，实际搜索时永远返回同一批内容。

旧逻辑还有三个问题：

- 候选按“路径优先”排列，首页 301 后把一半尝试次数浪费在旧域名上。
- 把 XML、IP 未授权、网页都归为“不是 JSON”，用户无法判断是路径错、授权错还是格式不支持。
- 只做一次搜索校验，识别不了“忽略 wd、只返回固定列表”的接口。

### 5.2 修改内容

`src/net/SourceProbe.cpp` / `SourceProbe.h`：

1. 影视内置路径从 5 条扩展到 10 条，全部带 `{keyword}` 和 `wd=`：
   - `/api.php/provide/vod/?ac=detail&wd={keyword}`
   - `/api.php/provide/vod/?ac=videolist&wd={keyword}`
   - `/api.php/provide/vod/?ac=list&wd={keyword}`
   - `/api.php/provide/vod/at/json/?ac=detail&wd={keyword}`
   - `/index.php/ajax/data?mid=1&wd={keyword}&limit={limit}`
   - `/inc/api.php?ac=detail&wd={keyword}`
   - `/inc/api_mac10.php?ac=detail&wd={keyword}`
   - `/inc/api_mac_m3u8.php?ac=detail&wd={keyword}`
   - `/api.php/provide/vod?ac=detail&wd={keyword}`
   - `/api.php/provide/vod/?wd={keyword}`
2. 新增 `appendProbeQuery`：
   - 用户粘贴的示例搜索地址若带固定 `wd=电影`，替换成 `wd={keyword}`，而不是直接丢弃。
   - 首页抓到的接口自动补 `ac=detail&wd={keyword}`；`/index.php/ajax/data` 补 `mid=1&limit={limit}&wd={keyword}`。
3. 新增 `extractHomeApiHints`：从首页 HTML/JS 抓 `/api.php/provide/vod...`、`/from/xxx/`、`/inc/api.php`、`/inc/api_mac10.php`、`/inc/api_mac_m3u8.php`、`/index.php/ajax/data`，优先于内置路径。
4. 首页线索按 JSON 优先、XML 最后排序。真实站点 `api.apibdzy.com` 首页同时暴露 `/at/xml/` 和 JSON，旧顺序会先浪费一次请求。
5. 候选顺序改为：用户明确接口 > 首页线索 > 用户自定义路径 > 内置路径；域名优先，先试用户输入域名，再试跳转域名。
6. 探测预算从 18 秒/10 次提升到 24 秒/16 次，自动识别 32 秒/20 次；首页线索和自定义路径不占内置路径的尝试次数，避免后面的内置路径没机会执行。
7. 新增两轮试搜校验：
   - 第一轮 `电影`/`音乐`，第二轮 `电视剧`/`歌曲`。
   - 两轮标题集合完全相同 → 判定“接口忽略关键词，只返回固定列表”，不保存。
   - 第二轮失败或无结果 → 保留首轮结果，避免网络抖动误杀。
8. 失败归类新增：`IP 未授权/已到期`、`XML 只支持 JSON`、`JSON 无结果数组`、`JSON 有数组但字段猜不出`，并在 `diagnoseFailure` 给出可行动提示。

## 6. 验证记录

- `cmake --build`：F 盘全新构建目录，101 个目标全部成功；`MusicVideoPlayer.exe`、`mvp_tests.exe`、`mvp_integration.exe`、`mvp_storage_tests.exe`、`mvp_ui_tests.exe` 均链接成功。
- `ctest`：4/4 通过（`mvp_core_logic`、`mvp_integration`、`mvp_storage`、`mvp_ui_logic`）。
- 单元测试 `sourceProbeVideoPathsAreSearchable`：断言影视路径至少 10 条、全部含 `{keyword}` 和 `wd=`，并且不再包含不带关键词的 `/api.php/provide/vod/?ac=list`。
- 真实站点测试 `sourceProbeLiveVideoSearch`（需 `MVP_LIVE_PROBE=1`）：对 `https://api.apibdzy.com` 探测成功，识别为影视，保存地址为 `/api.php/provide/vod/?ac=detail&wd={keyword}`，首轮搜索 35 条；第二轮 `电视剧` 0 条时保留首轮结果。测试通过。
- 全量脚本验证：186 个节点、10 轮、每轮 ≥15；51 个可列目录、23 个真正可搜索、28 个 catalog-only。

## 7. 已知边界

- 当前探查器只解析 JSON；真实 XML 节点在本批 186 个里未发现，遇到只有 `/at/xml/` 的站点会明确提示“当前只解析 JSON”。
- `Content-Type` 不可靠：多个可用接口返回 `text/html` 但正文是 JSON，因此格式判断以正文首字符为准。
- `ac=list`、`ac=videolist` 在本批真实节点里没有单独成为有效搜索路径；主流仍是 `ac=detail&wd=`。
- `from/xxx/` 是按资源站模块区分的接口，不能用统一模板枚举，必须从首页线索或用户输入中获得。
- 186 个节点是否内置成 `SourceRegistry` 种子源，尚未改动；当前只作为探查器路径覆盖和验证依据。

