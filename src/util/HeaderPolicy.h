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

/// 原始页面地址对应的 Origin 头值。
inline QString originHeaderValue(const QUrl &origin)
{
    if (!origin.isValid() || origin.host().isEmpty())
        return QString();
    QString value = origin.scheme() + QStringLiteral("://") + origin.host();
    if (origin.port() > 0)
        value += QLatin1Char(':') + QString::number(origin.port());
    return value;
}

/// 给源站请求补齐 Referer/Origin。Cookie 的作用域由 HeaderPolicy 判断，
/// 但前提是请求里能看出源站；缺少这两个头时跨域请求无法安全裁剪。
inline QHash<QString, QString> withSourceHeaders(const QHash<QString, QString> &headers,
                                                 const QUrl &origin)
{
    QHash<QString, QString> result = headers;
    if (!origin.isValid() || origin.host().isEmpty())
        return result;
    if (!result.contains(QStringLiteral("Referer")))
        result.insert(QStringLiteral("Referer"), origin.toString());
    if (!result.contains(QStringLiteral("Origin"))) {
        const QString originValue = originHeaderValue(origin);
        if (!originValue.isEmpty())
            result.insert(QStringLiteral("Origin"), originValue);
    }
    return result;
}

/// 按目标地址过滤敏感头。
/// - origin 为空时从 Referer/Origin 推断；
/// - 目标 host 与源站不同：剥离 Cookie/Authorization 等；
/// - 源站是 https、目标降级为 http：同 host 也剥离；
/// - 完全无法判断源站时也剥离敏感头（fail closed），宁可请求失败也不外发凭据。
inline QHash<QString, QString> sanitizedForTarget(const QUrl &target,
                                                  const QHash<QString, QString> &headers,
                                                  const QUrl &origin = QUrl())
{
    if (headers.isEmpty())
        return headers;

    QUrl base = origin;
    if (base.isEmpty() || !base.isValid() || base.host().isEmpty())
        base = originFromHeaders(headers);

    // 没有源站信息时不能原样放行 Cookie/Authorization：绝对 CDN 地址
    // 和重定向后的第三方 host 都会因此拿到源站凭据。
    if (base.isEmpty() || !base.isValid() || base.host().isEmpty())
        return withoutSensitiveHeaders(headers);

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
