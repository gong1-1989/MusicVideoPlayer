#include "lyrics/LyricsService.h"

#include "cache/LyricsCache.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"

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

void LyricsService::load(const QUrl &url, const MediaItem &item)
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

    m_item = item;
    m_lyricUrl = url;

    // 在线歌词按“条目 + 歌词地址”缓存原始响应体。命中时不需要再打接口，
    // 但仍走同一套解析，JSON 包裹和纯 LRC 两种格式不会走岔。
    if (LyricsCache::isEligible(item)) {
        QByteArray cached;
        if (LyricsCache::load(item, &cached)) {
            qCDebug(lcLyrics) << "歌词缓存命中：" << item.displayName();
            deliverPayload(cached);
            return;
        }
    }

    if (m_http == nullptr) {
        // 网络模块不可用时也要给出终态，否则界面会一直停在“正在加载歌词”，
        // 而 failed/ready 两个信号都没有发出。
        qCWarning(lcLyrics) << "歌词加载失败：网络模块未就绪";
        emit lyricsFailed(tr("网络模块未就绪，无法加载在线歌词"));
        return;
    }

    const int generation = m_generation;
    m_reply = m_http->get(url);
    if (m_reply == nullptr) {
        emit lyricsFailed(tr("歌词请求未能发出"));
        return;
    }
    connect(m_reply, &QNetworkReply::finished, this, [this, generation] {
        onFinished(generation);
    });
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
    // 每次取消都让旧请求的世代号失效，即使 abort 之后的 finished 回调
    // 迟到，也不会把旧歌词塞进新条目的面板。
    ++m_generation;
    m_item = MediaItem();
    m_lyricUrl = QUrl();
    if (m_reply.isNull())
        return;
    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
}

void LyricsService::onFinished(int generation)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    if (generation != m_generation || reply != m_reply)
        return;
    m_reply = nullptr;

    if (!HttpClient::isHttpSuccess(reply)) {
        const QString message = HttpClient::describeError(reply);
        qCWarning(lcLyrics) << "歌词请求失败：" << message;
        emit lyricsFailed(message);
        return;
    }

    bool tooLarge = false;
    const QByteArray payload =
        HttpClient::readAllLimited(reply, NetLimits::kJsonBytes, &tooLarge);
    if (tooLarge) {
        qCWarning(lcLyrics) << "歌词响应超过" << NetLimits::kJsonBytes << "字节上限";
        emit lyricsFailed(tr("歌词响应超过 2 MB 上限"));
        return;
    }
    if (payload.isEmpty()) {
        emit lyricsReady(QVector<LrcLine>());
        return;
    }

    // 先落原始响应体再解析：歌词接口返回是否 JSON 包裹由上游决定，
    // 保存原文可以让下次命中和本次在线解析走完全相同的分支。
    if (LyricsCache::isEligible(m_item)
        && LyricsCache::store(m_item, payload)) {
        qCDebug(lcLyrics) << "歌词已缓存：" << m_item.displayName();
    }
    deliverPayload(payload);
}

void LyricsService::deliverPayload(const QByteArray &payload)
{
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
