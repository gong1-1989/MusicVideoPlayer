#include "net/ConfigSearchProvider.h"

#include "net/ConfigMapper.h"
#include "net/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>

#include "util/HeaderPolicy.h"

Q_LOGGING_CATEGORY(lcProvider, "mvp.net.provider")

using namespace ConfigMapper;

namespace {

QStringList collectNormalizedUrls(const QString &primary, const QStringList &alternatives,
                                  const QUrl &base)
{
    QStringList result;
    const QStringList raw = QStringList{primary} + alternatives;
    for (const QString &value : raw) {
        const QUrl url = normalizeUrl(value, base);
        if (url.isEmpty() || !url.isValid())
            continue;
        const QString text = url.toString();
        if (!result.contains(text))
            result.append(text);
    }
    return result;
}

/// 同一集的多条线路里优先 mp4/flv；没有直链时才用 HLS。
QUrl choosePlaybackUrl(const QStringList &candidates, QStringList *remaining)
{
    if (remaining != nullptr)
        remaining->clear();
    if (candidates.isEmpty())
        return QUrl();

    QString chosen = candidates.first();
    for (const QString &candidate : candidates) {
        if (isProgressiveVideoUrl(QUrl(candidate))) {
            chosen = candidate;
            break;
        }
    }
    if (remaining != nullptr) {
        for (const QString &candidate : candidates) {
            if (candidate != chosen)
                remaining->append(candidate);
        }
    }
    return QUrl(chosen);
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
    m_nextDetail = 0;
    m_activeDetails = 0;
    m_detailBuckets.clear();
    m_searchTimer.start();

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
    m_originUrl = requestUrl;

    QHash<QString, QString> headers = m_config.headers;
    if (!m_config.cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), m_config.cookie);
    headers = HeaderPolicy::sanitizedForTarget(requestUrl, headers, m_originUrl);

    qCInfo(lcProvider) << "搜索" << m_config.id << HttpClient::redactUrl(requestUrl);

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

    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    if (reply == nullptr) {
        emit finished(m_config.id, {}, 0, tr("响应丢失"));
        return;
    }
    m_baseUrl = reply->url();
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 elapsedMs = m_searchTimer.isValid() ? m_searchTimer.elapsed() : -1;

    if (!HttpClient::isHttpSuccess(reply)) {
        const QString error = HttpClient::describeError(reply);
        qCWarning(lcProvider) << m_config.id << "搜索失败：HTTP" << httpStatus
                              << "用时" << elapsedMs << "ms：" << error;
        emit finished(m_config.id, {}, 0, error);
        return;
    }

    const QByteArray payload = reply->readAll();
    // 成功路径也把状态码/字节数/耗时写进日志，排障时不必再猜响应大小
    qCInfo(lcProvider) << m_config.id << "搜索响应 HTTP" << httpStatus << payload.size()
                       << "字节，用时" << elapsedMs << "ms";
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcProvider) << m_config.id << "返回的不是 JSON：" << parseError.errorString()
                              << payload.size() << "字节，Content-Type："
                              << reply->header(QNetworkRequest::ContentTypeHeader).toString();
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
        base.lyricUrl = normalizeUrl(mapString(node, m_config.fields,
                                                QStringLiteral("lyricUrl"), vars), m_baseUrl);
        base.webUrl = normalizeUrl(mapString(node, m_config.fields,
                                              QStringLiteral("webUrl"), vars), m_baseUrl);
        base.sourceId = m_config.id;
        base.headers = m_config.headers;
        if (!m_config.cookie.isEmpty())
            base.headers.insert(QStringLiteral("Cookie"), m_config.cookie);
        base.online = true;
        // 影片展示字段：没有对应配置就留空，列表会自动跳过
        base.posterUrl = normalizeUrl(mapString(node, m_config.fields,
                                                QStringLiteral("posterUrl"), vars), m_baseUrl);
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
            m_result += buildItems(base, stream);
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

    if (m_pendingIds.isEmpty()) {
        finishSearch();
        return;
    }
    // 每条详情一个结果槽，完成顺序不同也不影响最终列表顺序
    m_detailBuckets.resize(m_pendingIds.size());
    launchDetails();
}

QVector<MediaItem> ConfigSearchProvider::buildItems(const MediaItem &base, const QString &stream)
{
    QVector<MediaItem> items;
    if (m_config.streamKind == QLatin1String("episodes")) {
        // 一个结果 = 一整部剧/片：按剧集拆成多条，标题带上集名区分
        const QVector<EpisodePart> parts = parseEpisodes(stream);
        if (parts.isEmpty()) {
            ++m_filtered;
            return items;
        }
        for (const EpisodePart &part : parts) {
            MediaItem item = base;
            const QStringList candidates = collectNormalizedUrls(part.url, part.altUrls, m_baseUrl);
            QStringList remaining;
            item.url = choosePlaybackUrl(candidates, &remaining);
            item.alternateUrls = remaining;
            if (!part.name.isEmpty() && part.name != base.title)
                item.title = QStringLiteral("%1 · %2").arg(base.title, part.name);
            if (item.title.isEmpty())
                item.title = item.url.fileName();
            if (item.url.isEmpty() || !item.url.isValid()) {
                ++m_filtered;
                continue;
            }
            items.append(item);
        }
        return items;
    }

    MediaItem item = base;
    // 单集接口也可能用 `$$$` 给多条线路；和剧集模式一样优先 mp4/flv。
    QStringList rawSources = stream.split(QStringLiteral("$$$"), Qt::SkipEmptyParts);
    if (rawSources.isEmpty())
        rawSources.append(stream);
    const QString primary = rawSources.takeFirst().trimmed();
    QStringList remaining;
    const QStringList candidates = collectNormalizedUrls(primary, rawSources, m_baseUrl);
    item.url = choosePlaybackUrl(candidates, &remaining);
    item.alternateUrls = remaining;
    if (item.title.isEmpty())
        item.title = item.url.fileName();
    if (item.url.isEmpty() || !item.url.isValid()) {
        ++m_filtered;
        return items;
    }
    items.append(item);
    return items;
}

void ConfigSearchProvider::launchDetails()
{
    constexpr int kDetailConcurrency = 3;

    while (m_activeDetails < kDetailConcurrency && m_nextDetail < m_pendingIds.size()) {
        const int index = m_nextDetail++;
        if (m_http == nullptr) {
            ++m_filtered;
            continue;
        }

        QHash<QString, QString> vars;
        vars.insert(QStringLiteral("keyword"), m_keyword);
        vars.insert(QStringLiteral("id"), m_pendingIds.at(index));
        vars.insert(QStringLiteral("title"), m_pendingTitles.value(index));
        vars.insert(QStringLiteral("artist"), m_pendingArtists.value(index));

        const QUrl requestUrl(renderTemplate(m_config.detailUrlTemplate, vars));
        if (!requestUrl.isValid()) {
            qCWarning(lcProvider) << m_config.id << "详情地址无效："
                                  << HttpClient::redactUrl(requestUrl);
            ++m_filtered;
            continue;
        }

        QHash<QString, QString> headers = m_config.headers;
        if (!m_config.cookie.isEmpty())
            headers.insert(QStringLiteral("Cookie"), m_config.cookie);
        // 详情接口可能被配置到另一个 host；只把凭据留给搜索源站。
        headers = HeaderPolicy::sanitizedForTarget(requestUrl, headers, m_originUrl);

        QNetworkReply *reply = m_http->get(requestUrl, QByteArray(), headers);
        if (reply == nullptr) {
            ++m_filtered;
            continue;
        }

        ++m_activeDetails;
        QElapsedTimer timer;
        timer.start();
        connect(reply, &QNetworkReply::finished, this, [this, reply, index, timer] {
            onDetailFinished(reply, index, timer.elapsed());
        });
    }

    if (m_nextDetail >= m_pendingIds.size() && m_activeDetails == 0)
        finishSearch();
}

void ConfigSearchProvider::onDetailFinished(QNetworkReply *reply, int index, qint64 elapsedMs)
{
    if (m_done) {
        if (reply != nullptr)
            reply->deleteLater();
        return;
    }
    if (m_activeDetails > 0)
        --m_activeDetails;

    if (reply == nullptr) {
        ++m_filtered;
        launchDetails();
        return;
    }
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!HttpClient::isHttpSuccess(reply)) {
        qCWarning(lcProvider) << m_config.id << "详情请求失败：HTTP" << httpStatus
                              << "用时" << elapsedMs << "ms："
                              << HttpClient::describeError(reply);
        ++m_filtered;
        launchDetails();
        return;
    }

    const QByteArray payload = reply->readAll();
    qCInfo(lcProvider) << m_config.id << "详情" << index << "HTTP" << httpStatus
                       << payload.size() << "字节，用时" << elapsedMs << "ms";
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcProvider) << m_config.id << "详情返回的不是 JSON：" << parseError.errorString();
        ++m_filtered;
        launchDetails();
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
        launchDetails();
        return;
    }

    if (index >= 0 && index < m_detailBuckets.size())
        m_detailBuckets[index] = buildItems(m_pendingBases.value(index), stream);
    else
        ++m_filtered;
    launchDetails();
}

void ConfigSearchProvider::finishSearch()
{
    if (m_done)
        return;
    m_done = true;

    // 详情结果按索引顺序并回主列表，保证并发完成后顺序与搜索命中一致
    for (const QVector<MediaItem> &bucket : m_detailBuckets)
        m_result += bucket;

    qCInfo(lcProvider) << m_config.id << "解析完成" << m_result.size() << "条，过滤"
                       << m_filtered << "条";
    const QVector<MediaItem> result = m_result;
    const int filtered = m_filtered;
    m_result.clear();
    m_pendingBases.clear();
    m_pendingIds.clear();
    m_pendingTitles.clear();
    m_pendingArtists.clear();
    m_detailBuckets.clear();
    m_nextDetail = 0;
    m_activeDetails = 0;
    emit finished(m_config.id, result, filtered, QString());
}
