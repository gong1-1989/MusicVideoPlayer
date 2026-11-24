# MusicVideoPlayer

基于 Qt 6 Widgets / Multimedia 的 Windows 本地音乐与视频播放器，支持本地文件、配置化在线搜索、m3u8 播放、播放缓存、下载队列与 HLS 下载转 MP4。

## 功能概览

- 本地音乐 / 视频播放，音乐歌词解析与在线歌词缓存
- 音乐、视频分列表管理，视频条目跟随播放列表保存播放进度
- 配置驱动搜索源：HTTP JSON、maccms 网页源、本机外部解析器
- HLS 经本地回环代理播放，支持 Referer / Cookie、相对分片改写和深拖动预取
- 下载队列、暂停续传、退出时保留进度、并发上限与单任务体积上限
- HLS 下载完成后调用内置 FFmpeg 无损转 MP4
- 搜索播放缓存：音乐单文件、影视目录，可直接转为下载
- 深色 / 浅色主题、自定义图标、真实像素字号、全屏自动隐藏控制条

## 环境

- Qt 6.10.1（MinGW 64-bit）
- CMake 3.21+
- C++17
- Windows 10/11

## 构建

```powershell
$env:PATH = 'D:\Qt\Tools\mingw1310_64\bin;D:\Qt\Tools\CMake_64\bin;D:\Qt\6.10.1\mingw_64\bin;' + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

`build` 是可再生目录，清理项目时可以直接删除；下次重新执行上面的配置命令即可。

## 运行

```powershell
$env:PATH = 'D:\Qt\6.10.1\mingw_64\bin;' + $env:PATH
.\build\MusicVideoPlayer.exe
```

运行时必须能找到 Qt Multimedia 后端，否则视频会黑屏或提示缺少插件。构建时会把 `third_party/ffmpeg/` 里的 FFmpeg 解压并复制到程序目录 `ffmpeg/`；也可以通过 `MVP_FFMPEG_PATH` 指向自定义 `ffmpeg.exe`。

## 数据目录

用户数据只放在**程序根目录**（exe 所在目录）下的 `Data/`，不写系统用户数据目录：

- `Data/playlists.json`：音乐 / 视频播放列表；视频条目自带 `position`（毫秒）播放进度
- `Data/sources.json`：搜索源配置
- `Data/downloads.json`：下载队列与续传状态
- `Data/media-cache/`：播放缓存；音乐为单文件，影视按条目目录保存
- `Data/lyrics-cache/`：在线歌词缓存
- `Data/icons/`、`Data/ui-icons.json`：可选的自定义界面图标
- `log/MusicVideoPlayer.log`：运行日志，单文件超过 8 MB 轮转为 `.1`

程序目录不可写时，保存会直接失败并在界面提示，不会偷偷改写到 `QStandardPaths::AppDataLocation`。

测试专用：设置环境变量 `MVP_DATA_DIR` 可把数据目录指向临时路径，单元测试用它避免碰真实数据。

## 视频播放与 HLS

- 视频条目有多个播放地址时优先选择 MP4/FLV 直链，HLS 作为回退地址保留；下载同样使用优先地址。
- HLS 统一经 `StreamProxy` 本地回环代理，补 Referer/Cookie，并把 m3u8 里的相对分片改写成本地 token 地址。
- 分片请求先发 16 KB 探测 Range，再按 4 路 Range 拆分；上游忽略 Range、不返回 206 时自动回退单路透传。
- 深拖动时，代理按播放列表索引定位目标分片，预取 `targetIndex..targetIndex+9`，每片 4 路 Range、同时 4 片流水预取。预取窗口必须用播放列表索引，不能用文件名反推。
- 正常播放使用较小的顺序预取窗口；连续失败或预取超时会自动降并发、暂停预取，优先保证前台播放。
- 网络响应设统一上限：JSON 与外部解析器输出 2 MB、HTML 4 MB、HLS 播放列表 4 MB 且最多 1 万片。普通请求在接收过程中累计超过上限会立即中止，超限按失败处理并写日志。

## 下载

- 下载队列最多同时运行 2 个任务，其余暂停排队；程序退出时活动任务转为暂停，下次启动自动续传。
- 半成品使用 `.mvpdownload`；同名最终文件、半成品或 HLS `.ts` 中间文件被占用时，自动改用 `(1)`、`(2)` 等后缀，避免互相覆盖。
- 默认下载目录是系统“下载”下的 `MusicVideoPlayer`，音乐、影视分别落到 `音乐`、`影视` 子目录，可在系统设置里修改根目录。
- 单个下载任务默认超过 8 GB 停止，可在设置里改为 2/4/8/16 GB；配置键允许 512 MB–64 GB。
- HLS 下载先合并分片，完成后调用内置 FFmpeg 无损转封装为 MP4；取消或删除任务时同步清理本任务自己的半成品，不碰同路径的其他任务文件。
- 下载完成后默认删除该条目的播放缓存与歌词缓存，避免同一内容在 `media-cache`、`lyrics-cache` 和下载目录各留一份；普通网络下载即使没有复用缓存，也会按条目身份补扫清理。可在系统设置关闭。缓存正被播放器占用时删除会失败，任务会保留清理标记并写入 `downloads.json`，由定时器重试，下次启动继续清理。

## 界面与字体

- 播放控制键固定在控制条中间；模式、全屏、设置、音量放在右侧。
- 播放键默认使用内置矢量图标，鼠标悬停显示文字说明；主题切换时图标按当前强调色重画。
- 用户自定义图标：把 `prev / play / pause / stop / next / mode-list / mode-single / mode-random / fullscreen / settings / volume` 同名文件（支持 svg/png/ico/jpg/jpeg/webp/bmp）放到程序目录 `icons/` 或 `Data/icons/` 即可覆盖内置图标。
- 也可以用 `Data/ui-icons.json` 精确指定文件，相对路径按 exe 目录解析，例如 `{"play":"icons/custom/play.png","pause":"icons/custom/pause.png"}`。
- 系统设置里的“字体大小”是真实像素字号：10/11/12/13/14/15/16/18/20 px，默认 13 px；配置文件允许 9–20 px。列表、设置页和音乐动感视图会一起调整。

## 搜索源

`Data/sources.json` 是配置驱动的源列表：

- `kind=config`：HTTP 接口，按 `itemsPath` / `fields` 取字段，可选 `detailUrl` 做二次解析
- `kind=builtin`：内置解析器（当前为 maccms 网页源）
- `kind=external`：调用本机外部程序，默认关闭，需要显式开启

每个源实际抓取条数由系统设置“搜索条数”控制（配置键 `search/limit`，默认 20，范围 1–100；设置界面提供 10/20/30/50）。搜索结果每页条数按搜索列表当前可见高度和实际行高动态计算，分页条右侧显示当前“每页 N 条”。

网页影视源在 `timeoutMs=0` 时按搜索条数自动放宽单源超时；`timeoutMs>0` 时配置型、网页型和外部源都按该值限时。

## 测试

```powershell
ctest --test-dir build --output-on-failure
```

- `mvp_core_logic`：时间格式、LRC、条目/源配置校验、URL 脱敏、HLS 分片与播放列表解析、缓存路径
- `mvp_integration`：本地 HTTP 服务、重定向密钥剥离、下载完整性、搜索超时/中止、响应上限
- `mvp_storage`：播放列表、视频进度持久化、搜索源、损坏文件备份、下载状态与路径冲突
- `mvp_ui_logic`：歌词切歌取消、进度条点击定位、动态每页条数、字体缩放、播放列表保存失败重试

## 发布

发布构建不生成测试程序：

```powershell
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
cmake --install build --prefix release
```

- `BUILD_TESTING` 是 CMake/CTest 标准开关，设为 `OFF` 时 `mvp_tests`、`mvp_integration`、`mvp_storage_tests`、`mvp_ui_tests` 不会构建。
- `MVP_INSTALL_TESTS` 默认 `OFF`，即使开发构建里生成了测试程序，`cmake --install` 也不会把它们装进发布目录；只有显式设置 `-DMVP_INSTALL_TESTS=ON` 才会安装测试程序。
- 安装脚本会通过 `qt_generate_deploy_app_script` 部署 Qt 运行库，并把 FFmpeg 放到发布目录的 `bin/ffmpeg/`。

## 目录

```
src/app        程序入口、日志、单实例
src/audio      音频频谱分析
src/cache      播放缓存、歌词缓存、HLS 缓存
src/core       播放控制、播放列表模型与持久化
src/download   下载任务、队列、转封装
src/lyrics     歌词解析与服务
src/media      媒体条目与身份规则
src/net        HTTP、搜索源、搜索协调、源探测
src/settings   设置、快捷键、窗口预设
src/storage    数据目录解析
src/stream     m3u8 / 音频本地流转代理
src/ui         主窗、控制器与对话框
tests          单元与集成测试
resources      主题 QSS 与图标
docs           源探查规则与结果
tools          源探查脚本
third_party    内置 FFmpeg 压缩包
```
