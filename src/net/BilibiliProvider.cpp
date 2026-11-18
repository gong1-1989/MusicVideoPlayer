#include "net/BilibiliProvider.h"

#include "net/HttpClient.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

Q_LOGGING_CATEGORY(lcBili, "mvp.net.bilibili")

namespace {

/// wbi 的固定重排表：把 img_key+sub_key 拼成的 64 位串按这张表重排，取前 32 位
const int kMixinTable[64] = {
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
    27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52};

/// 参数字符串里要剔除的字符（官方要求）
QString filterValue(const QString &value)
{
    QString filtered = value;
    const QString forbidden = QStringLiteral("!'()*");
    for (const QChar &ch : forbidden)
        filtered.remove(ch);
    return filtered;
}

QString defaultHeadersReferer(const QHash<QString, QString> &headers)
{
    return headers.value(QStringLiteral("Referer"),
                         QStringLiteral("https://www.bilibili.com/"));
}

} // namespace

BilibiliProvider::BilibiliProvider(const SourceConfig &config, HttpClient *http, QObject *parent)
    : SearchProviderBase(parent)
    , m_config(config)
    , m_http(http)
{
}

void BilibiliProvider::search(const QString &keyword, int limit)
{
    m_keyword = keyword;
    // 一个源最多解析 10 条：每条都要再打两次接口，条数多了首屏会等太久
    m_limit = qBound(1, limit, 10);
    m_done = false;
    m_items.clear();
    m_entries.clear();
    m_cursor = 0;
    m_filtered = 0;

    if (m_http == nullptr) {
        finish(tr("网络模块未就绪"));
        return;
    }
    // 密钥每天会换，超过 6 小时就重新取
    if (m_mixinKey.isEmpty() || m_keysAt.isValid() == false
        || m_keysAt.secsTo(QDateTime::currentDateTime()) > 6 * 3600) {
        fetchKeys();
    } else {
        doSearch();
    }
}

void BilibiliProvider::requestJson(const QUrl &url, const JsonCallback &callback)
{
    QHash<QString, QString> headers = m_config.headers;
    if (!headers.contains(QStringLiteral("Referer")))
        headers.insert(QStringLiteral("Referer"), defaultHeadersReferer(headers));

    QNetworkReply *reply = m_http->get(url, QByteArray(), headers);
    if (reply == nullptr) {
        callback(QJsonObject(), tr("请求未发出"));
        return;
    }

    QPointer<BilibiliProvider> self(this);
    connect(reply, &QNetworkReply::finished, this, [self, reply, callback] {
        reply->deleteLater();
        if (self.isNull())
            return;
        if (!HttpClient::isHttpSuccess(reply)) {
            callback(QJsonObject(), HttpClient::describeError(reply));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) {
            callback(QJsonObject(), self->tr("响应不是 JSON"));
            return;
        }
        callback(document.object(), QString());
    });
}

void BilibiliProvider::fetchKeys()
{
    requestJson(QUrl(QStringLiteral("https://api.bilibili.com/x/web-interface/nav")),
                [this](const QJsonObject &object, const QString &error) {
                    if (!error.isEmpty()) {
                        finish(tr("取签名密钥失败：%1").arg(error));
                        return;
                    }
                    const QJsonObject data = object.value(QStringLiteral("data")).toObject();
                    const QJsonObject wbi = data.value(QStringLiteral("wbi_img")).toObject();
                    const QString imgKey =
                        QUrl(wbi.value(QStringLiteral("img_url")).toString())
                            .fileName()
                            .section(QLatin1Char('.'), 0, 0);
                    const QString subKey =
                        QUrl(wbi.value(QStringLiteral("sub_url")).toString())
                            .fileName()
                            .section(QLatin1Char('.'), 0, 0);
                    if (imgKey.isEmpty() || subKey.isEmpty()) {
                        finish(tr("签名密钥缺失，接口可能已变更"));
                        return;
                    }
                    m_mixinKey = buildMixinKey(imgKey, subKey);
                    m_keysAt = QDateTime::currentDateTime();
                    qCInfo(lcBili) << "wbi keys refreshed";
                    doSearch();
                });
}

void BilibiliProvider::doSearch()
{
    QHash<QString, QString> params;
    params.insert(QStringLiteral("keyword"), m_keyword);
    params.insert(QStringLiteral("search_type"), QStringLiteral("video"));
    params.insert(QStringLiteral("page"), QStringLiteral("1"));
    params.insert(QStringLiteral("page_size"), QString::number(m_limit));

    const QString query = signedQuery(params);
    const QUrl url(QStringLiteral("https://api.bilibili.com/x/web-interface/wbi/search/type?")
                   + query);

    requestJson(url, [this](const QJsonObject &object, const QString &error) {
        if (!error.isEmpty()) {
            finish(tr("搜索失败：%1").arg(error));
            return;
        }

        const int code = object.value(QStringLiteral("code")).toInt(-1);
        if (code != 0) {
            const QString message = object.value(QStringLiteral("message")).toString();
            if (code == -412)
                finish(tr("B站风控拦截（-412）：请求过快或需要登录，稍后重试"));
            else if (code == -101)
                finish(tr("B站要求登录（-101）"));
            else
                finish(tr("B站返回错误 %1：%2").arg(code).arg(message));
            return;
        }

        const QJsonObject data = object.value(QStringLiteral("data")).toObject();
        const QJsonArray results = data.value(QStringLiteral("result")).toArray();

        for (const QJsonValue &value : results) {
            if (!value.isObject())
                continue;
            const QJsonObject item = value.toObject();
            const QString bvid = item.value(QStringLiteral("bvid")).toString();
            if (bvid.isEmpty()) {
                // 广告位、合集卡片没有 bvid，拿不到播放地址
                ++m_filtered;
                continue;
            }
            Entry entry;
            entry.bvid = bvid;
            entry.title = cleanTitle(item.value(QStringLiteral("title")).toString());
            entry.author = item.value(QStringLiteral("author")).toString();
            m_entries.append(entry);
        }

        if (m_entries.isEmpty()) {
            finish(QString());
            return;
        }
        resolveNext();
    });
}

void BilibiliProvider::resolveNext()
{
    if (m_done)
        return;
    if (m_cursor >= m_entries.size()) {
        finish(QString());
        return;
    }

    const Entry entry = m_entries.at(m_cursor);
    const QUrl viewUrl(QStringLiteral("https://api.bilibili.com/x/web-interface/view?bvid=")
                       + entry.bvid);

    requestJson(viewUrl, [this, entry](const QJsonObject &viewObject, const QString &viewError) {
        if (!viewError.isEmpty() || viewObject.value(QStringLiteral("code")).toInt(-1) != 0) {
            qCWarning(lcBili) << "view failed" << entry.bvid << viewError;
            ++m_filtered;
            ++m_cursor;
            resolveNext();
            return;
        }

        const QJsonObject data = viewObject.value(QStringLiteral("data")).toObject();
        const QString cid = data.value(QStringLiteral("cid")).toVariant().toString();
        if (cid.isEmpty()) {
            ++m_filtered;
            ++m_cursor;
            resolveNext();
            return;
        }

        // fnval=1 要单文件 mp4（dash 是音视频分轨，播放器吃不下）
        const QUrl playUrl(
            QStringLiteral("https://api.bilibili.com/x/player/playurl?bvid=%1&cid=%2"
                           "&fnval=1&qn=64&platform=html5&high_quality=1")
                .arg(entry.bvid, cid));

        requestJson(playUrl,
                    [this, entry, data](const QJsonObject &playObject, const QString &playError) {
                        ++m_cursor;
                        if (!playError.isEmpty()
                            || playObject.value(QStringLiteral("code")).toInt(-1) != 0) {
                            qCWarning(lcBili) << "playurl failed" << entry.bvid << playError;
                            ++m_filtered;
                            resolveNext();
                            return;
                        }
                        const QJsonObject playData =
                            playObject.value(QStringLiteral("data")).toObject();
                        const QJsonArray durl = playData.value(QStringLiteral("durl")).toArray();
                        if (durl.isEmpty()) {
                            ++m_filtered;
                            resolveNext();
                            return;
                        }
                        const QString stream =
                            durl.at(0).toObject().value(QStringLiteral("url")).toString();
                        if (stream.isEmpty()) {
                            ++m_filtered;
                            resolveNext();
                            return;
                        }

                        MediaItem item;
                        item.type = MediaType::Video;
                        item.title = data.value(QStringLiteral("title")).toString().isEmpty()
                                         ? entry.title
                                         : cleanTitle(data.value(QStringLiteral("title")).toString());
                        item.artist = data.value(QStringLiteral("owner"))
                                          .toObject()
                                          .value(QStringLiteral("name"))
                                          .toString();
                        if (item.artist.isEmpty())
                            item.artist = entry.author;
                        const double seconds =
                            data.value(QStringLiteral("duration")).toDouble(0.0);
                        item.durationMs = static_cast<qint64>(seconds * 1000);
                        item.url = QUrl(stream);
                        item.webUrl =
                            QUrl(QStringLiteral("https://www.bilibili.com/video/") + entry.bvid);
                        item.sourceId = m_config.id;
                        item.online = true;
                        item.headers = m_config.headers;
                        if (!item.headers.contains(QStringLiteral("Referer")))
                            item.headers.insert(QStringLiteral("Referer"),
                                                defaultHeadersReferer(m_config.headers));
                        m_items.append(item);
                        resolveNext();
                    });
    });
}

void BilibiliProvider::finish(const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    qCInfo(lcBili) << "bilibili" << m_items.size() << "items, filtered" << m_filtered
                   << (error.isEmpty() ? "ok" : error);
    emit finished(m_config.id, m_items, m_filtered, error);
}

QString BilibiliProvider::signedQuery(QHash<QString, QString> params) const
{
    params.insert(QStringLiteral("wts"), QString::number(QDateTime::currentSecsSinceEpoch()));

    QStringList keys = params.keys();
    keys.sort();

    QStringList pairs;
    for (const QString &key : keys) {
        pairs.append(key + QLatin1Char('=')
                     + QString::fromUtf8(QUrl::toPercentEncoding(filterValue(params.value(key)))));
    }
    const QString query = pairs.join(QLatin1Char('&'));
    const QString wrid =
        QString::fromLatin1(QCryptographicHash::hash((query + m_mixinKey).toUtf8(),
                                                     QCryptographicHash::Md5)
                                .toHex());
    return query + QStringLiteral("&w_rid=") + wrid;
}

QString BilibiliProvider::buildMixinKey(const QString &imgKey, const QString &subKey)
{
    const QString raw = imgKey + subKey;
    if (raw.size() < 64)
        return QString();

    QString mapped;
    for (int i = 0; i < 64; ++i)
        mapped.append(raw.at(kMixinTable[i]));

    return mapped.left(32);
}

QString BilibiliProvider::cleanTitle(const QString &raw)
{
    // 搜索结果标题里带 <em class="keyword"> 高亮标签
    QString title = raw;
    title.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    title.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    title.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    title.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    title.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    title.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return title.trimmed();
}
