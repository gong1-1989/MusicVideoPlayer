#include "net/ConfigSearchProvider.h"

#include "net/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QRegularExpression>
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
    if (value.isBool())
        return value.toBool() ? 1.0 : 0.0;
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
        // bool 不能和数字混算：true == false == 0 会把 audius 的
        // “is_streamable eq false” 误判成全部命中，20 条结果全被过滤。
        if (expected.isBool() || actual.isBool()) {
            equal = expected.isBool() && actual.isBool()
                    && expected.toBool() == actual.toBool();
        } else if (expected.isDouble() || actual.isDouble()) {
            equal = toNumber(actual) == toNumber(expected);
        } else {
            equal = actual.toString() == expected.toString();
        }
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

    qCWarning(lcProvider) << "未知的过滤操作符：" << op;
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

/// 接口里的标题/简介常带 HTML 标签和实体（采集类接口尤其多），统一清一遍
QString cleanText(const QString &raw)
{
    QString text = raw;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return text.simplified();
}

/// 年份可能是 "2019"、"2019-02-05"、"2019年"，统一取前四位
QString normalizeYear(const QString &raw)
{
    const QString text = cleanText(raw);
    const QString head = text.left(4);
    bool ok = false;
    const int value = head.toInt(&ok);
    if (ok && value >= 1888 && value <= 2999)
        return head;
    return text;
}

/// 协议相对地址（//i0.hdslb.com/...）补成 https
QUrl normalizeUrl(const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.startsWith(QStringLiteral("//")))
        return QUrl(QStringLiteral("https:") + text);
    return QUrl(text);
}

/// JSON 模板里的 {keyword} 必须转义引号/反斜杠，不能再做百分号编码，
/// 否则服务端收到的是 %E6%... 而不是关键词本身。
QString jsonEscape(const QString &raw)
{
    QString text = raw;
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return text;
}

struct EpisodePart
{
    QString name;
    QString url;
};

/// 采集接口的播放字段形如 "第01集$https://x/1.m3u8#第02集$https://x/2.m3u8"，
/// 多个播放源之间用 $$$ 分隔，这里只取第一组。
QVector<EpisodePart> parseEpisodes(const QString &raw)
{
    QVector<EpisodePart> parts;
    QString body = raw.trimmed();
    const int sourceSep = body.indexOf(QStringLiteral("$$$"));
    if (sourceSep >= 0)
        body = body.left(sourceSep);

    const QStringList groups = body.split(QLatin1Char('#'), Qt::SkipEmptyParts);
    for (const QString &group : groups) {
        const QString trimmed = group.trimmed();
        if (trimmed.isEmpty())
            continue;
        const int dollar = trimmed.indexOf(QLatin1Char('$'));
        const QString name = dollar > 0 ? trimmed.left(dollar).trimmed() : QString();
        const QString url = dollar >= 0 ? trimmed.mid(dollar + 1).trimmed() : trimmed;
        if (url.isEmpty())
            continue;
        parts.append(EpisodePart{name, url});
    }
    return parts;
}

} // namespace

ConfigSearchProvider::ConfigSearchProvider(const SourceConfig &config, HttpClient *http,
                                           QObject *parent)
    : SearchProviderBase(parent)
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

    qCInfo(lcProvider) << "搜索" << m_config.id << requestUrl.toString();

    if (m_config.method == QLatin1String("POST")) {
        QHash<QString, QString> bodyVars = vars;
        if (m_config.bodyIsJson)
            bodyVars.insert(QStringLiteral("keyword"), jsonEscape(keyword));
        const QByteArray body = renderTemplate(m_config.bodyTemplate, bodyVars).toUtf8();
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
        qCWarning(lcProvider) << m_config.id << "搜索失败：" << error;
        emit finished(m_config.id, {}, 0, error);
        return;
    }

    const QByteArray payload = reply->readAll();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcProvider) << m_config.id << "返回的不是 JSON：" << parseError.errorString()
                              << payload.left(120);
        emit finished(m_config.id, {}, 0, tr("响应不是 JSON 数据"));
        return;
    }

    const QJsonValue root = document.isArray() ? QJsonValue(document.array())
                                               : QJsonValue(document.object());
    const QJsonValue itemsValue = valueAtPath(root, m_config.itemsPath);
    if (!itemsValue.isArray()) {
        qCWarning(lcProvider) << m_config.id << "结果路径不是数组：" << m_config.itemsPath;
        emit finished(m_config.id, {}, 0,
                      tr("找不到结果路径“%1”").arg(m_config.itemsPath));
        return;
    }

    const QJsonArray items = itemsValue.toArray();
    m_result.clear();
    m_result.reserve(items.size());
    m_filtered = 0;
    m_pendingBases.clear();
    m_pendingIds.clear();
    m_pendingTitles.clear();
    m_pendingArtists.clear();

    for (const QJsonValue &node : items) {
        if (!node.isObject()) {
            ++m_filtered;
            continue;
        }
        if (!m_config.skipIf.isEmpty() && ruleMatches(node, m_config.skipIf)) {
            ++m_filtered;
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

        MediaItem base;
        base.type = m_config.mediaType;
        base.title = cleanText(vars.value(QStringLiteral("title")));
        base.artist = cleanText(vars.value(QStringLiteral("artist")));
        base.lyricUrl = QUrl(mapString(node, m_config.fields, QStringLiteral("lyricUrl"), vars));
        base.webUrl = QUrl(mapString(node, m_config.fields, QStringLiteral("webUrl"), vars));
        base.sourceId = m_config.id;
        base.headers = m_config.headers;
        if (!m_config.cookie.isEmpty())
            base.headers.insert(QStringLiteral("Cookie"), m_config.cookie);
        base.online = true;
        // 影片展示字段：没有对应配置就留空，列表会自动跳过
        base.posterUrl = normalizeUrl(mapString(node, m_config.fields,
                                                QStringLiteral("posterUrl"), vars));
        base.year = normalizeYear(mapString(node, m_config.fields, QStringLiteral("year"), vars));
        base.area = cleanText(mapString(node, m_config.fields, QStringLiteral("area"), vars));
        base.genre = cleanText(mapString(node, m_config.fields, QStringLiteral("genre"), vars));
        base.description = cleanText(
            mapString(node, m_config.fields, QStringLiteral("description"), vars));

        const QString durationSpec = m_config.fields.value(QStringLiteral("durationMs"));
        if (!durationSpec.isEmpty() && !durationSpec.startsWith(QLatin1Char('@'))) {
            const double raw = toNumber(valueAtPath(node, durationSpec));
            base.durationMs = static_cast<qint64>(raw);
        }

        if (!stream.trimmed().isEmpty()) {
            appendItem(base, stream);
            continue;
        }

        // 列表里没有直链：配置了二次解析就拿 id 去请求详情，否则只能过滤掉
        if (m_config.needsDetailFetch() && !vars.value(QStringLiteral("id")).isEmpty()) {
            m_pendingBases.append(base);
            m_pendingIds.append(vars.value(QStringLiteral("id")));
            m_pendingTitles.append(vars.value(QStringLiteral("title")));
            m_pendingArtists.append(vars.value(QStringLiteral("artist")));
        } else {
            ++m_filtered;
        }
    }

    m_detailIndex = 0;
    if (m_pendingIds.isEmpty())
        finishSearch();
    else
        startNextDetail();
}

void ConfigSearchProvider::appendItem(const MediaItem &base, const QString &stream)
{
    if (m_config.streamKind == QLatin1String("episodes")) {
        // 一个结果 = 一整部剧/片：按剧集拆成多条，标题带上集名区分
        const QVector<EpisodePart> parts = parseEpisodes(stream);
        if (parts.isEmpty()) {
            ++m_filtered;
            return;
        }
        for (const EpisodePart &part : parts) {
            MediaItem item = base;
            item.url = normalizeUrl(part.url);
            if (!part.name.isEmpty() && part.name != base.title)
                item.title = QStringLiteral("%1 · %2").arg(base.title, part.name);
            if (item.title.isEmpty())
                item.title = item.url.fileName();
            if (item.url.isEmpty() || !item.url.isValid()) {
                ++m_filtered;
                continue;
            }
            m_result.append(item);
        }
        return;
    }

    MediaItem item = base;
    item.url = normalizeUrl(stream);
    if (item.title.isEmpty())
        item.title = item.url.fileName();
    if (item.url.isEmpty() || !item.url.isValid()) {
        ++m_filtered;
        return;
    }
    m_result.append(item);
}

void ConfigSearchProvider::startNextDetail()
{
    if (m_detailIndex >= m_pendingIds.size()) {
        finishSearch();
        return;
    }

    const int index = m_detailIndex;
    if (m_http == nullptr) {
        ++m_filtered;
        ++m_detailIndex;
        startNextDetail();
        return;
    }

    QHash<QString, QString> vars;
    vars.insert(QStringLiteral("keyword"), m_keyword);
    vars.insert(QStringLiteral("id"), m_pendingIds.at(index));
    vars.insert(QStringLiteral("title"), m_pendingTitles.value(index));
    vars.insert(QStringLiteral("artist"), m_pendingArtists.value(index));

    const QUrl requestUrl(renderTemplate(m_config.detailUrlTemplate, vars));
    if (!requestUrl.isValid()) {
        qCWarning(lcProvider) << m_config.id << "详情地址无效：" << requestUrl.toString();
        ++m_filtered;
        ++m_detailIndex;
        startNextDetail();
        return;
    }

    QHash<QString, QString> headers = m_config.headers;
    if (!m_config.cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), m_config.cookie);

    m_detailReply = m_http->get(requestUrl, QByteArray(), headers);
    if (m_detailReply.isNull()) {
        ++m_filtered;
        ++m_detailIndex;
        startNextDetail();
        return;
    }
    connect(m_detailReply.data(), &QNetworkReply::finished, this,
            &ConfigSearchProvider::onDetailFinished);
}

void ConfigSearchProvider::onDetailFinished()
{
    QNetworkReply *reply = m_detailReply.data();
    m_detailReply = nullptr;

    const int index = m_detailIndex;
    ++m_detailIndex;

    if (reply == nullptr) {
        ++m_filtered;
        startNextDetail();
        return;
    }
    reply->deleteLater();

    if (!HttpClient::isHttpSuccess(reply)) {
        qCWarning(lcProvider) << m_config.id << "详情请求失败：" << HttpClient::describeError(reply);
        ++m_filtered;
        startNextDetail();
        return;
    }

    const QByteArray payload = reply->readAll();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcProvider) << m_config.id << "详情返回的不是 JSON：" << parseError.errorString();
        ++m_filtered;
        startNextDetail();
        return;
    }

    const QJsonValue root = document.isArray() ? QJsonValue(document.array())
                                               : QJsonValue(document.object());
    const QJsonValue node = valueAtPath(root, m_config.detailItemsPath);

    QHash<QString, QString> vars;
    vars.insert(QStringLiteral("keyword"), m_keyword);
    vars.insert(QStringLiteral("id"), m_pendingIds.value(index));
    vars.insert(QStringLiteral("title"), m_pendingTitles.value(index));
    vars.insert(QStringLiteral("artist"), m_pendingArtists.value(index));

    const QString stream =
        mapString(node, m_config.detailFields, QStringLiteral("streamUrl"), vars);
    if (stream.trimmed().isEmpty()) {
        ++m_filtered;
        startNextDetail();
        return;
    }

    appendItem(m_pendingBases.value(index), stream);
    startNextDetail();
}

void ConfigSearchProvider::finishSearch()
{
    qCInfo(lcProvider) << m_config.id << "解析完成" << m_result.size() << "条，过滤"
                       << m_filtered << "条";
    const QVector<MediaItem> result = m_result;
    const int filtered = m_filtered;
    m_result.clear();
    m_pendingBases.clear();
    m_pendingIds.clear();
    m_pendingTitles.clear();
    m_pendingArtists.clear();
    emit finished(m_config.id, result, filtered, QString());
}
