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
/// HLS 播放列表本身不是单文件：HlsCacheBuilder 在播放时后台下载全部分片，
/// 合并后无损转封装成无后缀 MP4，并把真实扩展名写成 ".mp4"。
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

/// HLS 播放缓存里可用于“下载续传”的视频轨现场。
/// 下载入口只复用已完成分片的前缀，下载任务重新解析播放列表后还会用
/// fingerprint 校验是不是同一份分片结构；不一致时丢弃这段种子重新下载。
struct HlsTrackSeed
{
    /// 条目在 media-cache 中的正文路径，用于下载完成后清理整条播放缓存。
    QString bodyPath;
    /// 轨道固定目标路径（media.ts / media.mp4）。
    QString trackPath;
    /// 实际可复制的数据文件：未完成时是 `.mvpdownload`，完成时是轨道本体。
    QString dataPath;
    qint64 bytes = 0;
    int segmentIndex = 0;
    int segmentCount = 0;
    QString fingerprint;
    bool completed = false;
};

QString rootDir();
/// 条目在缓存目录里的安全基础名（不含 (2)/(3) 冲突后缀）。
/// 歌词缓存等旁车缓存复用它，保证同一首歌在音乐缓存和歌词缓存里名字一致。
QString entryBaseName(const MediaItem &item);
/// 缓存归属身份。不能直接用 mediaIdentity：在线直链的查询串里常常才是
/// 真正的歌曲/视频 id，而 mediaIdentity 为了去重会把查询串整个丢掉。
QString cacheIdentity(const MediaItem &item);
/// 条目缓存正文所在的目录（音乐为缓存根，影视为条目文件夹）。
QString directoryForItem(const MediaItem &item);
/// 条目对应的缓存正文路径；正文不带后缀。
QString filePathForItem(const MediaItem &item);
QString metaPathForFile(const QString &cachePath);
/// 未完成 HLS 缓存的归属标记：正文旁车 JSON 只在完成后写入，退出时还没有
/// 完成元数据；没有这个标记，下次会因目录非空而错误跳到 " (2)" 新目录。
QString partialStatePathForFile(const QString &cachePath);
bool writePartialIdentity(const QString &cachePath, const QString &identity);
QString readPartialIdentity(const QString &cachePath);
bool removePartialIdentity(const QString &cachePath);

/// 删除条目自己的缓存（音乐删正文与 JSON，影视删整个文件夹）；
/// 不存在视为成功。Windows 上文件被播放器占用时会失败，调用方可延迟重试。
bool removeForItem(const MediaItem &item);
/// 按缓存正文路径精确删除该条缓存，不按条目身份重新扫描。
/// 只允许 media-cache 根目录下的路径；影视条目删空父目录，但不会碰
/// lyrics-cache 等其他旁车缓存。文件不存在视为成功。
bool removeCachePath(const QString &cachePath);

/// 只有可按字节 Range 读取的在线直链才适合整段落盘。
bool isCacheable(const MediaItem &item);
qint64 fileSize(const QString &cachePath);
bool readState(const QString &cachePath, State *state);
/// 写入旁车元数据；缓存数据本身由 StreamProxy 负责追加。
/// extension 为空时按 URL/Content-Type 推断；HLS 合并转封装后必须显式传 ".mp4"，
/// 否则 m3u8 地址会被推断成 ".m3u8" 或 ".ts"。
bool writeState(const QString &cachePath, const QString &identity, const QUrl &url,
                const QByteArray &contentType, qint64 totalBytes, bool complete,
                const QString &extension = QString());
/// 已完成缓存才算可离线播放：要求元数据里有总长，且文件大小正好等于总长。
/// 数据已完整但元数据来不及标 complete（例如刚写完就崩溃）也按完成处理。
QString completeFileForItem(const MediaItem &item);
/// 有部分数据但尚未完整时返回缓存路径；完整时返回空，让调用方优先用完整文件。
QString partialFileForItem(const MediaItem &item);
/// 读取 HLS 影视播放缓存的视频轨续传现场，供手动下载复用已完成分片。
/// 没有续传状态、分片数量为 0 或缺少播放列表指纹时返回 false；缺少指纹
/// 无法保证新旧分片属于同一条时间线，宁可从头下载也不能拼接。
bool hlsTrackSeedForItem(const MediaItem &item, HlsTrackSeed *seed);

} // namespace MediaCache

#endif // MEDIACACHE_H
