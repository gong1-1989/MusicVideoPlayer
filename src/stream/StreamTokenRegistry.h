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
        qint64 createdAtMs = 0;
    };

    void clear();
    int size() const { return m_tokens.size(); }

    /// 登记上游地址并返回本地代理目录 URL；监听端口无效时返回空 URL。
    QUrl registerFor(const QUrl &upstream, const QHash<QString, QString> &headers, int port);

    /// 按 token 取回上游；未知 token 返回 false。
    bool lookup(const QString &token, Upstream *upstream) const;

    /// 过期清理：512 条以下不扫，6 小时后过期，极端增长时丢最老四分之一。
    void prune();

private:
    QHash<QString, Upstream> m_tokens;
};

#endif // STREAMTOKENREGISTRY_H
