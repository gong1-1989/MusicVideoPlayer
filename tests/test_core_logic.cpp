#include "lyrics/LrcParser.h"
#include "media/MediaItem.h"
#include "net/SourceConfig.h"
#include "util/TimeFormat.h"

#include <QJsonObject>
#include <QTest>

class CoreLogicTest : public QObject
{
    Q_OBJECT

private slots:
    void timeFormat();
    void lrcParseWithOffsetAndMultipleTimestamps();
    void lrcIndexAt();
    void mediaItemJsonRoundTrip();
    void mediaItemValidity();
    void mediaIdentityUsesStableWebUrl();
    void sourceConfigJsonRoundTrip();
    void sourceConfigRejectsIncompleteConfig();
    void sourceConfigNormalizesTimeout();
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

QTEST_GUILESS_MAIN(CoreLogicTest)

#include "test_core_logic.moc"
