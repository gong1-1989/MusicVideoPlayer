#ifndef STREAMCACHESTORE_H
#define STREAMCACHESTORE_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QUrl>

class QNetworkReply;
class QFile;
class QTcpSocket;

/// Range 默认连接数，和 StreamProxy 的分片策略保持一致。
inline constexpr int kStreamCacheDefaultRangeConnections = 4;

/// 一条上游响应在本地保存的副本（内存或在媒体缓存目录里的单文件）。
/// 上游忽略 Range 时，拖动请求由这里的缓冲区/文件切片返回 206。
struct StreamCacheEntry
{
    QByteArray data;
    /// 整段落盘缓存时，data 不保存正文，实际字节在 file 指向的文件里。
    /// dataSize 同时覆盖内存与文件两种模式，避免各处混用 data.size()。
    bool diskBacked = false;
    QString filePath;
    /// 写入旁车 JSON 的条目身份；代理本身不认识播放列表，只能由调用方带入。
    QString cacheIdentity;
    QUrl upstream;
    qint64 dataSize = 0;
    QFile *file = nullptr;
    qint64 total = -1;
    /// 已登记但还没收到的字节数。pending 占位和单连接在途响应都按 total
    /// 预留配额，避免多个未完成条目绕过 kMaxCacheTotalBytes。
    qint64 reservedBytes = 0;
    bool complete = false;
    bool failed = false;
    // 只用于把已有半截文件的前缀切片交给播放器，不参与后续缓存命中。
    // 客户端发送完毕后必须从缓存表移除，否则会留下一个没有 upstream 的
    // 半截条目，下一次整段请求会停在本地前缀后一直等不到数据。
    bool transient = false;
    // 预取请求已占位但响应头还没到。播放器这时命中缓存不能另开一条上游，
    // 否则同一片会重复下载，反而把刚腾出的带宽又吃掉。
    bool pending = false;
    /// 分片占位是否已经真正发起了上游请求。占位插入和 `m_http->stream`
    /// 之间没有事件循环，用它区分“先到者已开请求”与“后来者只拿到占位”。
    bool upstreamStarted = false;
    QByteArray contentType;
    qint64 lastUsedMs = 0;
    QNetworkReply *reply = nullptr;
    /// 上游连接被 CDN/代理中途重置时，按已缓存长度重新发 Range。
    /// 限制次数，避免源站持续拒绝时形成无限重试。
    int resumeAttempts = 0;
    QList<QPointer<QTcpSocket>> clients;
    // 多连接 Range 下载：各分片先按起始偏移暂存，凑齐连续前缀后再并入
    // data 供播放器顺序读取。rangeReplies 用来在换片/淘汰时统一中止。
    bool multiRange = false;
    // 预取条目由 HLS 流水预取创建，完成后要归还预取并发槽位。
    bool prefetch = false;
    int rangeConnections = kStreamCacheDefaultRangeConnections;
    QHash<qint64, QByteArray> rangeParts;
    QHash<qint64, qint64> rangePartEnds;
    QSet<qint64> rangePending;
    QSet<qint64> rangeRetried;
    QList<QPointer<QNetworkReply>> rangeReplies;

    /// 已拿到上游总长且没有失败，可以作为缓存命中候选继续服务/等待。
    bool isUsableCache() const { return total >= 0 && !failed; }
    /// 已登记但还没真正发出上游请求的 HLS 分片占位。
    bool isPendingPlaceholder() const { return pending && !upstreamStarted; }
    /// 多连接 Range 分片仍在拼接，失败后不再接受新的分片回调。
    bool isMultiRangeActive() const { return multiRange && !failed; }
};

/// 缓存条目容器。只负责存取和统计，网络回调与 socket 转发仍由 StreamProxy 管。
class StreamCacheStore
{
public:
    using Entry = StreamCacheEntry;
    using value_type = QSharedPointer<Entry>;
    using const_iterator = QHash<QString, value_type>::const_iterator;
    using iterator = QHash<QString, value_type>::iterator;

    void clear() { m_entries.clear(); }
    int size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }

    value_type value(const QString &key) const { return m_entries.value(key); }
    bool contains(const QString &key) const { return m_entries.contains(key); }
    void insert(const QString &key, const value_type &entry) { m_entries.insert(key, entry); }
    int remove(const QString &key) { return m_entries.remove(key); }
    value_type take(const QString &key) { return m_entries.take(key); }

    const_iterator cbegin() const { return m_entries.cbegin(); }
    const_iterator cend() const { return m_entries.cend(); }
    const_iterator begin() const { return m_entries.cbegin(); }
    const_iterator end() const { return m_entries.cend(); }
    iterator begin() { return m_entries.begin(); }
    iterator end() { return m_entries.end(); }

    static qint64 bytes(const Entry &entry);

private:
    QHash<QString, value_type> m_entries;
};

#endif // STREAMCACHESTORE_H
