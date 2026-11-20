#include "lyrics/LrcParser.h"
#include "media/MediaItem.h"
#include "net/ConfigMapper.h"
#include "net/HttpClient.h"
#include "net/SourceConfig.h"
#include "stream/StreamProxy.h"
#include "util/TimeFormat.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

namespace {

QByteArray proxyTestBody()
{
    QByteArray body;
    body.resize(1000);
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
                                          "Content-Type: audio/mpeg\r\n"
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
    void sourceConfigJsonRoundTrip();
    void sourceConfigRejectsIncompleteConfig();
    void sourceConfigNormalizesTimeout();
    void httpLogUrlRedactsSignature();
    void configMapperReadsNestedPaths();
    void configMapperBoolRulesDoNotMixWithNumbers();
    void configMapperSplitsEpisodesAndResolvesUrls();
    void streamProxySlicesRangeFromCache();
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
    item.online = true;
    item.durationMs = 12345;
    item.posterUrl = QUrl(QStringLiteral("https://example.com/p.jpg"));
    item.year = QStringLiteral("2026");
    item.area = QStringLiteral("中国大陆");
    item.genre = QStringLiteral("测试");
    item.description = QStringLiteral("简介");

    const MediaItem restored = mediaItemFromJson(mediaItemToJson(item));

    QVERIFY(restored.type == MediaType::Video);
    QCOMPARE(restored.title, item.title);
    QCOMPARE(restored.artist, item.artist);
    QCOMPARE(restored.url, item.url);
    QCOMPARE(restored.alternateUrls, item.alternateUrls);
    QCOMPARE(restored.lyricUrl, item.lyricUrl);
    QCOMPARE(restored.webUrl, item.webUrl);
    QCOMPARE(restored.sourceId, item.sourceId);
    QVERIFY(restored.headers == item.headers);
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

    MediaItem progressivePrimary;
    progressivePrimary.type = MediaType::Video;
    progressivePrimary.url = QUrl(QStringLiteral("https://example.com/video.flv"));
    progressivePrimary.alternateUrls = {QStringLiteral("https://example.com/video.m3u8")};
    QCOMPARE(preferredPlaybackUrl(progressivePrimary), progressivePrimary.url);

    MediaItem hlsOnly = hlsPrimary;
    hlsOnly.alternateUrls = {QStringLiteral("https://example.com/video.m3u8"),
                             QStringLiteral("https://cdn.example.com/not-a-video")};
    QCOMPARE(preferredPlaybackUrl(hlsOnly), hlsOnly.url);

    MediaItem music;
    music.type = MediaType::Music;
    music.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    music.alternateUrls = {QStringLiteral("https://example.com/song.mp4")};
    QCOMPARE(preferredPlaybackUrl(music), music.url);
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
    QCOMPARE(config.timeoutMs, 8000);
}

void CoreLogicTest::httpLogUrlRedactsSignature()
{
    const QString text = HttpClient::redactUrl(
        QUrl(QStringLiteral("https://cdn.example.com/a.m3u8?id=42&sign=abc123&token=xyz&key=kk")));
    const QUrlQuery query{QUrl(text)};

    QCOMPARE(query.queryItemValue(QStringLiteral("id")), QStringLiteral("42"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sign")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("token")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("key")), QStringLiteral("***"));
    QVERIFY(!text.contains(QStringLiteral("abc123")));
    QVERIFY(!text.contains(QStringLiteral("xyz")));
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

QTEST_GUILESS_MAIN(CoreLogicTest)

#include "test_core_logic.moc"
