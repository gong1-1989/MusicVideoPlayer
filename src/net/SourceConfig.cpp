#include "net/SourceConfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QUrl>

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

    if (kind == QLatin1String("builtin")) {
        // 内置实现见 createSearchProvider()：目前只剩网页型影视（内置抓取）
        if (provider == QLatin1String("maccms_web"))
            return !urlTemplate.trimmed().isEmpty();
        return false;
    }

    if (kind == QLatin1String("external")) {
        // 命令可以是绝对路径、PATH 里的程序名，或相对程序目录的脚本
        return !command.trimmed().isEmpty();
    }

    const QString scheme = QUrl(urlTemplate).scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    // 没有播放地址就没法用：要么搜索结果里直接给，要么二次解析详情里给
    const bool streamFromList = !fields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    const bool streamFromDetail = needsDetailFetch()
                                  && !detailFields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    if (!streamFromList && !streamFromDetail)
        return false;
    if (fields.value(QStringLiteral("title")).trimmed().isEmpty())
        return false;
    return true;
}

QString SourceConfig::validationError() const
{
    if (id.trimmed().isEmpty())
        return QObject::tr("缺少 id");
    if (name.trimmed().isEmpty())
        return QObject::tr("缺少名称");

    if (kind == QLatin1String("builtin")) {
        if (provider == QLatin1String("maccms_web")) {
            if (urlTemplate.trimmed().isEmpty())
                return QObject::tr("网页型影视源没填站点地址");
            const QString scheme = QUrl(urlTemplate).scheme();
            if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
                return QObject::tr("站点地址不是 http/https");
            return QString();
        }
        return QObject::tr("内置源类型未知：%1").arg(provider);
    }

    if (kind == QLatin1String("external")) {
        if (command.trimmed().isEmpty())
            return QObject::tr("外部解析器没填命令");
        return QString();
    }

    if (urlTemplate.trimmed().isEmpty())
        return QObject::tr("搜索地址为空");

    const QString scheme = QUrl(urlTemplate).scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QObject::tr("搜索地址不是 http/https");

    if (fields.value(QStringLiteral("title")).trimmed().isEmpty())
        return QObject::tr("缺少标题字段");
    const bool streamFromList = !fields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    const bool streamFromDetail = needsDetailFetch()
                                  && !detailFields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    if (!streamFromList && !streamFromDetail)
        return QObject::tr("缺少播放地址字段（可直接给 streamUrl，或用二次解析从详情里取）");
    if (needsDetailFetch()) {
        const QString detailScheme = QUrl(detailUrlTemplate).scheme();
        if (detailScheme != QLatin1String("http") && detailScheme != QLatin1String("https"))
            return QObject::tr("详情地址模板不是 http/https");
    }
    return QString();
}

QString SourceConfig::typeLabel() const
{
    return mediaType == MediaType::Video ? QObject::tr("视频") : QObject::tr("音乐");
}

QString SourceConfig::kindLabel() const
{
    if (kind == QLatin1String("builtin"))
        return QObject::tr("内置");
    if (kind == QLatin1String("external"))
        return QObject::tr("外部解析器");
    return QObject::tr("配置");
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
    object.insert(QStringLiteral("kind"), config.kind);
    if (!config.provider.isEmpty())
        object.insert(QStringLiteral("provider"), config.provider);
    if (config.kind == QLatin1String("external")) {
        object.insert(QStringLiteral("command"), config.command);
        object.insert(QStringLiteral("arguments"), config.arguments);
        object.insert(QStringLiteral("timeoutMs"), config.timeoutMs);
    }
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
    if (config.streamKind != QLatin1String("direct"))
        object.insert(QStringLiteral("streamKind"), config.streamKind);
    if (!config.skipIf.isEmpty())
        object.insert(QStringLiteral("skipIf"), config.skipIf);
    if (config.needsDetailFetch()) {
        object.insert(QStringLiteral("detailUrl"), config.detailUrlTemplate);
        if (!config.detailItemsPath.isEmpty())
            object.insert(QStringLiteral("detailItemsPath"), config.detailItemsPath);
        if (!config.detailFields.isEmpty())
            object.insert(QStringLiteral("detailFields"), headersToJson(config.detailFields));
    }
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
    config.kind = object.value(QStringLiteral("kind")).toString(QStringLiteral("config"));
    config.provider = object.value(QStringLiteral("provider")).toString();
    config.command = object.value(QStringLiteral("command")).toString();
    config.arguments = object.value(QStringLiteral("arguments")).toString();
    config.timeoutMs = object.value(QStringLiteral("timeoutMs")).toInt(8000);
    if (config.timeoutMs < 500)
        config.timeoutMs = 8000;
    config.method = object.value(QStringLiteral("method")).toString(QStringLiteral("GET")).toUpper();
    config.urlTemplate = object.value(QStringLiteral("url")).toString();
    config.bodyTemplate = object.value(QStringLiteral("body")).toString();
    config.bodyIsJson = object.value(QStringLiteral("bodyIsJson")).toBool(false);
    config.headers = headersFromJson(object.value(QStringLiteral("headers")).toObject());
    config.cookie = object.value(QStringLiteral("cookie")).toString();
    config.itemsPath = object.value(QStringLiteral("itemsPath")).toString();
    config.fields = headersFromJson(object.value(QStringLiteral("fields")).toObject());
    config.streamKind = object.value(QStringLiteral("streamKind"))
                            .toString(QStringLiteral("direct"));
    if (config.streamKind != QLatin1String("episodes"))
        config.streamKind = QStringLiteral("direct");
    config.skipIf = object.value(QStringLiteral("skipIf")).toObject();
    config.detailUrlTemplate = object.value(QStringLiteral("detailUrl")).toString();
    config.detailItemsPath = object.value(QStringLiteral("detailItemsPath")).toString();
    config.detailFields =
        headersFromJson(object.value(QStringLiteral("detailFields")).toObject());

    if (ok != nullptr)
        *ok = config.isValid();
    return config;
}
