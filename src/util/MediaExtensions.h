#ifndef MEDIAEXTENSIONS_H
#define MEDIAEXTENSIONS_H

#include <QString>

class QUrl;

/// 全项目统一的媒体后缀策略。
///
/// 不同调用点需要的集合并不相同：缓存落盘只接受可安全写盘的后缀，
/// 下载路径需要影视/音乐两套回退规则，粘贴链接识别还要包含播放列表。
/// 因此这里按用途分函数，不合并成一张“大而全”的表。
namespace MediaExtensions {

/// 从本地路径或 URL 的 path 提取小写后缀；没有后缀时返回空。
QString suffixFromPath(const QString &path);
QString suffixFromUrl(const QUrl &url);

/// 播放缓存允许用于磁盘文件名的安全后缀。
bool isCacheSafeSuffix(const QString &suffix);

/// 常见音频后缀；下载路径与直链分类共用。
bool isAudioSuffix(const QString &suffix);

/// 下载文件名可保留的影视/音乐后缀。
bool isDownloadVideoSuffix(const QString &suffix);
bool isDownloadAudioSuffix(const QString &suffix);

/// 内容嗅探认为“像媒体/播放列表”的后缀（含 m4s、m3u8）。
bool isMediaContentSuffix(const QString &suffix);

/// 用户粘贴链接时可直接当直链处理的后缀（额外包含 mpd）。
bool isDirectLinkSuffix(const QString &suffix);

/// 本地音乐可标记为“无损”的后缀。
bool isLosslessAudioSuffix(const QString &suffix);

} // namespace MediaExtensions

#endif // MEDIAEXTENSIONS_H
