#include "lyrics/LyricsService.h"

#include "net/HttpClient.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>

Q_LOGGING_CATEGORY(lcLyrics, "mvp.lyrics")

namespace {

QString extractLyricText(const QByteArray &payload)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return QString();

    const QJsonObject root = doc.object();
    const QString main = root.value(QStringLiteral("lrc")).toObject().value(QStringLiteral("lyric")).toString();
    if (!main.trimmed().isEmpty())
        return main;

    const QString karaoke =
        root.value(QStringLiteral("klyric")).toObject().value(QStringLiteral("lyric")).toString();
    if (!karaoke.trimmed().isEmpty())
        return karaoke;

    return root.value(QStringLiteral("tlyric")).toObject().value(QStringLiteral("lyric")).toString();
}

} // namespace

LyricsService::LyricsService(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

void LyricsService::load(const QUrl &url)
{
    cancel();

    if (!url.isValid()) {
        emit lyricsReady(QVector<LrcLine>());
        return;
    }
    if (url.isLocalFile()) {
        loadLocalFile(url.toLocalFile());
        return;
    }
    if (m_http == nullptr)
        return;

    m_reply = m_http->get(url);
    connect(m_reply, &QNetworkReply::finished, this, &LyricsService::onFinished);
}

void LyricsService::loadLocalFile(const QString &filePath)
{
    cancel();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcLyrics) << "打开歌词文件失败：" << filePath << file.errorString();
        emit lyricsFailed(tr("无法读取歌词文件"));
        return;
    }
    const QVector<LrcLine> lines = LrcParser::parseBytes(file.readAll());
    qCDebug(lcLyrics) << "本地歌词行数" << lines.size();
    emit lyricsReady(lines);
}

void LyricsService::cancel()
{
    if (m_reply.isNull())
        return;
    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
}

void LyricsService::onFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    if (reply != m_reply)
        return;
    m_reply = nullptr;

    if (!HttpClient::isHttpSuccess(reply)) {
        const QString message = HttpClient::describeError(reply);
        qCWarning(lcLyrics) << "歌词请求失败：" << message;
        emit lyricsFailed(message);
        return;
    }

    const QByteArray payload = reply->readAll();
    if (payload.isEmpty()) {
        emit lyricsReady(QVector<LrcLine>());
        return;
    }

    QVector<LrcLine> lines;
    if (payload.trimmed().startsWith('{')) {
        // 网易云 /api/song/lyric 返回 JSON，真正歌词在 lrc.lyric 字段里
        const QString text = extractLyricText(payload);
        if (!text.isEmpty())
            lines = LrcParser::parse(text);
    } else {
        lines = LrcParser::parseBytes(payload);
    }

    qCDebug(lcLyrics) << "在线歌词行数" << lines.size();
    emit lyricsReady(lines);
}
