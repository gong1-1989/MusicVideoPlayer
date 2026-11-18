#include "net/SourceConfig.h"

#include <QJsonArray>

namespace {

QJsonObject headersToJson(const QHash<QString, QString> &headers)
{
    QJsonObject object;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (!it.key().trimmed().isEmpty())
            object.insert(it.key().trimmed(), it.value());
    }
    return object;
}

QHash<QString, QString> headersFromJson(const QJsonObject &object)
{
    QHash<QString, QString> headers;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        headers.insert(it.key(), it.value().toString());
    return headers;
}

} // namespace

bool SourceConfig::isValid() const
{
    if (id.trimmed().isEmpty() || name.trimmed().isEmpty())
        return false;
    const QString scheme = QUrl(urlTemplate).scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    // 没有播放地址就没法用：配置里必须给出 streamUrl
    if (fields.value(QStringLiteral("streamUrl")).trimmed().isEmpty())
        return false;
    if (fields.value(QStringLiteral("title")).trimmed().isEmpty())
        return false;
    return true;
}

QString SourceConfig::typeLabel() const
{
    return mediaType == MediaType::Video ? QObject::tr("视频") : QObject::tr("音乐");
}

QJsonObject sourceConfigToJson(const SourceConfig &config)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), config.id);
    object.insert(QStringLiteral("name"), config.name);
    object.insert(QStringLiteral("enabled"), config.enabled);
    object.insert(QStringLiteral("builtin"), config.builtin);
    object.insert(QStringLiteral("mediaType"),
                  config.mediaType == MediaType::Video ? QStringLiteral("video")
                                                       : QStringLiteral("music"));
    object.insert(QStringLiteral("method"), config.method.toUpper());
    object.insert(QStringLiteral("url"), config.urlTemplate);
    if (!config.bodyTemplate.isEmpty()) {
        object.insert(QStringLiteral("body"), config.bodyTemplate);
        object.insert(QStringLiteral("bodyIsJson"), config.bodyIsJson);
    }
    if (!config.headers.isEmpty())
        object.insert(QStringLiteral("headers"), headersToJson(config.headers));
    if (!config.cookie.isEmpty())
        object.insert(QStringLiteral("cookie"), config.cookie);
    object.insert(QStringLiteral("itemsPath"), config.itemsPath);
    object.insert(QStringLiteral("fields"), headersToJson(config.fields));
    if (!config.skipIf.isEmpty())
        object.insert(QStringLiteral("skipIf"), config.skipIf);
    return object;
}

SourceConfig sourceConfigFromJson(const QJsonObject &object, bool *ok)
{
    SourceConfig config;
    config.id = object.value(QStringLiteral("id")).toString();
    config.name = object.value(QStringLiteral("name")).toString();
    config.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    config.builtin = object.value(QStringLiteral("builtin")).toBool(false);
    config.mediaType = object.value(QStringLiteral("mediaType")).toString() == QLatin1String("video")
                           ? MediaType::Video
                           : MediaType::Music;
    config.method = object.value(QStringLiteral("method")).toString(QStringLiteral("GET")).toUpper();
    config.urlTemplate = object.value(QStringLiteral("url")).toString();
    config.bodyTemplate = object.value(QStringLiteral("body")).toString();
    config.bodyIsJson = object.value(QStringLiteral("bodyIsJson")).toBool(false);
    config.headers = headersFromJson(object.value(QStringLiteral("headers")).toObject());
    config.cookie = object.value(QStringLiteral("cookie")).toString();
    config.itemsPath = object.value(QStringLiteral("itemsPath")).toString();
    config.fields = headersFromJson(object.value(QStringLiteral("fields")).toObject());
    config.skipIf = object.value(QStringLiteral("skipIf")).toObject();

    if (ok != nullptr)
        *ok = config.isValid();
    return config;
}
