# MusicVideoPlayer

基于 Qt 6 Widgets / Multimedia 的本地音乐与视频播放器，支持本地文件、在线搜索（网易云、Audius、网页影视源）和 m3u8 播放。

## 环境

- Qt 6.10.1（MinGW 64-bit）
- CMake 3.21+
- C++17
- Windows 10/11

## 构建

```powershell
$env:PATH = 'd:\Qt\Tools\mingw1310_64\bin;D:\Qt\Tools\CMake_64\bin;D:\Qt\6.10.1\mingw_64\bin;' + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

构建目录也可以用 Qt Creator 自带的 `build/Desktop_Qt_6_10_1_MinGW_64_bit_Debug`。

## 运行

```powershell
$env:PATH = 'D:\Qt\6.10.1\mingw_64\bin;' + $env:PATH
.\build\Desktop_Qt_6_10_1_MinGW_64_bit_Debug\MusicVideoPlayer.exe
```

运行时必须能找到 Qt 的 Multimedia 后端（FFmpeg），否则视频会黑屏或提示缺少插件；上面的 PATH 是最省事的方式。

## 数据目录

用户数据放在**程序根目录**（exe 所在目录）下的 `Data/`：

- `playlists.json`：音乐 / 视频播放列表
- `sources.json`：搜索源配置

运行日志放在 exe 同级的 `log/MusicVideoPlayer.log`，单文件超过 8 MB 轮转为 `.1`；当 `log/` 不可写时，回退到系统用户数据目录（`QStandardPaths::AppDataLocation`）下的同名日志文件。

测试专用：设置环境变量 `MVP_DATA_DIR` 可把数据目录指向临时路径，单元测试用它避免碰真实数据。

## 视频播放与 HLS 深拖动

- 视频条目有多个播放地址时优先选择 MP4/FLV 直链，HLS 作为回退地址保留；下载同样使用优先地址。
- HLS 统一经 `StreamProxy` 本地回环代理，补 Referer/Cookie，并把 m3u8 里的相对分片改写成本地 token 地址。
- 分片请求先发 16 KB 探测 Range，再按 4 路 Range 拆分；上游忽略 Range、不返回 206 时自动回退单路透传。
- 深拖动时，代理按播放列表索引定位目标分片，预取 `targetIndex..targetIndex+9`，每片 4 路 Range、同时 4 片流水预取。实测 600 秒目标从约 6.5 秒降到约 4.5 秒；同条件关闭预取约 5.5 秒。
- 预取窗口必须用播放列表索引，不能用文件名反推：播放列表索引 152 可能对应文件 `0000143.ts`，按文件名偏移会导致整段预取错位。
- 网络响应设统一上限：JSON 与外部解析器输出 2 MB、HTML 4 MB、HLS 播放列表 4 MB 且最多 1 万片；单个下载文件默认超过 8 GB 停止，可在系统设置里改为 2/4/8/16 GB。普通请求在接收过程中累计超过上限会立即中止，不等响应收完；超限按失败处理并写日志，避免异常源把内存或磁盘吃满。
- 公开 MP4 样例源（`https://dpaste.org/9mnUE/raw`）仅用于验收 MP4 直链与拖动，默认停用，避免静态样例污染正常搜索结果。

## 测试

```powershell
ctest --test-dir build/Desktop_Qt_6_10_1_MinGW_64_bit_Debug --output-on-failure
```

包含 `mvp_core_logic`（时间格式、LRC 解析、条目/源配置校验、日志 URL 脱敏、HLS 分片多连接 Range 与 206 回退）、`mvp_integration`（本地 HTTP 服务、重定向密钥剥离、下载完整性、搜索超时/中止、响应上限）、`mvp_storage`（播放列表与搜索源持久化、损坏文件备份）与 `mvp_ui_logic`（歌词切歌取消、播放列表保存失败重试）。

## 搜索源

`sources.json` 是配置驱动的源列表：

- `kind=config`：HTTP 接口，按 `itemsPath` / `fields` 取字段，可选 `detailUrl` 做二次解析
- `kind=builtin`：内置解析器（当前为 maccms 网页源）；网易云、Audius、iTunes 等仍由配置描述
- `kind=external`：调用本机外部程序，默认关闭，需要显式开启

每个源实际抓取条数由系统设置“搜索条数”控制（配置键 `search/limit`，默认 20，范围 1–100；设置界面提供 10/20/30/50，配置文件里的其它值会作为“当前配置”保留）。主界面的“每页显示条数”只控制结果分页（配置键 `search/pageSize`，默认 20，范围 5–200），不改变每个源抓取多少条。网页影视源在 `timeoutMs=0` 时会按搜索条数自动放宽单源超时；`timeoutMs>0` 时配置型、网页型和外部源都按该值限时。

## 目录

```
src/app        程序入口、日志
src/core       播放控制、播放列表模型与持久化
src/media      媒体条目
src/net        HTTP、搜索源、搜索协调、健康探测
src/stream     m3u8/音频本地流转代理
src/download   下载任务
src/lyrics     歌词解析与界面
src/ui         主窗与各控制器、对话框
src/settings   设置、主题、窗口预设
src/storage    数据目录解析
tests          单元测试
resources      主题 QSS 与图标
```
