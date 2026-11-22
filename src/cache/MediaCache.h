#ifndef MEDIACACHE_H
#define MEDIACACHE_H

#include "media/MediaItem.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

/// 在线条目的整段磁盘缓存。
///
/// 目录按 mediaIdentity 的哈希稳定命名，所以同一播放列表条目在重启后仍能
/// 找到自己上次的缓存；缓存目录与条目一一对应，后续清理也不会串到别的条目。
/// HLS 是分片索引而不是单文件，暂不在这里落盘，仍走 StreamProxy 的分片内存缓存。
namespace MediaCache {

struct State
{
    QUrl url;
    QString contentType;
    qint64 totalBytes = -1;
    bool complete = false;
    qint64 updatedAtMs = 0;
};

QString rootDir();
QString directoryForItem(const MediaItem &item);
/// 条目对应的单文件缓存路径（扩展名尽量沿用原地址，便于 QMediaPlayer 识别）。
QString filePathForItem(const MediaItem &item);
QString metaPathForFile(const QString &cachePath);

/// 只有可按字节 Range 读取的在线直链才适合整段落盘。
bool isCacheable(const MediaItem &item);
qint64 fileSize(const QString &cachePath);
bool readState(const QString &cachePath, State *state);
/// 写入旁车元数据；缓存数据本身由 StreamProxy 负责追加。
bool writeState(const QString &cachePath, const QUrl &url, const QByteArray &contentType,
                qint64 totalBytes, bool complete);
/// 已完成缓存才算可离线播放：要求元数据里有总长，且文件大小正好等于总长。
/// 数据已完整但元数据来不及标 complete（例如刚写完就崩溃）也按完成处理。
QString completeFileForItem(const MediaItem &item);
/// 有部分数据但尚未完整时返回缓存路径；完整时返回空，让调用方优先用完整文件。
QString partialFileForItem(const MediaItem &item);

/// 启动时按目录最近使用时间淘汰，限制磁盘缓存总占用。
constexpr qint64 DefaultMaxBytes = 16LL * 1024 * 1024 * 1024;
void prune(qint64 maxBytes);

} // namespace MediaCache

#endif // MEDIACACHE_H
