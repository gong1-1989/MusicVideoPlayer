#ifndef HEADERPOLICY_H
#define HEADERPOLICY_H

#include <QHash>
#include <QString>
#include <QUrl>

/// 所有网络请求共用的一套请求头安全策略。
///
/// 源配置里的 Cookie/Authorization 只应该发给源站；m3u8 分片、详情接口、
/// 下载 CDN 经常在另一个 host，如果原样转发就会把凭据带给第三方。
namespace HeaderPolicy {

inline bool isSensitiveHeader(const QString &name)
{
    const QString lower = name.toLower();
    return lower == QLatin1String("cookie")
        || lower == QLatin1String("authorization")
        || lower == QLatin1String("proxy-authorization")
        || lower == QLatin1String("x-api-key")
        || lower == QLatin1String("x-auth-token");
}

inline QHash<QString, QString> withoutSensitiveHeaders(const QHash<QString, QString> &headers)
{
    QHash<QString, QString> safe;
    safe.reserve(headers.size());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (!isSensitiveHeader(it.key()))
            safe.insert(it.key(), it.value());
    }
    return safe;
}

/// 请求头里能代表“源站”的地址：优先 Referer，其次 Origin。
inline QUrl originFromHeaders(const QHash<QString, QString> &headers)
{
    const QUrl referer(headers.value(QStringLiteral("Referer")));
    if (!referer.isEmpty() && referer.isValid() && !referer.host().isEmpty())
        return referer;

    const QUrl origin(headers.value(QStringLiteral("Origin")));
    if (!origin.isEmpty() && origin.isValid() && !origin.host().isEmpty())
        return origin;

    return QUrl();
}

/// 按目标地址过滤敏感头。
/// - origin 为空时从 Referer/Origin 推断；
/// - 目标 host 与源站不同：剥离 Cookie/Authorization 等；
/// - 源站是 https、目标降级为 http：同 host 也剥离；
/// - 其余情况保留（例如同一个 host 的播放列表和详情请求）。
inline QHash<QString, QString> sanitizedForTarget(const QUrl &target,
                                                  const QHash<QString, QString> &headers,
                                                  const QUrl &origin = QUrl())
{
    if (headers.isEmpty())
        return headers;

    QUrl base = origin;
    if (base.isEmpty() || !base.isValid() || base.host().isEmpty())
        base = originFromHeaders(headers);

    // 没有任何源站信息时保持原样，避免把旧配置的 Referer-only 请求误伤。
    if (base.isEmpty() || !base.isValid() || base.host().isEmpty())
        return headers;

    const bool crossHost = target.host().compare(base.host(), Qt::CaseInsensitive) != 0;
    const bool insecureDowngrade = base.scheme().compare(QLatin1String("https"),
                                                          Qt::CaseInsensitive) == 0
        && target.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0;
    if (!crossHost && !insecureDowngrade)
        return headers;

    return withoutSensitiveHeaders(headers);
}

/// 持久化播放列表时去掉不应该落盘的凭据头。
inline QHash<QString, QString> persistedHeaders(const QHash<QString, QString> &headers)
{
    return withoutSensitiveHeaders(headers);
}

} // namespace HeaderPolicy

#endif // HEADERPOLICY_H
