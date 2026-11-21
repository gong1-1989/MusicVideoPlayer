#ifndef NETLIMITS_H
#define NETLIMITS_H

#include <QtGlobal>

/// 网络响应体与解析输出的统一上限。
/// 只限制会整体读进内存的文本/JSON；音视频流式转发的分片不受这里约束。
namespace NetLimits {
inline constexpr qint64 kJsonBytes = 2LL * 1024 * 1024;
inline constexpr qint64 kHtmlBytes = 4LL * 1024 * 1024;
inline constexpr qint64 kPlaylistBytes = 4LL * 1024 * 1024;
inline constexpr int kPlaylistSegments = 10000;
inline constexpr qint64 kDownloadBytes = 8LL * 1024 * 1024 * 1024;
} // namespace NetLimits

#endif // NETLIMITS_H
