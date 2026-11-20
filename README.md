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
- `log/MusicVideoPlayer.log`：运行日志，单文件超过 8 MB 轮转为 `.1`

测试专用：设置环境变量 `MVP_DATA_DIR` 可把数据目录指向临时路径，单元测试用它避免碰真实数据。

## 测试

```powershell
ctest --test-dir build/Desktop_Qt_6_10_1_MinGW_64_bit_Debug --output-on-failure
```

包含 `mvp_core_logic`（时间格式、LRC 解析、条目/源配置校验、日志 URL 脱敏）与 `mvp_storage`（播放列表与搜索源持久化、损坏文件备份）。

## 搜索源

`sources.json` 是配置驱动的源列表：

- `kind=config`：HTTP 接口，按 `itemsPath` / `fields` 取字段，可选 `detailUrl` 做二次解析
- `kind=builtin`：内置解析器（网易云、Audius、maccms 网页源）
- `kind=external`：调用本机外部程序，默认关闭，需要显式开启

网页影视源有独立的单次抓取上限（默认 6 条），与界面上的“每页显示条数”不是同一个设置。

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
