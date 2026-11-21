#include "lyrics/LrcParser.h"
#include "core/PlaybackController.h"
#include "download/DownloadTask.h"
#include "media/MediaItem.h"
#include "net/ConfigMapper.h"
#include "net/HttpClient.h"
#include "net/SourceConfig.h"
#include "net/SearchCoordinator.h"
#include "stream/StreamProxy.h"
#include "stream/HlsPlaylist.h"
#include "stream/HlsSeekWindow.h"
#include "stream/StreamTokenRegistry.h"
#include "util/HeaderPolicy.h"
#include "util/PlaylistSniffer.h"
#include "util/SecretStore.h"
#include "util/TimeFormat.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

namespace {

QByteArray proxyTestBody(int size = 1000)
{
    QByteArray body;
    body.resize(size);
    for (int i = 0; i < body.size(); ++i)
        body[i] = char('a' + (i % 26));
    return body;
}

/// 模拟“忽略 Range、永远返回 200 全量”的上游 CDN。
/// 这是网易云等直链的常见行为，也是代理缓存要覆盖的核心场景。
class RangeIgnoringServer : public QTcpServer
{
public:
    explicit RangeIgnoringServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool())
                        return;

                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    const int headEnd = pending.indexOf("\r\n\r\n");
                    if (headEnd < 0) {
                        socket->setProperty("pending", pending);
                        return;
                    }

                    ++requestCount;
                    for (const QByteArray &line : pending.left(headEnd).split('\n')) {
                        if (line.toLower().startsWith("range:"))
                            lastRange = QString::fromUtf8(line.mid(6)).trimmed();
                    }

                    socket->setProperty("responded", true);
                    QByteArray response = "HTTP/1.1 200 OK\r\n"
                                          "Content-Type: " + contentType + "\r\n"
                                          "Accept-Ranges: none\r\n"
                                          "Content-Length: "
                                          + QByteArray::number(body.size())
                                          + "\r\nConnection: close\r\n\r\n" + body;
                    socket->write(response);
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    int requestCount = 0;
    QString lastRange;
    QByteArray contentType = QByteArrayLiteral("audio/mpeg");
};

/// 模拟真正支持 206 的 HLS 分片源站。代理对 `bytes=0-` 的 .ts 请求会先
/// 用 16KB 探测总长，再并发拉剩余分段；这里逐条记录 Range，供测试确认
/// 多连接路径确实被走到、而不是退化在上游单路透传。
class RangeSupportingServer : public QTcpServer
{
public:
    explicit RangeSupportingServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool())
                        return;

                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    const int headEnd = pending.indexOf("\r\n\r\n");
                    if (headEnd < 0) {
                        socket->setProperty("pending", pending);
                        return;
                    }

                    ++requestCount;
                    qint64 start = 0;
                    qint64 end = body.size() - 1;
                    for (const QByteArray &line : pending.left(headEnd).split('\n')) {
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const QByteArray spec = line.mid(6).trimmed();
                        if (!spec.startsWith("bytes="))
                            continue;
                        const QByteArray value = spec.mid(6);
                        const int dash = value.indexOf('-');
                        if (dash <= 0)
                            continue;
                        start = value.left(dash).toLongLong();
                        const QByteArray tail = value.mid(dash + 1);
                        if (!tail.isEmpty())
                            end = tail.toLongLong();
                    }
                    ranges.append(qMakePair(start, end));

                    socket->setProperty("responded", true);
                    if (start < 0 || start >= body.size()) {
                        const QByteArray response = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                                    "Content-Range: bytes */"
                                                    + QByteArray::number(body.size())
                                                    + "\r\nContent-Length: 0\r\n"
                                                      "Connection: close\r\n\r\n";
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    end = qMin(end, qint64(body.size() - 1));
                    const QByteArray part = body.mid(int(start), int(end - start + 1));
                    QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                          "Content-Type: video/mp2t\r\n"
                                          "Accept-Ranges: bytes\r\n"
                                          "Content-Range: bytes "
                                          + QByteArray::number(start) + '-'
                                          + QByteArray::number(end) + '/'
                                          + QByteArray::number(body.size())
                                          + "\r\nContent-Length: "
                                          + QByteArray::number(part.size())
                                          + "\r\nConnection: close\r\n\r\n" + part;
                    socket->write(response);
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    int requestCount = 0;
    QList<QPair<qint64, qint64>> ranges;
};

/// 缓存淘汰回归源：播放列表有 10 个分片，预取请求只回 206 头和 1KB 正文后挂住；
/// 普通媒体请求返回小文件，用来把缓存条目数推过 24 条上限。
class PrefetchEvictionServer : public QTcpServer
{
public:
    explicit PrefetchEvictionServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        playlist = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:5\n"
            "#EXTINF:5,\n"
            "slow0.ts\n"
            "#EXTINF:5,\n"
            "slow1.ts\n"
            "#EXTINF:5,\n"
            "slow2.ts\n"
            "#EXTINF:5,\n"
            "slow3.ts\n"
            "#EXTINF:5,\n"
            "slow4.ts\n"
            "#EXTINF:5,\n"
            "slow5.ts\n"
            "#EXTINF:5,\n"
            "slow6.ts\n"
            "#EXTINF:5,\n"
            "slow7.ts\n"
            "#EXTINF:5,\n"
            "slow8.ts\n"
            "#EXTINF:5,\n"
            "slow9.ts\n"
            "#EXT-X-ENDLIST\n");

        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool())
                        return;
                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    const int headEnd = pending.indexOf("\r\n\r\n");
                    if (headEnd < 0) {
                        socket->setProperty("pending", pending);
                        return;
                    }

                    const QByteArray requestLine =
                        pending.left(headEnd).split('\n').value(0).trimmed();
                    const QByteArray path = requestLine.split(' ').value(1);
                    socket->setProperty("responded", true);

                    if (path == "/hls/index.m3u8") {
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: application/vnd.apple.mpegurl\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(playlist.size())
                                              + "\r\nConnection: close\r\n\r\n" + playlist;
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    if (path.startsWith("/hls/slow")) {
                        const QString name = QString::fromUtf8(path.mid(5));
                        if (!slowRequests.contains(name))
                            slowRequests.append(name);
                        // 头声明 16KB，只发 1KB 后保持连接：rangeParts 里会有数据，
                        // 但该分片永远不会完成，模拟“预取进行中被缓存淘汰”。
                        QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                              "Content-Type: video/mp2t\r\n"
                                              "Content-Range: bytes 0-16383/200000\r\n"
                                              "Content-Length: 16384\r\n\r\n";
                        socket->write(response + QByteArray(1024, 's'));
                        return;
                    }

                    if (path.startsWith("/media/")) {
                        ++mediaRequests;
                        const QByteArray body(1000, 'm');
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: audio/mpeg\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(body.size())
                                              + "\r\nConnection: close\r\n\r\n" + body;
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                  "Connection: close\r\n\r\n");
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray playlist;
    QStringList slowRequests;
    int mediaRequests = 0;
};

/// 本地 HLS 下载源：主播放列表带 EXT-X-MAP 的 BYTERANGE，分片支持 206。
/// 用来验证 DownloadTask 的 init 段范围、分片长度校验和 .mp4→.ts 切换。
class HlsDownloadServer : public QTcpServer
{
public:
    explicit HlsDownloadServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        initBody = QByteArrayLiteral("INIT-SEGMENT-BYTES-0123456789");
        segment0 = proxyTestBody(100);
        segment1 = proxyTestBody(100);
        playlist = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:7\n"
            "#EXT-X-TARGETDURATION:1\n"
            "#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"10@0\"\n"
            "#EXTINF:1,\n"
            "seg0.ts\n"
            "#EXTINF:1,\n"
            "seg1.ts\n"
            "#EXT-X-ENDLIST\n");

        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool())
                        return;
                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    const int headEnd = pending.indexOf("\r\n\r\n");
                    if (headEnd < 0) {
                        socket->setProperty("pending", pending);
                        return;
                    }

                    const QByteArray head = pending.left(headEnd);
                    const QByteArray requestLine = head.split('\n').value(0).trimmed();
                    const QByteArray path = requestLine.split(' ').value(1);
                    qint64 start = 0;
                    qint64 end = -1;
                    bool hasRange = false;
                    for (const QByteArray &line : head.split('\n')) {
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const QByteArray value = line.mid(6).trimmed();
                        if (!value.startsWith("bytes="))
                            continue;
                        const int dash = value.indexOf('-');
                        if (dash <= 6)
                            continue;
                        start = value.mid(6, dash - 6).toLongLong();
                        const QByteArray tail = value.mid(dash + 1);
                        if (!tail.isEmpty())
                            end = tail.toLongLong();
                        hasRange = true;
                    }

                    QByteArray content;
                    QByteArray contentType;
                    if (path == "/index.m3u8") {
                        content = playlist;
                        contentType = "application/vnd.apple.mpegurl";
                    } else if (path == "/init.mp4") {
                        content = initBody;
                        contentType = "video/mp4";
                        initRangeStart = start;
                        initRangeEnd = end;
                        initRangeReceived = hasRange;
                    } else if (path == "/seg0.ts") {
                        content = segment0;
                        contentType = "video/mp2t";
                    } else if (path == "/seg1.ts") {
                        content = segment1;
                        contentType = "video/mp2t";
                    } else {
                        socket->setProperty("responded", true);
                        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    if (hasRange) {
                        end = (end < 0 || end >= content.size()) ? content.size() - 1 : end;
                        if (start < 0 || start > end) {
                            socket->setProperty("responded", true);
                            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\n"
                                          "Content-Range: bytes */"
                                          + QByteArray::number(content.size())
                                          + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                            return;
                        }
                        const QByteArray part = content.mid(int(start), int(end - start + 1));
                        QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                              "Content-Type: " + contentType + "\r\n"
                                              "Content-Range: bytes "
                                              + QByteArray::number(start) + '-'
                                              + QByteArray::number(end) + '/'
                                              + QByteArray::number(content.size())
                                              + "\r\nContent-Length: "
                                              + QByteArray::number(part.size())
                                              + "\r\nConnection: close\r\n\r\n" + part;
                        socket->setProperty("responded", true);
                        socket->write(response);
                    } else {
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: " + contentType + "\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(content.size())
                                              + "\r\nConnection: close\r\n\r\n" + content;
                        socket->setProperty("responded", true);
                        socket->write(response);
                    }
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray playlist;
    QByteArray initBody;
    QByteArray segment0;
    QByteArray segment1;
    bool initRangeReceived = false;
    qint64 initRangeStart = -1;
    qint64 initRangeEnd = -1;
};

bool readProxyResponse(QTcpSocket *socket, QByteArray *head, QByteArray *body,
                       int timeoutMs = 5000)
{
    QByteArray buffer;
    int headEnd = -1;
    qint64 expectedLength = 0;
    bool done = false;

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    auto consume = [&] {
        buffer += socket->readAll();
        if (headEnd < 0)
            headEnd = buffer.indexOf("\r\n\r\n");
        if (headEnd < 0)
            return;

        *head = buffer.left(headEnd + 4);
        for (const QByteArray &line : head->split('\n')) {
            if (line.toLower().startsWith("content-length:")) {
                expectedLength = line.mid(15).trimmed().toLongLong();
                break;
            }
        }
        *body = buffer.mid(headEnd + 4);
        if (body->size() >= expectedLength) {
            body->truncate(int(expectedLength));
            done = true;
            loop.quit();
        }
    };

    // 代理和 mock 上游都在本线程，必须跑事件循环才能让 QTcpServer/QNetworkAccessManager
    // 收到连接；单纯 waitForReadyRead 只会等当前客户端 socket。
    const QMetaObject::Connection readyConnection =
        QObject::connect(socket, &QTcpSocket::readyRead, socket, consume);
    const QMetaObject::Connection dropConnection =
        QObject::connect(socket, &QTcpSocket::disconnected, socket, [&] {
            consume();
            loop.quit();
        });

    timeout.start(timeoutMs);
    consume();
    if (!done)
        loop.exec();
    consume();
    QObject::disconnect(readyConnection);
    QObject::disconnect(dropConnection);
    return done;
}

} // namespace

class CoreLogicTest : public QObject
{
    Q_OBJECT

private slots:
    void timeFormat();
    void lrcParseWithOffsetAndMultipleTimestamps();
    void lrcIndexAt();
    void mediaItemJsonRoundTrip();
    void preferredPlaybackUrlSelectsProgressive();
    void mediaItemValidity();
    void mediaIdentityUsesStableWebUrl();
    void dedupeItemsKeepsFirstOccurrence();
    void playbackModeTextMatchesCompactLabels();
    void sourceConfigJsonRoundTrip();
    void sourceConfigSecretsNotPlaintext();
    void sourceConfigRejectsIncompleteConfig();
    void sourceConfigNormalizesTimeout();
    void sourceTimeoutUsesConfiguredValue();
    void httpLogUrlRedactsSignature();
    void headerPolicyStripsSecretsAcrossHost();
    void playlistSnifferRecognizesBody();
    void downloadHlsVerifiesSegmentsAndMapRange();
    void configMapperReadsNestedPaths();
    void configMapperBoolRulesDoNotMixWithNumbers();
    void configMapperSplitsEpisodesAndResolvesUrls();
    void streamProxySlicesRangeFromCache();
    void streamProxyHandlesSuffixlessTextPlaylist();
    void streamProxyEvictsPrefetchWithoutLeakingSlots();
    void seekPrefetchWindowFollowsPlaylistIndex();
    void streamProxyMultiRangeForHlsSegment();
    void streamProxyFallsBackWhenHlsRangeIgnored();
    void hlsPlaylistParsesAndIndexes();
    void hlsPlaylistRewritesUris();
    void streamTokensReuseByUrlAndHeaders();
};

void CoreLogicTest::timeFormat()
{
    QCOMPARE(TimeFormat::formatMs(-1), QStringLiteral("--:--"));
    QCOMPARE(TimeFormat::formatMs(0), QStringLiteral("00:00"));
    QCOMPARE(TimeFormat::formatMs(61'000), QStringLiteral("01:01"));
    QCOMPARE(TimeFormat::formatMs(3'600'000), QStringLiteral("1:00:00"));
}

void CoreLogicTest::lrcParseWithOffsetAndMultipleTimestamps()
{
    const QVector<LrcLine> lines = LrcParser::parse(
        QStringLiteral("[ti:测试]\n"
                       "[offset:100]\n"
                       "[00:01.5]第一句\n"
                       "[00:02.00][00:03.000]重复\n"
                       "[00:04]末句\n"));

    QCOMPARE(lines.size(), 4);
    QCOMPARE(lines.at(0).timeMs, qint64(1600));
    QCOMPARE(lines.at(0).text, QStringLiteral("第一句"));
    QCOMPARE(lines.at(1).timeMs, qint64(2100));
    QCOMPARE(lines.at(1).text, QStringLiteral("重复"));
    QCOMPARE(lines.at(2).timeMs, qint64(3100));
    QCOMPARE(lines.at(2).text, QStringLiteral("重复"));
    QCOMPARE(lines.at(3).timeMs, qint64(4100));
    QCOMPARE(lines.at(3).text, QStringLiteral("末句"));
}

void CoreLogicTest::lrcIndexAt()
{
    const QVector<LrcLine> lines{
        {1000, QStringLiteral("a")},
        {2000, QStringLiteral("b")},
        {3000, QStringLiteral("c")},
    };

    QCOMPARE(LrcParser::indexAt(lines, 0), -1);
    QCOMPARE(LrcParser::indexAt(lines, 1000), 0);
    QCOMPARE(LrcParser::indexAt(lines, 1999), 0);
    QCOMPARE(LrcParser::indexAt(lines, 2000), 1);
    QCOMPARE(LrcParser::indexAt(lines, 9999), 2);
}

void CoreLogicTest::mediaItemJsonRoundTrip()
{
    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("标题");
    item.artist = QStringLiteral("作者");
    item.url = QUrl(QStringLiteral("https://example.com/video.mp4"));
    item.alternateUrls = {QStringLiteral("https://example.com/video.m3u8"),
                          QStringLiteral("https://example.com/video.flv")};
    item.lyricUrl = QUrl(QStringLiteral("https://example.com/a.lrc"));
    item.webUrl = QUrl(QStringLiteral("https://example.com/page"));
    item.sourceId = QStringLiteral("src");
    item.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/"));
    item.headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret"));
    item.headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret"));
    item.online = true;
    item.durationMs = 12345;
    item.posterUrl = QUrl(QStringLiteral("https://example.com/p.jpg"));
    item.year = QStringLiteral("2026");
    item.area = QStringLiteral("中国大陆");
    item.genre = QStringLiteral("测试");
    item.description = QStringLiteral("简介");

    const QByteArray payload =
        QJsonDocument(mediaItemToJson(item)).toJson(QJsonDocument::Compact);
    QVERIFY(!payload.contains("sid=secret"));
    QVERIFY(!payload.contains("Bearer secret"));

    const MediaItem restored = mediaItemFromJson(mediaItemToJson(item));

    QVERIFY(restored.type == MediaType::Video);
    QCOMPARE(restored.title, item.title);
    QCOMPARE(restored.artist, item.artist);
    QCOMPARE(restored.url, item.url);
    QCOMPARE(restored.alternateUrls, item.alternateUrls);
    QCOMPARE(restored.lyricUrl, item.lyricUrl);
    QCOMPARE(restored.webUrl, item.webUrl);
    QCOMPARE(restored.sourceId, item.sourceId);
    QCOMPARE(restored.headers.value(QStringLiteral("Referer")),
             QStringLiteral("https://example.com/"));
    QVERIFY(!restored.headers.contains(QStringLiteral("Cookie")));
    QVERIFY(!restored.headers.contains(QStringLiteral("Authorization")));
    QCOMPARE(restored.online, item.online);
    QCOMPARE(restored.durationMs, item.durationMs);
    QCOMPARE(restored.posterUrl, item.posterUrl);
    QCOMPARE(restored.year, item.year);
    QCOMPARE(restored.area, item.area);
    QCOMPARE(restored.genre, item.genre);
    QCOMPARE(restored.description, item.description);
}

void CoreLogicTest::preferredPlaybackUrlSelectsProgressive()
{
    MediaItem hlsPrimary;
    hlsPrimary.type = MediaType::Video;
    hlsPrimary.url = QUrl(QStringLiteral("https://example.com/video.m3u8"));
    hlsPrimary.alternateUrls = {QStringLiteral("https://example.com/video.mp4"),
                                QStringLiteral("https://example.com/video.flv")};
    QCOMPARE(preferredPlaybackUrl(hlsPrimary),
             QUrl(QStringLiteral("https://example.com/video.mp4")));
    // 实际播放 mp4 时不能按 HLS 进入“定位中”，即使原始 URL 是 m3u8。
    QVERIFY(!playbackIsHls(hlsPrimary));

    MediaItem progressivePrimary;
    progressivePrimary.type = MediaType::Video;
    progressivePrimary.url = QUrl(QStringLiteral("https://example.com/video.flv"));
    progressivePrimary.alternateUrls = {QStringLiteral("https://example.com/video.m3u8")};
    QCOMPARE(preferredPlaybackUrl(progressivePrimary), progressivePrimary.url);
    QVERIFY(!playbackIsHls(progressivePrimary));

    MediaItem hlsOnly = hlsPrimary;
    hlsOnly.alternateUrls = {QStringLiteral("https://example.com/video.m3u8"),
                             QStringLiteral("https://cdn.example.com/not-a-video")};
    QCOMPARE(preferredPlaybackUrl(hlsOnly), hlsOnly.url);
    QVERIFY(playbackIsHls(hlsOnly));

    MediaItem music;
    music.type = MediaType::Music;
    music.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    music.alternateUrls = {QStringLiteral("https://example.com/song.mp4")};
    QCOMPARE(preferredPlaybackUrl(music), music.url);
    QVERIFY(!playbackIsHls(music));
}

void CoreLogicTest::mediaItemValidity()
{
    MediaItem empty;
    empty.title = QStringLiteral("无地址");
    QVERIFY(!empty.isValid());

    MediaItem local;
    local.url = QUrl::fromLocalFile(QStringLiteral("C:/music/a.mp3"));
    QVERIFY(local.isValid());

    MediaItem remote;
    remote.url = QUrl(QStringLiteral("https://example.com/a.mp3"));
    QVERIFY(remote.isValid());
}

void CoreLogicTest::mediaIdentityUsesStableWebUrl()
{
    MediaItem a;
    a.online = true;
    a.title = QStringLiteral("第一集");
    a.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    a.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=old"));

    MediaItem b = a;
    b.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=new"));

    QCOMPARE(mediaIdentity(a), mediaIdentity(b));
    QVERIFY(mediaIdentity(a).startsWith(QStringLiteral("web:")));

    b.title = QStringLiteral("第二集");
    QVERIFY(mediaIdentity(a) != mediaIdentity(b));

    MediaItem noWeb = a;
    noWeb.webUrl = QUrl();
    noWeb.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=old"));
    MediaItem noWeb2 = noWeb;
    noWeb2.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=new"));
    QCOMPARE(mediaIdentity(noWeb), mediaIdentity(noWeb2));
}

void CoreLogicTest::dedupeItemsKeepsFirstOccurrence()
{
    MediaItem first;
    first.online = true;
    first.title = QStringLiteral("第一集");
    first.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    first.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=old"));

    MediaItem duplicate = first;
    duplicate.url = QUrl(QStringLiteral("https://cdn.example.com/b.mp4?token=new"));
    duplicate.artist = QStringLiteral("另一个源");

    MediaItem nextEpisode = first;
    nextEpisode.title = QStringLiteral("第二集");

    MediaItem localA;
    localA.url = QUrl::fromLocalFile(QStringLiteral("/tmp/a.mp3"));
    MediaItem localDuplicate = localA;
    localDuplicate.title = QStringLiteral("同名文件");

    const QVector<MediaItem> unique = dedupeMediaItems(
        {first, duplicate, nextEpisode, localA, localDuplicate});
    QCOMPARE(unique.size(), 3);
    // 保留第一次出现的 URL，分页里展示的仍是原始结果顺序
    QCOMPARE(unique.at(0).url, first.url);
    QCOMPARE(unique.at(1).title, QStringLiteral("第二集"));
    QCOMPARE(unique.at(2).url, localA.url);
}

void CoreLogicTest::playbackModeTextMatchesCompactLabels()
{
    using Mode = PlaybackController::Mode;
    QCOMPARE(PlaybackController::modeDisplayText(Mode::ListLoop, false),
             QStringLiteral("列表循环"));
    QCOMPARE(PlaybackController::modeDisplayText(Mode::ListLoop, true),
             QStringLiteral("循环"));
    QCOMPARE(PlaybackController::modeDisplayText(Mode::SingleLoop, false),
             QStringLiteral("单曲循环"));
    QCOMPARE(PlaybackController::modeDisplayText(Mode::SingleLoop, true),
             QStringLiteral("单曲"));
    QCOMPARE(PlaybackController::modeDisplayText(Mode::Random, false),
             QStringLiteral("随机播放"));
    QCOMPARE(PlaybackController::modeDisplayText(Mode::Random, true),
             QStringLiteral("随机"));
}

void CoreLogicTest::sourceConfigJsonRoundTrip()
{
    SourceConfig config;
    config.id = QStringLiteral("demo");
    config.name = QStringLiteral("演示源");
    config.mediaType = MediaType::Video;
    config.kind = QStringLiteral("config");
    config.method = QStringLiteral("POST");
    config.urlTemplate = QStringLiteral("https://api.example.com/search?q={keyword}");
    config.bodyTemplate = QStringLiteral("{\"q\":\"{keyword}\"}");
    config.bodyIsJson = true;
    config.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/"));
    config.headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret-token"));
    config.cookie = QStringLiteral("sid=1");
    config.itemsPath = QStringLiteral("data.items");
    config.fields.insert(QStringLiteral("title"), QStringLiteral("name"));
    config.fields.insert(QStringLiteral("streamUrl"), QStringLiteral("@https://cdn/{id}.mp4"));
    config.streamKind = QStringLiteral("episodes");
    config.skipIf = QJsonObject{{QStringLiteral("name"), QStringLiteral("广告")}};
    config.detailUrlTemplate = QStringLiteral("https://api.example.com/detail/{id}");
    config.detailItemsPath = QStringLiteral("data");
    config.detailFields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));
    config.timeoutMs = 9000;

    QVERIFY2(config.isValid(), qPrintable(config.validationError()));

    const QJsonObject json = sourceConfigToJson(config);
    const QString serialized =
        QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact));
    QVERIFY(!serialized.contains(QStringLiteral("sid=1")));
    QVERIFY(!serialized.contains(QStringLiteral("secret-token")));
    QVERIFY(json.value(QStringLiteral("cookie")).toString().startsWith(
        QStringLiteral("mvp-dpapi:v1:")));
    QVERIFY(!json.value(QStringLiteral("headers")).toObject().contains(
        QStringLiteral("Authorization")));
    QVERIFY(json.value(QStringLiteral("headersProtected")).toObject().contains(
        QStringLiteral("Authorization")));
    bool ok = false;
    const SourceConfig restored = sourceConfigFromJson(json, &ok);

    QVERIFY(ok);
    QCOMPARE(restored.id, config.id);
    QCOMPARE(restored.name, config.name);
    QVERIFY(restored.mediaType == config.mediaType);
    QCOMPARE(restored.kind, config.kind);
    QCOMPARE(restored.method, config.method);
    QCOMPARE(restored.urlTemplate, config.urlTemplate);
    QCOMPARE(restored.bodyTemplate, config.bodyTemplate);
    QCOMPARE(restored.bodyIsJson, config.bodyIsJson);
    QVERIFY(restored.headers == config.headers);
    QCOMPARE(restored.cookie, config.cookie);
    QCOMPARE(restored.itemsPath, config.itemsPath);
    QVERIFY(restored.fields == config.fields);
    QCOMPARE(restored.streamKind, config.streamKind);
    QVERIFY(restored.skipIf == config.skipIf);
    QCOMPARE(restored.detailUrlTemplate, config.detailUrlTemplate);
    QCOMPARE(restored.detailItemsPath, config.detailItemsPath);
    QVERIFY(restored.detailFields == config.detailFields);
    QCOMPARE(restored.timeoutMs, config.timeoutMs);
}

void CoreLogicTest::sourceConfigSecretsNotPlaintext()
{
    if (!SecretStore::isAvailable())
        QSKIP("当前平台没有凭据加密能力，源凭据按设计不落盘");

    QJsonObject legacy;
    legacy.insert(QStringLiteral("id"), QStringLiteral("legacy"));
    legacy.insert(QStringLiteral("name"), QStringLiteral("旧源"));
    legacy.insert(QStringLiteral("url"), QStringLiteral("https://api.example.com/search"));
    legacy.insert(QStringLiteral("itemsPath"), QStringLiteral("data"));
    legacy.insert(QStringLiteral("fields"),
                  QJsonObject{{QStringLiteral("title"), QStringLiteral("name")},
                              {QStringLiteral("streamUrl"), QStringLiteral("url")}});
    legacy.insert(QStringLiteral("headers"),
                  QJsonObject{{QStringLiteral("Authorization"),
                               QStringLiteral("Bearer legacy-token")}});
    legacy.insert(QStringLiteral("cookie"), QStringLiteral("sid=legacy"));

    bool ok = false;
    const SourceConfig config = sourceConfigFromJson(legacy, &ok);
    QVERIFY(ok);
    QCOMPARE(config.cookie, QStringLiteral("sid=legacy"));
    QCOMPARE(config.headers.value(QStringLiteral("Authorization")),
             QStringLiteral("Bearer legacy-token"));

    const QJsonObject stored = sourceConfigToJson(config);
    const QString serialized =
        QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact));
    QVERIFY(!serialized.contains(QStringLiteral("sid=legacy")));
    QVERIFY(!serialized.contains(QStringLiteral("legacy-token")));
    QVERIFY(stored.value(QStringLiteral("cookie")).toString().startsWith(
        QStringLiteral("mvp-dpapi:v1:")));
    QVERIFY(stored.value(QStringLiteral("headersProtected"))
                .toObject()
                .contains(QStringLiteral("Authorization")));
    QVERIFY(!stored.value(QStringLiteral("headers"))
                 .toObject()
                 .contains(QStringLiteral("Authorization")));

    bool restoredOk = false;
    const SourceConfig restored = sourceConfigFromJson(stored, &restoredOk);
    QVERIFY(restoredOk);
    QCOMPARE(restored.cookie, QStringLiteral("sid=legacy"));
    QCOMPARE(restored.headers.value(QStringLiteral("Authorization")),
             QStringLiteral("Bearer legacy-token"));
}

void CoreLogicTest::sourceConfigRejectsIncompleteConfig()
{
    SourceConfig config;
    config.id = QStringLiteral("broken");
    config.name = QStringLiteral("坏源");
    config.urlTemplate = QStringLiteral("https://api.example.com/search");
    config.fields.insert(QStringLiteral("title"), QStringLiteral("name"));
    QVERIFY(!config.isValid());
    QVERIFY(!config.validationError().isEmpty());

    config.fields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));
    QVERIFY(config.isValid());

    SourceConfig external;
    external.id = QStringLiteral("ext");
    external.name = QStringLiteral("外部");
    external.kind = QStringLiteral("external");
    QVERIFY(!external.isValid());
    QVERIFY(external.validationError().contains(QStringLiteral("命令")));

    SourceConfig builtin = external;
    builtin.kind = QStringLiteral("builtin");
    builtin.provider = QStringLiteral("unknown");
    QVERIFY(!builtin.isValid());
}

void CoreLogicTest::sourceConfigNormalizesTimeout()
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), QStringLiteral("ext"));
    object.insert(QStringLiteral("name"), QStringLiteral("外部"));
    object.insert(QStringLiteral("kind"), QStringLiteral("external"));
    object.insert(QStringLiteral("command"), QStringLiteral("parser.exe"));
    object.insert(QStringLiteral("timeoutMs"), 100);

    bool ok = false;
    const SourceConfig config = sourceConfigFromJson(object, &ok);
    QVERIFY(ok);
    QCOMPARE(config.timeoutMs, 0);
    // 缺失 timeoutMs 也按自动预算，不能偷偷回落到 8 秒
    QJsonObject absent = object;
    absent.remove(QStringLiteral("timeoutMs"));
    const SourceConfig autoConfig = sourceConfigFromJson(absent, &ok);
    QVERIFY(ok);
    QCOMPARE(autoConfig.timeoutMs, 0);
}

void CoreLogicTest::sourceTimeoutUsesConfiguredValue()
{
    SourceConfig music;
    music.kind = QStringLiteral("config");
    music.timeoutMs = 12000;
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(music, 20), 12000);

    music.timeoutMs = 0;
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(music, 20), 20000);

    SourceConfig web;
    web.kind = QStringLiteral("builtin");
    web.provider = QStringLiteral("maccms_web");
    web.timeoutMs = 0;
    // 网页影视源没填超时仍按条数自动放宽
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(web, 20), 20000);
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(web, 50), 32000);

    // 一旦用户明确填写，网页源也按配置值，不再被自动预算覆盖
    web.timeoutMs = 15000;
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(web, 50), 15000);
}

void CoreLogicTest::httpLogUrlRedactsSignature()
{
    const QString text = HttpClient::redactUrl(
        QUrl(QStringLiteral("https://cdn.example.com/a.m3u8?id=42&sign=abc123&token=xyz&key=kk&vuutv=vv")));
    const QUrlQuery query{QUrl(text)};

    QCOMPARE(query.queryItemValue(QStringLiteral("id")), QStringLiteral("42"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sign")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("token")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("key")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("vuutv")), QStringLiteral("***"));
    QVERIFY(!text.contains(QStringLiteral("abc123")));
    QVERIFY(!text.contains(QStringLiteral("xyz")));

    const QString pathText = HttpClient::redactUrl(QUrl(QStringLiteral(
        "https://cdn.example.com/sign/0123456789abcdef0123456789abcdef/video.mp4")));
    QVERIFY(!pathText.contains(QStringLiteral("0123456789abcdef0123456789abcdef")));
    QVERIFY(pathText.contains(QStringLiteral("video.mp4")));
}

void CoreLogicTest::playlistSnifferRecognizesBody()
{
    QVERIFY(PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/a/index.m3u8")), QString()));
    QVERIFY(PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/signed/abc")),
        QStringLiteral("application/vnd.apple.mpegurl")));
    QVERIFY(!PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/signed/abc")),
        QStringLiteral("text/plain")));

    QVERIFY(PlaylistSniffer::bodyIsPlaylist(QByteArrayLiteral("#EXTM3U\n#EXTINF:1,\na.ts\n")));
    QVERIFY(PlaylistSniffer::bodyIsPlaylist(
        QByteArrayLiteral("\xEF\xBB\xBF  \r\n#EXTM3U\n#EXTINF:1,\na.ts\n")));
    QVERIFY(!PlaylistSniffer::bodyIsPlaylist(QByteArrayLiteral("<html>not a playlist</html>")));
    QVERIFY(!PlaylistSniffer::bodyIsPlaylist(
        QByteArrayLiteral("#EXTINF:1,\na.ts\n#EXTM3U\n")));
}

void CoreLogicTest::headerPolicyStripsSecretsAcrossHost()
{
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), QStringLiteral("https://source.example/"));
    headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret"));
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret"));
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("MVP"));

    const QHash<QString, QString> sameHost =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://source.example/a.m3u8")),
                                         headers);
    QVERIFY(sameHost.contains(QStringLiteral("Cookie")));
    QVERIFY(sameHost.contains(QStringLiteral("Authorization")));

    const QHash<QString, QString> crossHost =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://cdn.example/seg.ts")),
                                         headers);
    QVERIFY(!crossHost.contains(QStringLiteral("Cookie")));
    QVERIFY(!crossHost.contains(QStringLiteral("Authorization")));
    QCOMPARE(crossHost.value(QStringLiteral("Referer")),
             QStringLiteral("https://source.example/"));

    const QHash<QString, QString> downgrade =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("http://source.example/a")),
                                         headers);
    QVERIFY(!downgrade.contains(QStringLiteral("Cookie")));
}

void CoreLogicTest::downloadHlsVerifiesSegmentsAndMapRange()
{
    HlsDownloadServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("movie.mp4"));

    HttpClient http;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/index.m3u8")
                       .arg(server.serverPort()));
    DownloadTask *task = DownloadTask::start(&http, url, target);
    QVERIFY(task != nullptr);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);

    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 8000);
    QCOMPARE(failed.count(), 0);
    QVERIFY(succeeded.count() == 1);

    // 目标原本是 .mp4，识别为播放列表后必须实际落到 .ts。
    const QString output = dir.filePath(QStringLiteral("movie.ts"));
    QVERIFY2(QFile::exists(output), qPrintable(output));
    QFile file(output);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.size(), qint64(server.initBody.mid(0, 10).size()
                                 + server.segment0.size() + server.segment1.size()));
    QCOMPARE(file.readAll(), server.initBody.mid(0, 10) + server.segment0 + server.segment1);

    // EXT-X-MAP 的 BYTERANGE="10@0" 必须变成 bytes=0-9，而不是整段 init。
    QVERIFY(server.initRangeReceived);
    QCOMPARE(server.initRangeStart, qint64(0));
    QCOMPARE(server.initRangeEnd, qint64(9));
}

void CoreLogicTest::configMapperReadsNestedPaths()
{
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArrayLiteral("{\"data\":{\"items\":[{\"id\":11,\"title\":\"第一首\"}]}}"));
    QVERIFY(document.isObject());

    const QJsonValue items = ConfigMapper::valueAtPath(document.object(), QStringLiteral("data.items"));
    QVERIFY(items.isArray());
    QCOMPARE(items.toArray().size(), 1);

    const QJsonValue title =
        ConfigMapper::valueAtPath(items, QStringLiteral("[0].title"));
    QCOMPARE(title.toString(), QStringLiteral("第一首"));
    QVERIFY(ConfigMapper::valueAtPath(items, QStringLiteral("[1].id")).isNull());
}

void CoreLogicTest::configMapperBoolRulesDoNotMixWithNumbers()
{
    const QJsonObject item{{QStringLiteral("is_streamable"), true},
                           {QStringLiteral("play_count"), 12}};

    // true 不能等于 false，也不能和数字 0 混算
    QVERIFY(!ConfigMapper::ruleMatches(
        item, QJsonObject{{QStringLiteral("path"), QStringLiteral("is_streamable")},
                          {QStringLiteral("op"), QStringLiteral("eq")},
                          {QStringLiteral("value"), false}}));
    QVERIFY(ConfigMapper::ruleMatches(
        item, QJsonObject{{QStringLiteral("path"), QStringLiteral("is_streamable")},
                          {QStringLiteral("op"), QStringLiteral("eq")},
                          {QStringLiteral("value"), true}}));

    // 字段缺失时 eq false 不算命中，避免把“没有该字段”当成合法结果
    QVERIFY(!ConfigMapper::ruleMatches(
        QJsonObject{}, QJsonObject{{QStringLiteral("path"), QStringLiteral("is_streamable")},
                                   {QStringLiteral("op"), QStringLiteral("eq")},
                                   {QStringLiteral("value"), false}}));

    QVERIFY(ConfigMapper::ruleMatches(
        item, QJsonObject{{QStringLiteral("path"), QStringLiteral("play_count")},
                          {QStringLiteral("op"), QStringLiteral("gt")},
                          {QStringLiteral("value"), 10}}));
}

void CoreLogicTest::configMapperSplitsEpisodesAndResolvesUrls()
{
    const QVector<ConfigMapper::EpisodePart> parts = ConfigMapper::parseEpisodes(
        QStringLiteral("第01集$https://x/1.m3u8#第02集$https://x/2.m3u8"
                       "$$$第01集$https://y/1.mp4#第02集$https://y/2.mp4"));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts.at(0).name, QStringLiteral("第01集"));
    QCOMPARE(parts.at(0).url, QStringLiteral("https://x/1.m3u8"));
    QCOMPARE(parts.at(0).altUrls.size(), 1);
    QCOMPARE(parts.at(0).altUrls.first(), QStringLiteral("https://y/1.mp4"));
    QCOMPARE(parts.at(1).name, QStringLiteral("第02集"));
    QCOMPARE(parts.at(1).altUrls.first(), QStringLiteral("https://y/2.mp4"));

    const QUrl protocolRelative =
        ConfigMapper::normalizeUrl(QStringLiteral("//cdn.example.com/a.ts"));
    QCOMPARE(protocolRelative.scheme(), QStringLiteral("https"));
    QCOMPARE(protocolRelative.host(), QStringLiteral("cdn.example.com"));

    const QUrl relative = ConfigMapper::normalizeUrl(
        QStringLiteral("000001.ts"), QUrl(QStringLiteral("https://x/a/index.m3u8")));
    QCOMPARE(relative.toString(), QStringLiteral("https://x/a/000001.ts"));

    QCOMPARE(ConfigMapper::jsonEscape(QStringLiteral("a\"b\\c")),
             QStringLiteral("a\\\"b\\\\c"));
}

void CoreLogicTest::streamProxySlicesRangeFromCache()
{
    RangeIgnoringServer upstream;
    upstream.body = proxyTestBody();
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/audio/test.mp3").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    auto fetch = [&local](const QByteArray &range, QByteArray *head, QByteArray *body) {
        QTcpSocket socket;
        socket.connectToHost(local.host(), quint16(local.port()));
        if (!socket.waitForConnected(3000))
            return false;

        QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n";
        if (!range.isEmpty())
            request += "Range: " + range + "\r\n";
        request += "Connection: close\r\n\r\n";
        socket.write(request);
        socket.flush();
        return readProxyResponse(&socket, head, body);
    };

    QByteArray head;
    QByteArray body;

    // 上游收到 Range 仍回 200 全量，代理必须自己切出第一段 206。
    QVERIFY(fetch("bytes=0-99", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QVERIFY(head.contains("Content-Range: bytes 0-99/1000"));
    QCOMPARE(body, upstream.body.mid(0, 100));
    QTRY_COMPARE_WITH_TIMEOUT(upstream.requestCount, 1, 3000);

    // 第二段必须命中内存缓存，不能再打上游。
    QVERIFY(fetch("bytes=200-299", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QVERIFY(head.contains("Content-Range: bytes 200-299/1000"));
    QCOMPARE(body, upstream.body.mid(200, 100));
    QCOMPARE(upstream.requestCount, 1);

    // bytes=-N：播放器 seek 到尾部时常见。
    QVERIFY(fetch("bytes=-50", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QVERIFY(head.contains("Content-Range: bytes 950-999/1000"));
    QCOMPARE(body, upstream.body.mid(950, 50));
    QCOMPARE(upstream.requestCount, 1);

    // 起点越界必须 416，并且带完整长度，避免播放器无限重试或退化成整段重下。
    QVERIFY(fetch("bytes=1000-", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 416"), head.constData());
    QVERIFY(head.contains("Content-Range: bytes */1000"));
    QCOMPARE(body.size(), 0);
    QCOMPARE(upstream.requestCount, 1);
}

void CoreLogicTest::streamProxyHandlesSuffixlessTextPlaylist()
{
    // 签名 HLS 常见形态：地址没有 .m3u8，Content-Type 只是 text/plain。
    // 代理必须在 bytes=0- 时仍拉全文并改写分片，不能把原始 m3u8 当媒体缓存。
    RangeIgnoringServer upstream;
    upstream.contentType = QByteArrayLiteral("text/plain; charset=utf-8");
    upstream.body = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1,\n"
        "seg0.ts\n"
        "#EXTINF:1,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/signed/playlist?token=abc")
            .arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));

    QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Range: bytes=0-\r\n"
                         "Connection: close\r\n\r\n";
    socket.write(request);
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 200"), head.constData());
    QVERIFY(head.contains("Content-Type: text/plain"));
    QVERIFY(body.contains("#EXTM3U"));
    QVERIFY(body.contains("/stream/"));
    QCOMPARE(upstream.requestCount, 1);
}

void CoreLogicTest::streamProxyEvictsPrefetchWithoutLeakingSlots()
{
    PrefetchEvictionServer upstream;
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString playlistUrl =
        QStringLiteral("http://127.0.0.1:%1/hls/index.m3u8").arg(upstream.serverPort());
    const QUrl localPlaylist = proxy.proxyUrlFor(QUrl(playlistUrl));
    QVERIFY(localPlaylist.isValid());

    auto fetch = [](const QUrl &local, QByteArray *head, QByteArray *body) {
        QTcpSocket socket;
        socket.connectToHost(local.host(), quint16(local.port()));
        if (!socket.waitForConnected(3000))
            return false;
        QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Connection: close\r\n\r\n";
        socket.write(request);
        socket.flush();
        return readProxyResponse(&socket, head, body);
    };

    QByteArray head;
    QByteArray body;
    QVERIFY(fetch(localPlaylist, &head, &body));
    QVERIFY(body.contains("/stream/"));

    // 深拖动预取会创建 4 条多连接 Range 条目；源站只回 1KB 后挂住，
    // 这些条目会长期占用缓存，但字节必须计入 cacheStats()。
    proxy.prefetchForSeek(QUrl(playlistUrl), {}, 0);
    QTRY_VERIFY_WITH_TIMEOUT(proxy.cacheStats().rangePrefetches >= 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(proxy.cacheStats().bytes >= 1024, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(upstream.slowRequests.size() >= 1, 3000);

    // 再用 30 个普通媒体请求把缓存推过 24 条上限，淘汰最老的预取条目。
    // 若预取槽位没有归还，队列里的 slow4.ts 永远不会被发起。
    for (int i = 0; i < 30; ++i) {
        const QString mediaUrl =
            QStringLiteral("http://127.0.0.1:%1/media/%2.mp3")
                .arg(upstream.serverPort())
                .arg(i);
        const QUrl local = proxy.proxyUrlFor(QUrl(mediaUrl));
        QVERIFY(local.isValid());
        QVERIFY(fetch(local, &head, &body));
    }

    QTRY_VERIFY_WITH_TIMEOUT(upstream.slowRequests.contains(QStringLiteral("slow4.ts")),
                             5000);
    const StreamProxy::CacheStats stats = proxy.cacheStats();
    QVERIFY2(stats.entries <= 24, qPrintable(QString::number(stats.entries)));
    QVERIFY2(stats.activePrefetches <= 4, qPrintable(QString::number(stats.activePrefetches)));
}

void CoreLogicTest::seekPrefetchWindowFollowsPlaylistIndex()
{
    // 索引 152 是播放列表里的目标片；FFmpeg 会顺序打开 152..161。
    // 这个用例锁住“从目标索引开始、向后 9 片”的窗口，防止再按文件名
    // 做 target+2 / target-9 导致整段预取错位。
    const auto normal = HlsSeekWindow::forTarget(152, 600, 9);
    QVERIFY(normal.isValid());
    QCOMPARE(normal.first, 152);
    QCOMPARE(normal.last, 161);

    const auto atStart = HlsSeekWindow::forTarget(0, 600, 9);
    QVERIFY(atStart.isValid());
    QCOMPARE(atStart.first, 0);
    QCOMPARE(atStart.last, 9);

    // 靠近末尾时只能裁到最后一个分片，不能越界。
    const auto atEnd = HlsSeekWindow::forTarget(598, 600, 9);
    QVERIFY(atEnd.isValid());
    QCOMPARE(atEnd.first, 598);
    QCOMPARE(atEnd.last, 599);

    QVERIFY(!HlsSeekWindow::forTarget(-1, 600, 9).isValid());
    QVERIFY(!HlsSeekWindow::forTarget(600, 600, 9).isValid());
    QVERIFY(!HlsSeekWindow::forTarget(152, 0, 9).isValid());
}

void CoreLogicTest::streamProxyMultiRangeForHlsSegment()
{
    RangeSupportingServer upstream;
    upstream.body = proxyTestBody(200'000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/hls/0000001.ts").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));

    const QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\n"
                               "Range: bytes=0-\r\n"
                               "Connection: close\r\n\r\n";
    socket.write(request);
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body, 8000));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QVERIFY(head.contains("Content-Range: bytes 0-199999/200000"));
    QCOMPARE(body, upstream.body);

    // 16KB 探测 + 剩余按 4 路拆分，必须是多连接 Range，而不是单路全量透传。
    QTRY_VERIFY_WITH_TIMEOUT(upstream.requestCount >= 4, 5000);
    QVERIFY(upstream.ranges.contains(qMakePair(qint64(0), qint64(16383))));
    QVERIFY(upstream.ranges.contains(qMakePair(qint64(16384), qint64(66383))));
}

void CoreLogicTest::streamProxyFallsBackWhenHlsRangeIgnored()
{
    RangeIgnoringServer upstream;
    upstream.body = proxyTestBody();
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/hls/0000002.ts").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));

    const QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\n"
                               "Range: bytes=0-\r\n"
                               "Connection: close\r\n\r\n";
    socket.write(request);
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body, 8000));
    QCOMPARE(body, upstream.body);
    // 首片探测收到 200 后必须回退单路，不能把客户端挂在 multiRange 上等超时。
    QTRY_COMPARE_WITH_TIMEOUT(upstream.requestCount, 2, 5000);
    QCOMPARE(upstream.lastRange, QStringLiteral("bytes=0-"));
}

void CoreLogicTest::hlsPlaylistParsesAndIndexes()
{
    const QByteArray body =
        "#EXTM3U\n"
        "#EXT-X-BASE:https://cdn.test/a/\n"
        "#EXTINF:4,\n"
        "seg1.ts\n"
        "#EXTINF:2.5,\n"
        "../b/seg2.ts?x=1\n";
    const QSharedPointer<HlsPlaylist::Info> info =
        HlsPlaylist::parse(body, QUrl(QStringLiteral("https://origin.test/path/list.m3u8")));
    QVERIFY(!info.isNull());
    QCOMPARE(info->segments.size(), 2);
    QCOMPARE(info->segments.at(0),
             QUrl(QStringLiteral("https://cdn.test/a/seg1.ts")));
    QCOMPARE(info->segments.at(1),
             QUrl(QStringLiteral("https://cdn.test/b/seg2.ts?x=1")));
    QCOMPARE(info->starts.at(0), qint64(0));
    QCOMPARE(info->starts.at(1), qint64(4000));
    QCOMPARE(info->totalMs, qint64(6500));
    QVERIFY(info->prefetchable);

    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, -1), 0);
    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, 3999), 0);
    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, 4000), 1);
    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, 9999), 1);

    QVERIFY(HlsPlaylist::sameScope(
        QUrl(QStringLiteral("https://cdn.test/a/list.m3u8")),
        QUrl(QStringLiteral("https://cdn.test/a/720/index.m3u8"))));
    QVERIFY(!HlsPlaylist::sameScope(
        QUrl(QStringLiteral("https://cdn.test/a/list.m3u8")),
        QUrl(QStringLiteral("https://other.test/a/list.m3u8"))));

    const QByteArray byteRange =
        "#EXTM3U\n"
        "#EXTINF:4,\n"
        "#EXT-X-BYTERANGE:100@0\n"
        "seg.ts\n";
    const QSharedPointer<HlsPlaylist::Info> ranged =
        HlsPlaylist::parse(byteRange, QUrl(QStringLiteral("https://cdn.test/a/list.m3u8")));
    QVERIFY(!ranged.isNull());
    QVERIFY(!ranged->prefetchable);
}

void CoreLogicTest::hlsPlaylistRewritesUris()
{
    const QUrl playlistUrl(QStringLiteral("https://cdn.test/a/list.m3u8"));
    const QByteArray body =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:4,\n"
        "seg1.ts\n";

    const QByteArray rewritten = HlsPlaylist::rewrite(
        body, playlistUrl, [](const QUrl &absolute) {
            Q_UNUSED(absolute)
            return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
        });

    const QString text = QString::fromUtf8(rewritten);
    QVERIFY(text.contains(QStringLiteral("URI=\"http://127.0.0.1:1234/stream/token/key.bin\"")));
    QVERIFY(text.contains(QStringLiteral("http://127.0.0.1:1234/stream/token/seg1.ts")));
    QVERIFY(!text.contains(QStringLiteral("seg1.ts\nseg1.ts")));
}

void CoreLogicTest::streamTokensReuseByUrlAndHeaders()
{
    StreamTokenRegistry tokens;
    const QUrl upstream(QStringLiteral("https://cdn.test/a/seg1.ts"));
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), QStringLiteral("https://source.test/"));
    headers.insert(QStringLiteral("X-Source-Session"), QStringLiteral("sid=1"));

    const QUrl first = tokens.registerFor(upstream, headers, 4567);
    const QUrl second = tokens.registerFor(upstream, headers, 4567);
    QVERIFY(first.isValid());
    QCOMPARE(first, second);
    QCOMPARE(tokens.size(), 1);

    QHash<QString, QString> other = headers;
    other.insert(QStringLiteral("X-Source-Session"), QStringLiteral("sid=2"));
    const QUrl changed = tokens.registerFor(upstream, other, 4567);
    QVERIFY(changed != first);
    QCOMPARE(tokens.size(), 2);

    const QString path = first.path();
    const QString prefix = QStringLiteral("/stream/");
    QVERIFY(path.startsWith(prefix));
    const QString token = path.mid(prefix.size()).section(QLatin1Char('/'), 0, 0);
    QVERIFY(!token.isEmpty());

    StreamTokenRegistry::Upstream restored;
    QVERIFY(tokens.lookup(token, &restored));
    QCOMPARE(restored.url, upstream);
    QCOMPARE(restored.headers.value(QStringLiteral("X-Source-Session")),
             QStringLiteral("sid=1"));
    QVERIFY(!tokens.lookup(QStringLiteral("unknown"), &restored));

    tokens.clear();
    QCOMPARE(tokens.size(), 0);
}

QTEST_GUILESS_MAIN(CoreLogicTest)

#include "test_core_logic.moc"
