# 内置 FFmpeg

本目录用于随程序分发 HLS 下载后的无损转封装能力。

- 构建来源：BtbN / FFmpeg-Builds
- 版本：FFmpeg n8.1，Windows x64，LGPL shared
- 压缩包：`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`
- 下载地址：https://github.com/BtbN/FFmpeg-Builds/releases
- 许可证：LGPL v3，完整文本随压缩包内的 `LICENSE.txt` 一起安装到发布包的 `ffmpeg/` 目录

`CMakeLists.txt` 会在配置阶段把压缩包解压到构建目录，并在构建/安装时把 `ffmpeg.exe` 和它依赖的 DLL 复制到程序目录的 `ffmpeg/`。程序通过 `MediaRemux` 调用：

```text
ffmpeg.exe -y -i input.ts -c copy -movflags +faststart output.mp4
```

只做无损转封装，不重新编码，不改变音视频内容。
