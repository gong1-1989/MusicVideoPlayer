#ifndef DOWNLOADPATHS_H
#define DOWNLOADPATHS_H

#include <QString>
#include <QUrl>

namespace DownloadPaths {

/// 在目标目录生成不覆盖已有文件的路径；同名时追加 “ (1)”“ (2)”。
QString uniqueFilePath(const QString &directory, const QString &fileName);
/// 根据地址和是否 HLS 选默认扩展名。HLS 统一先落 .ts，下载完成后再无损转 MP4。
QString suffixForDownload(const QUrl &url, bool hls, bool video);

} // namespace DownloadPaths

#endif // DOWNLOADPATHS_H
