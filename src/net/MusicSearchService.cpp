#include "net/MusicSearchService.h"

#include "net/HttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QUrl>

Q_LOGGING_CATEGORY(lcSearch, "mvp.net.search")

namespace {

const char kSearchEndpoint[] = "https://music.163.com/api/cloudsearch/pc";

} // namespace

MusicSearchService::MusicSearchService(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

void MusicSearchService::search(const QString &keyword)
{
    const QString trimmed = keyword.trimmed();
    if (trimmed.isEmpty() || m_http == nullptr)
        return;

    cancel();

    // 该接口通行的调用方式是 POST 表单，GET 容易返回 400/403
    const QByteArray body = QByteArrayLiteral("s=")
                          + trimmed.toUtf8().toPercentEncoding()
                          + QByteArrayLiteral("&type=1&offset=0&limit=")
                          + QByteArray::number(m_resultLimit);

    emit searchStarted();

    m_reply = m_http->postForm(QUrl(QString::fromLatin1(kSearchEndpoint)), body);
    connect(m_reply, &QNetworkReply::finished, this, &MusicSearchService::onFinished);
}

void MusicSearchService::setResultLimit(int limit)
{
    m_resultLimit = qBound(1, limit, 100);
}

void MusicSearchService::cancel()
{
    if (m_reply.isNull())
        return;
    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
}

void MusicSearchService::onFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    // 旧请求的回调可能晚于新请求到达，先比对再处理
    if (reply != m_reply)
        return;
    m_reply = nullptr;

    if (!HttpClient::isHttpSuccess(reply)) {
        const QString message = HttpClient::describeError(reply);
        qCWarning(lcSearch) << "search failed:" << message;
        emit searchFailed(message);
        emit searchFinished();
        return;
    }

    const QByteArray payload = reply->readAll();
    qCDebug(lcSearch) << "payload" << payload.size() << "bytes";

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcSearch) << "invalid json:" << error.errorString();
        emit searchFailed(tr("返回数据解析失败"));
        emit searchFinished();
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray songs =
        root.value(QStringLiteral("result")).toObject().value(QStringLiteral("songs")).toArray();

    QVector<MediaItem> items;
    items.reserve(songs.size());
    m_filteredCount = 0;
    for (const QJsonValue value : songs) {
        if (!value.isObject())
            continue;
        const QJsonObject song = value.toObject();
        const qint64 id = song.value(QStringLiteral("id")).toVariant().toLongLong();
        if (id <= 0)
            continue;

        // fee==1 是 VIP 专享：匿名访问 outer/url 会被 302 到 /404，既不能播也不能下载。
        // 在搜索阶段就剔掉，避免用户点了才看到 "播放失败"。
        // fee 为 0（免费）和 8（付费但可试听）实测都能正常拉到 128k 音源。
        if (song.value(QStringLiteral("fee")).toInt() == 1) {
            ++m_filteredCount;
            continue;
        }

        MediaItem item;
        item.type = MediaType::Music;
        item.title = song.value(QStringLiteral("name")).toString();
        const QJsonArray artists = song.value(QStringLiteral("ar")).toArray();
        if (!artists.isEmpty())
            item.artist = artists.at(0).toObject().value(QStringLiteral("name")).toString();
        item.url = QUrl(QStringLiteral("https://music.163.com/song/media/outer/url?id=%1.mp3").arg(id));
        item.lyricUrl =
            QUrl(QStringLiteral("https://music.163.com/api/song/lyric?lv=-1&kv=-1&tv=-1&id=%1").arg(id));
        item.online = true;
        item.durationMs = song.value(QStringLiteral("dt")).toVariant().toLongLong();
        items.append(item);
    }

    qCInfo(lcSearch) << "parsed" << items.size() << "items, filtered" << m_filteredCount;
    emit searchSucceeded(items);
    emit searchFinished();
}
