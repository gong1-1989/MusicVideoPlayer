#ifndef MEDIACACHE_H
#define MEDIACACHE_H

#include "media/MediaItem.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

/// 在线条目的整段磁盘缓存。
///
/// 布局由用户直接管理：
/// - 音乐：`media-cache/<条目名>`，一个条目一个文件；
/// - 影视：`media-cache/<条目名>/media`，一个条目一个文件夹；
/// - 正文一律不带后缀，真实扩展名记录在紧邻的 `<正文>.json` 里。
/// 同名条目按 `(2)`、`(3)` 自动区分，条目身份也记在 JSON 中，避免张冠李戴。
/// HLS 是分片索引而不是单文件，暂不在这里落盘，仍走 StreamProxy 的分片内存缓存。
namespace MediaCache {

struct State
{
    QString identity;
    QUrl url;
    QString contentType;
    /// 带点的小写扩展名，例如 ".mp3"；仅用于下载时给目标文件命名。
    QString extension;
    qint64 totalBytes = -1;
    bool complete = false;
    qint64 updatedAtMs = 0;
};

QString rootDir();
/// 缓存归属身份。不能直接用 mediaIdentity：在线直链的查询串里常常才是
/// 真正的歌曲/视频 id，而 mediaIdentity 为了去重会把查询串整个丢掉。
QString cacheIdentity(const MediaItem &item);
/// 条目缓存正文所在的目录（音乐为缓存根，影视为条目文件夹）。
QString directoryForItem(const MediaItem &item);
/// 条目对应的缓存正文路径；正文不带后缀。
QString filePathForItem(const MediaItem &item);
QString metaPathForFile(const QString &cachePath);
QString extensionForUrl(const QUrl &url, const QByteArray &contentType = QByteArray());

/// 删除条目自己的缓存（音乐删正文与 JSON，影视删整个文件夹）；
/// 不存在视为成功。Windows 上文件被播放器占用时会失败，调用方可延迟重试。
bool removeForItem(const MediaItem &item);

/// 只有可按字节 Range 读取的在线直链才适合整段落盘。
bool isCacheable(const MediaItem &item);
qint64 fileSize(const QString &cachePath);
bool readState(const QString &cachePath, State *state);
/// 写入旁车元数据；缓存数据本身由 StreamProxy 负责追加。
bool writeState(const QString &cachePath, const QString &identity, const QUrl &url,
                const QByteArray &contentType, qint64 totalBytes, bool complete);
/// 已完成缓存才算可离线播放：要求元数据里有总长，且文件大小正好等于总长。
/// 数据已完整但元数据来不及标 complete（例如刚写完就崩溃）也按完成处理。
QString completeFileForItem(const MediaItem &item);
/// 有部分数据但尚未完整时返回缓存路径；完整时返回空，让调用方优先用完整文件。
QString partialFileForItem(const MediaItem &item);

} // namespace MediaCache

#endif // MEDIACACHE_H
