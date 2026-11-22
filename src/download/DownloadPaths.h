#ifndef DOWNLOADPATHS_H
#define DOWNLOADPATHS_H

#include <QString>
#include <QUrl>

#include <functional>

namespace DownloadPaths {

/// 在目标目录生成不覆盖已有文件的路径；同名时追加 “ (1)”“ (2)”。
/// inUse 用于把下载队列里已经开始占用的路径也算作冲突，避免两个任务
/// 同时拿同一个最终名和同一个 .mvpdownload 半成品。
using PathInUse = std::function<bool(const QString &)>;
QString uniqueFilePath(const QString &directory, const QString &fileName,
                       const PathInUse &inUse = PathInUse());

/// 比较两个路径是否指向同一个文件；Windows 下按大小写不敏感处理。
bool pathsReferToSameFile(const QString &a, const QString &b);
/// 根据地址和是否 HLS 选默认扩展名。HLS 统一先落 .ts，下载完成后再无损转 MP4。
QString suffixForDownload(const QUrl &url, bool hls, bool video);

} // namespace DownloadPaths

#endif // DOWNLOADPATHS_H
