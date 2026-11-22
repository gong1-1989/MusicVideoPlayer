#ifndef STREAMTOKENREGISTRY_H
#define STREAMTOKENREGISTRY_H

#include <QHash>
#include <QString>
#include <QUrl>

/// 本地流转 token 注册表。
///
/// 播放器只能拿到 /stream/<token>/...，由 StreamProxy 用 token 反查上游地址和
/// 该源需要的请求头。token 只在 127.0.0.1 上有意义，不对外暴露。
class StreamTokenRegistry
{
public:
    struct Upstream
    {
        QUrl url;
        QHash<QString, QString> headers;
        qint64 lastUsedMs = 0;
        QString cacheKey;
        /// 该条目对应的整段磁盘缓存路径；HLS 播放列表/分片为空。
        QString cachePath;
    };

    void clear();
    int size() const { return m_tokens.size(); }

    /// 地址 + 鉴权头的稳定键；缓存和 token 复用都必须用同一套规则。
    static QString cacheKeyFor(const QUrl &upstream, const QHash<QString, QString> &headers);

    /// 登记上游地址并返回本地代理目录 URL；同一地址+同一组头会复用 token。
    /// 监听端口无效时返回空 URL。cachePath 只对可直接整段落盘的直链生效。
    QUrl registerFor(const QUrl &upstream, const QHash<QString, QString> &headers, int port,
                     const QString &cachePath = QString());

    /// 按 token 取回上游并刷新空闲时间；未知 token 返回 false。
    bool lookup(const QString &token, Upstream *upstream);

    /// 过期清理：512 条以下每 64 次扫一遍，空闲 2 小时后过期，
    /// 极端增长时丢最老四分之一。
    void prune();
    /// 测试用：按指定时间做一次完整过期扫描，不受“每 64 次”计数器限制。
    void pruneAt(qint64 nowMs);

private:
    QHash<QString, Upstream> m_tokens;
    QHash<QString, QString> m_tokenByKey;
    int m_pruneCounter = 0;
};

#endif // STREAMTOKENREGISTRY_H
