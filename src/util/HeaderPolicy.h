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
    // HTTP 头名允许 `-` 和 `_` 混用；实际源配置里出现过 X-APIKEY、apiKey、
    // X_API_KEY 等写法。先压掉分隔符再比较，避免这些变体绕过跨域裁剪。
    QString compact = lower;
    compact.remove(QLatin1Char('-'));
    compact.remove(QLatin1Char('_'));
    return lower == QLatin1String("cookie")
        || lower == QLatin1String("cookie2")
        || lower == QLatin1String("authorization")
        || lower == QLatin1String("proxy-authorization")
        || lower == QLatin1String("www-authenticate")
        || lower == QLatin1String("x-api-key")
        || lower == QLatin1String("api-key")
        || lower == QLatin1String("access-key")
        || compact == QLatin1String("xapikey")
        || compact == QLatin1String("apikey")
        || compact == QLatin1String("accesskey")
        || lower.contains(QLatin1String("token"))
        || lower.contains(QLatin1String("secret"))
        || lower.contains(QLatin1String("session"))
        || lower.contains(QLatin1String("credential"))
        || lower.endsWith(QLatin1String("-key"))
        || lower.endsWith(QLatin1String("-auth"));
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

/// 请求头名按 HTTP 语义大小写不敏感；配置里写小写也能识别。
inline QString headerValue(const QHash<QString, QString> &headers, const QString &name)
{
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return QString();
}

inline bool hasHeader(const QHash<QString, QString> &headers, const QString &name)
{
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

/// 请求头里能代表“源站”的地址：优先 Referer，其次 Origin。
inline QUrl originFromHeaders(const QHash<QString, QString> &headers)
{
    const QUrl referer(headerValue(headers, QStringLiteral("Referer")));
    if (!referer.isEmpty() && referer.isValid() && !referer.host().isEmpty())
        return referer;

    const QUrl origin(headerValue(headers, QStringLiteral("Origin")));
    if (!origin.isEmpty() && origin.isValid() && !origin.host().isEmpty())
        return origin;

    return QUrl();
}

/// 只保留 scheme + host + port，不带 path/query/userInfo。
inline QUrl originOnly(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty())
        return QUrl();
    QUrl result;
    result.setScheme(url.scheme());
    result.setHost(url.host());
    if (url.port() >= 0)
        result.setPort(url.port());
    return result;
}

inline int effectivePort(const QUrl &url)
{
    if (url.port() >= 0)
        return url.port();
    return url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
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

/// Referer 不能带 query/fragment：搜索 URL 里常有 API key，跨 host 分片
/// 也会收到 Referer，不能把完整签名地址外发。
inline QString refererHeaderValue(const QUrl &origin)
{
    if (!origin.isValid() || origin.host().isEmpty())
        return QString();
    QString value = origin.scheme() + QStringLiteral("://") + origin.host();
    if (origin.port() > 0)
        value += QLatin1Char(':') + QString::number(origin.port());
    const QString path = origin.path(QUrl::FullyEncoded);
    value += path.isEmpty() ? QStringLiteral("/") : path;
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
    if (!hasHeader(result, QStringLiteral("Referer"))) {
        const QString referer = refererHeaderValue(origin);
        if (!referer.isEmpty())
            result.insert(QStringLiteral("Referer"), referer);
    }
    if (!hasHeader(result, QStringLiteral("Origin"))) {
        const QString originValue = originHeaderValue(origin);
        if (!originValue.isEmpty())
            result.insert(QStringLiteral("Origin"), originValue);
    }
    return result;
}

/// 凭据只能发给同一 host + 同端口的源站；https→http 降级也必须裁剪。
/// 没有源站信息时返回 true，调用方应 fail closed。
inline bool shouldStripSensitive(const QUrl &target, const QUrl &origin)
{
    if (!origin.isValid() || origin.host().isEmpty())
        return true;
    if (target.host().compare(origin.host(), Qt::CaseInsensitive) != 0)
        return true;
    if (origin.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
        && target.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return effectivePort(target) != effectivePort(origin);
}

/// 按目标地址过滤敏感头。
/// - origin 为空时从 Referer/Origin 推断；
/// - 目标与源站不同 host、不同端口或 https→http 降级：剥离敏感头；
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

    if (shouldStripSensitive(target, base))
        return withoutSensitiveHeaders(headers);

    return headers;
}

/// 持久化播放列表时去掉不应该落盘的凭据头。
inline QHash<QString, QString> persistedHeaders(const QHash<QString, QString> &headers)
{
    return withoutSensitiveHeaders(headers);
}

} // namespace HeaderPolicy

#endif // HEADERPOLICY_H
