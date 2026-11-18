#include "net/ConfigSearchProvider.h"

#include "net/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QUrl>

Q_LOGGING_CATEGORY(lcProvider, "mvp.net.provider")

namespace {

/// 支持 a.b[0].c 形式的路径取值
QJsonValue valueAtPath(const QJsonValue &root, const QString &path)
{
    if (path.trimmed().isEmpty())
        return root;

    QJsonValue current = root;
    QString rest = path.trimmed();

    while (!rest.isEmpty()) {
        if (!rest.startsWith(QLatin1Char('['))) {
            int dot = rest.indexOf(QLatin1Char('.'));
            int bracket = rest.indexOf(QLatin1Char('['));
            int stop = dot;
            if (bracket >= 0 && (stop < 0 || bracket < stop))
                stop = bracket;
            const QString key = stop < 0 ? rest : rest.left(stop);
            if (!current.isObject())
                return QJsonValue();
            current = current.toObject().value(key);
            if (stop < 0)
                return current;
            rest = rest.mid(stop);
        }
        if (rest.startsWith(QLatin1Char('['))) {
            int close = rest.indexOf(QLatin1Char(']'));
            if (close < 0)
                return QJsonValue();
            bool ok = false;
            const int index = rest.mid(1, close - 1).toInt(&ok);
            if (!ok || !current.isArray())
                return QJsonValue();
            const QJsonArray array = current.toArray();
            if (index < 0 || index >= array.size())
                return QJsonValue();
            current = array.at(index);
            rest = rest.mid(close + 1);
        }
        if (rest.startsWith(QLatin1Char('.')))
            rest = rest.mid(1);
    }
    return current;
}

QString renderTemplate(QString text, const QHash<QString, QString> &vars)
{
    for (auto it = vars.cbegin(); it != vars.cend(); ++it) {
        text.replace(QStringLiteral("{") + it.key() + QStringLiteral("}"), it.value());
    }
    return text;
}

double toNumber(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString())
        return value.toString().toDouble();
    return 0.0;
}

bool ruleMatches(const QJsonValue &item, const QJsonObject &rule)
{
    if (rule.isEmpty())
        return false;

    if (rule.contains(QStringLiteral("any"))) {
        const QJsonArray any = rule.value(QStringLiteral("any")).toArray();
        for (const QJsonValue &entry : any) {
            if (entry.isObject() && ruleMatches(item, entry.toObject()))
                return true;
        }
        return false;
    }
    if (rule.contains(QStringLiteral("all"))) {
        const QJsonArray all = rule.value(QStringLiteral("all")).toArray();
        for (const QJsonValue &entry : all) {
            if (!entry.isObject() || !ruleMatches(item, entry.toObject()))
                return false;
        }
        return true;
    }

    const QString path = rule.value(QStringLiteral("path")).toString();
    const QString op = rule.value(QStringLiteral("op")).toString(QStringLiteral("eq"));
    const QJsonValue expected = rule.value(QStringLiteral("value"));
    const QJsonValue actual = valueAtPath(item, path);

    if (op == QLatin1String("empty"))
        return actual.isUndefined() || actual.isNull()
               || (actual.isString() && actual.toString().isEmpty());
    if (op == QLatin1String("notEmpty"))
        return !(actual.isUndefined() || actual.isNull()
                 || (actual.isString() && actual.toString().isEmpty()));

    if (op == QLatin1String("eq") || op == QLatin1String("ne")) {
        bool equal = false;
        if (expected.isDouble() || expected.isBool() || actual.isDouble() || actual.isBool())
            equal = toNumber(actual) == toNumber(expected);
        else
            equal = actual.toString() == expected.toString();
        return op == QLatin1String("eq") ? equal : !equal;
    }
    if (op == QLatin1String("gt"))
        return toNumber(actual) > toNumber(expected);
    if (op == QLatin1String("gte"))
        return toNumber(actual) >= toNumber(expected);
    if (op == QLatin1String("lt"))
        return toNumber(actual) < toNumber(expected);
    if (op == QLatin1String("lte"))
        return toNumber(actual) <= toNumber(expected);
    if (op == QLatin1String("contains"))
        return actual.toString().contains(expected.toString(), Qt::CaseInsensitive);

    qCWarning(lcProvider) << "unknown rule op" << op;
    return false;
}

QString mapString(const QJsonValue &node, const QHash<QString, QString> &fields,
                  const QString &key, const QHash<QString, QString> &vars)
{
    const QString spec = fields.value(key);
    if (spec.trimmed().isEmpty())
        return QString();
    if (spec.startsWith(QLatin1Char('@')))
        return renderTemplate(spec.mid(1), vars);
    return valueAtPath(node, spec).toVariant().toString();
}

} // namespace

ConfigSearchProvider::ConfigSearchProvider(const SourceConfig &config, HttpClient *http,
                                           QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_http(http)
{
}

void ConfigSearchProvider::search(const QString &keyword, int limit)
{
    m_keyword = keyword;
    m_limit = limit > 0 ? limit : 20;
    m_done = false;

    if (m_http == nullptr) {
        emit finished(m_config.id, {}, 0, tr("网络模块未就绪"));
        return;
    }
    if (!m_config.isValid()) {
        emit finished(m_config.id, {}, 0, tr("搜索源配置不完整"));
        return;
    }

    QHash<QString, QString> vars;
    vars.insert(QStringLiteral("keyword"), QString::fromUtf8(QUrl::toPercentEncoding(keyword)));
    vars.insert(QStringLiteral("limit"), QString::number(m_limit));

    const QString url = renderTemplate(m_config.urlTemplate, vars);
    const QUrl requestUrl(url);
    if (!requestUrl.isValid()) {
        emit finished(m_config.id, {}, 0, tr("搜索地址无效"));
        return;
    }

    QHash<QString, QString> headers = m_config.headers;
    if (!m_config.cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), m_config.cookie);

    qCInfo(lcProvider) << "search" << m_config.id << requestUrl.toString();

    if (m_config.method == QLatin1String("POST")) {
        const QByteArray body = renderTemplate(m_config.bodyTemplate, vars).toUtf8();
        if (m_config.bodyIsJson) {
            headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
            m_reply = m_http->postJson(requestUrl, body, headers);
        } else {
            m_reply = m_http->postForm(requestUrl, body, headers);
        }
    } else {
        m_reply = m_http->get(requestUrl, QByteArray(), headers);
    }

    if (m_reply.isNull()) {
        emit finished(m_config.id, {}, 0, tr("请求未发出"));
        return;
    }
    connect(m_reply.data(), &QNetworkReply::finished, this,
            &ConfigSearchProvider::onReplyFinished);
}

void ConfigSearchProvider::onReplyFinished()
{
    if (m_done)
        return;
    m_done = true;

    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    if (reply == nullptr) {
        emit finished(m_config.id, {}, 0, tr("响应丢失"));
        return;
    }
    reply->deleteLater();

    if (!HttpClient::isHttpSuccess(reply)) {
        const QString error = HttpClient::describeError(reply);
        qCWarning(lcProvider) << m_config.id << "failed:" << error;
        emit finished(m_config.id, {}, 0, error);
        return;
    }

    const QByteArray payload = reply->readAll();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcProvider) << m_config.id << "not json:" << parseError.errorString()
                              << payload.left(120);
        emit finished(m_config.id, {}, 0, tr("响应不是 JSON 数据"));
        return;
    }

    const QJsonValue root = document.isArray() ? QJsonValue(document.array())
                                               : QJsonValue(document.object());
    const QJsonValue itemsValue = valueAtPath(root, m_config.itemsPath);
    if (!itemsValue.isArray()) {
        qCWarning(lcProvider) << m_config.id << "items path not array:" << m_config.itemsPath;
        emit finished(m_config.id, {}, 0,
                      tr("找不到结果路径“%1”").arg(m_config.itemsPath));
        return;
    }

    const QJsonArray items = itemsValue.toArray();
    QVector<MediaItem> result;
    result.reserve(items.size());
    int filtered = 0;

    for (const QJsonValue &node : items) {
        if (!node.isObject()) {
            ++filtered;
            continue;
        }
        if (!m_config.skipIf.isEmpty() && ruleMatches(node, m_config.skipIf)) {
            ++filtered;
            continue;
        }

        QHash<QString, QString> vars;
        vars.insert(QStringLiteral("keyword"), m_keyword);
        vars.insert(QStringLiteral("id"), mapString(node, m_config.fields,
                                                    QStringLiteral("id"), vars));
        vars.insert(QStringLiteral("title"), mapString(node, m_config.fields,
                                                      QStringLiteral("title"), vars));
        vars.insert(QStringLiteral("artist"), mapString(node, m_config.fields,
                                                       QStringLiteral("artist"), vars));

        const QString stream = mapString(node, m_config.fields, QStringLiteral("streamUrl"), vars);
        if (stream.trimmed().isEmpty()) {
            ++filtered;
            continue;
        }

        MediaItem item;
        item.type = m_config.mediaType;
        item.title = vars.value(QStringLiteral("title")).trimmed();
        item.artist = vars.value(QStringLiteral("artist"));
        item.url = QUrl(stream);
        item.lyricUrl = QUrl(mapString(node, m_config.fields, QStringLiteral("lyricUrl"), vars));
        item.webUrl = QUrl(mapString(node, m_config.fields, QStringLiteral("webUrl"), vars));
        item.sourceId = m_config.id;
        item.headers = m_config.headers;
        item.online = true;

        const QString durationSpec = m_config.fields.value(QStringLiteral("durationMs"));
        if (!durationSpec.isEmpty() && !durationSpec.startsWith(QLatin1Char('@'))) {
            const double raw = toNumber(valueAtPath(node, durationSpec));
            item.durationMs = static_cast<qint64>(raw);
        }

        if (item.title.isEmpty())
            item.title = item.url.fileName();

        result.append(item);
    }

    qCInfo(lcProvider) << m_config.id << "parsed" << result.size() << "items, filtered"
                       << filtered;
    emit finished(m_config.id, result, filtered, QString());
}
