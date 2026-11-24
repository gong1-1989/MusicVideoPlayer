#ifndef HLSPRELOADINDEX_H
#define HLSPRELOADINDEX_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QUrl>

/// 一个已经完整写入合并轨道文件、可供播放代理直接读取的 HLS 分片。
struct HlsPreloadEntry
{
    QUrl url;
    QByteArray range;
    QString filePath;
    qint64 offset = 0;
    qint64 size = 0;
    QByteArray contentType;
};

/// 播放代理实际读取的切片；filePath 可能已经由 `.mvpdownload` 重命名为最终轨道。
struct HlsPreloadSlice
{
    QString filePath;
    qint64 offset = 0;
    qint64 size = 0;
    QByteArray contentType;
};

/// 后台 HLS 整段缓存的分片索引。
///
/// 后台缓存把分片合并进 `.video` / `.audio`，这里记录“条目身份 + 分片地址
/// → 文件偏移 + 长度”。播放代理在回源前查表，命中时直接读本地文件，所以
/// 已经缓存过的部分不会再次向源站下载。索引按条目录盘成 `.preload.jsonl`，
/// 下次启动仍能复用；对象内部加锁，供主线程和流转代理线程共同读取。
class HlsPreloadIndex
{
public:
    static HlsPreloadIndex &instance();

    /// 切换当前条目的索引日志并加载已有记录；空路径只清空内存索引。
    void openJournal(const QString &path);
    /// 登记一个完整分片；会追加到索引日志，进程异常退出也能恢复。
    void record(const QString &identity, const HlsPreloadEntry &entry);
    /// 批量登记；迁移旧缓存时只打开一次日志，避免几百次 open/append。
    void recordMany(const QString &identity, const QList<HlsPreloadEntry> &entries);
    /// 查询分片切片；range 为空和 `bytes=0-` 视为同一整段请求。
    bool find(const QString &identity, const QUrl &url, const QByteArray &range,
              HlsPreloadSlice *slice) const;
    /// 当前身份已经登记的分片数量。
    int count(const QString &identity) const;
    /// 当前身份在指定轨道（`.video`/`.audio`）上已登记的分片数量。
    int countTrack(const QString &identity, const QString &targetPath) const;
    /// 丢弃某条轨道（`.video`/`.audio` 目标基名）的全部索引。
    /// 分片结构变化、续传点被清零时必须调用，否则旧偏移会命中新文件里的错误字节。
    void clearTrack(const QString &targetPath);
    void clear();

private:
    HlsPreloadIndex() = default;
    static QString keyFor(const QString &identity, const QUrl &url, const QByteArray &range);
    static QByteArray normalizeRange(const QByteArray &range);
    static bool fileHasSlice(const QString &path, qint64 offset, qint64 size);
    static QString alternatePath(const QString &path);

    mutable QMutex m_mutex;
    QHash<QString, HlsPreloadSlice> m_slices;
    QString m_journalPath;
};

#endif // HLSPRELOADINDEX_H
