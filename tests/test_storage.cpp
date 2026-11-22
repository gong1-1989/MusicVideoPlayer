#include "core/PlaylistModel.h"
#include "core/PlaylistStore.h"
#include "download/DownloadJob.h"
#include "download/DownloadManager.h"
#include "download/DownloadPaths.h"
#include "net/HttpClient.h"
#include "net/SourceRegistry.h"
#include "storage/StoragePaths.h"
#include "util/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

MediaItem makeMusic(const QString &title)
{
    MediaItem item;
    item.type = MediaType::Music;
    item.title = title;
    item.url = QUrl(QStringLiteral("https://example.com/%1.mp3").arg(title));
    return item;
}

MediaItem makeVideo(const QString &title)
{
    MediaItem item;
    item.type = MediaType::Video;
    item.title = title;
    item.url = QUrl(QStringLiteral("https://example.com/%1.m3u8").arg(title));
    item.webUrl = QUrl(QStringLiteral("https://example.com/detail/%1").arg(title));
    item.online = true;
    return item;
}

} // namespace

class StorageTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void playlistRoundTrip();
    void playlistSaveWritesPrimaryOnly();
    void playlistCorruptFileBacksUp();
    void playlistVideoProgressRoundTrip();
    void playlistVideoProgressRemovedWithItem();
    void sourceRegistrySeedsAndPersists();
    void sourceRegistryReloadUsesPrimary();
    void sourceRegistryCorruptFileBacksUp();
    void sourceRegistryUnwritableDataDirFails();
    void downloadJobRoundTripEncryptsSensitiveHeaders();
    void downloadPathsAvoidOverwriteAndPickSuffix();
    void downloadPathsTreatPartialAsOccupied();
    void downloadManagerReservesActiveTarget();
    void sourceRegistryRehydratesPlaylistCredentials();

private:
    QTemporaryDir m_tempDir;
};

void StorageTest::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayerTest"));
    qputenv("MVP_DATA_DIR", m_tempDir.path().toUtf8());
    QVERIFY(QDir(StoragePaths::dataDir()).exists()
            || QDir().mkpath(StoragePaths::dataDir()));
}

void StorageTest::init()
{
    QVERIFY(QDir(StoragePaths::dataDir()).removeRecursively());
    QVERIFY(QDir().mkpath(StoragePaths::dataDir()));
}

void StorageTest::cleanupTestCase()
{
    qunsetenv("MVP_DATA_DIR");
}

void StorageTest::playlistRoundTrip()
{
    MediaItem music;
    music.type = MediaType::Music;
    music.title = QStringLiteral("测试歌曲");
    music.artist = QStringLiteral("测试歌手");
    music.url = QUrl::fromLocalFile(QStringLiteral("C:/temp/music.mp3"));
    music.durationMs = 123000;
    music.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/"));

    MediaItem video;
    video.type = MediaType::Video;
    video.title = QStringLiteral("测试影片");
    video.url = QUrl(QStringLiteral("https://example.com/video.m3u8"));
    video.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    video.online = true;
    video.sourceId = QStringLiteral("web_test");
    video.year = QStringLiteral("2026");
    video.area = QStringLiteral("中国大陆");
    video.genre = QStringLiteral("测试");

    QVERIFY(PlaylistStore::saveAll({music}, {video}));
    QVERIFY(QFile::exists(StoragePaths::dataFile(QStringLiteral("playlists.json"))));

    const QVector<MediaItem> musicItems = PlaylistStore::load(MediaType::Music);
    QCOMPARE(musicItems.size(), 1);
    QCOMPARE(musicItems.first().type, MediaType::Music);
    QCOMPARE(musicItems.first().title, music.title);
    QCOMPARE(musicItems.first().artist, music.artist);
    QCOMPARE(musicItems.first().url, music.url);
    QCOMPARE(musicItems.first().durationMs, music.durationMs);
    QCOMPARE(musicItems.first().headers.value(QStringLiteral("Referer")),
             QStringLiteral("https://example.com/"));

    const QVector<MediaItem> videoItems = PlaylistStore::load(MediaType::Video);
    QCOMPARE(videoItems.size(), 1);
    QCOMPARE(videoItems.first().type, MediaType::Video);
    QCOMPARE(videoItems.first().title, video.title);
    QCOMPARE(videoItems.first().url, video.url);
    QCOMPARE(videoItems.first().webUrl, video.webUrl);
    QVERIFY(videoItems.first().online);
    QCOMPARE(videoItems.first().sourceId, video.sourceId);
    QCOMPARE(videoItems.first().year, video.year);
}

void StorageTest::playlistSaveWritesPrimaryOnly()
{
    const QString primary = StoragePaths::dataFile(QStringLiteral("playlists.json"));

    QVERIFY(PlaylistStore::saveAll({makeMusic(QStringLiteral("只写主路径"))}, {}));
    QVERIFY(QFile::exists(primary));

    const QVector<MediaItem> items = PlaylistStore::load(MediaType::Music);
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().title, QStringLiteral("只写主路径"));
}

void StorageTest::playlistCorruptFileBacksUp()
{
    const QString path = StoragePaths::dataFile(QStringLiteral("playlists.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("{ this is not a playlist");
    file.close();

    QVERIFY(PlaylistStore::load(MediaType::Music).isEmpty());

    const QStringList backups =
        QDir(StoragePaths::dataDir())
            .entryList({QStringLiteral("playlists.json.corrupt-*")}, QDir::Files);
    QCOMPARE(backups.size(), 1);
}

void StorageTest::playlistVideoProgressRoundTrip()
{
    MediaItem video = makeVideo(QStringLiteral("功夫熊猫4"));
    video.durationMs = 600000;
    video.positionMs = 42000;

    QVERIFY(PlaylistStore::saveAll({}, {video}));
    const QVector<MediaItem> loaded = PlaylistStore::load(MediaType::Video);
    QCOMPARE(loaded.size(), 1);
    // 进度直接跟列表条目一起落盘，不需要额外的进度文件。
    QCOMPARE(loaded.first().positionMs, 42000);
    QVERIFY(!QFile::exists(StoragePaths::dataFile(QStringLiteral("video_positions.json"))));

    MediaItem music = makeMusic(QStringLiteral("测试歌曲"));
    music.positionMs = 42000;
    const QJsonObject musicJson = mediaItemToJson(music);
    QVERIFY(!musicJson.contains(QStringLiteral("position")));
}

void StorageTest::playlistVideoProgressRemovedWithItem()
{
    PlaylistModel model;
    MediaItem video = makeVideo(QStringLiteral("会从列表移除的影片"));
    video.positionMs = 30000;
    QVERIFY(model.addItem(video));
    QVERIFY(PlaylistStore::saveAll({}, model.items()));
    QCOMPARE(PlaylistStore::load(MediaType::Video).first().positionMs, 30000);

    // 条目删除后重新保存列表，进度随条目一起消失，不再留有孤儿记录。
    QVERIFY(model.removeRow(0));
    QVERIFY(PlaylistStore::saveAll({}, model.items()));
    QVERIFY(PlaylistStore::load(MediaType::Video).isEmpty());
}

void StorageTest::sourceRegistrySeedsAndPersists()
{
    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(registry.lastError().isEmpty());
    QCOMPARE(registry.enabledCount(), 3);
    QVERIFY(QFile::exists(registry.storagePath()));

    SourceConfig custom;
    custom.id = QStringLiteral("test_custom");
    custom.name = QStringLiteral("测试源");
    custom.kind = QStringLiteral("config");
    custom.mediaType = MediaType::Music;
    custom.urlTemplate = QStringLiteral("https://example.com/search?q={keyword}");
    custom.itemsPath = QStringLiteral("items");
    custom.fields.insert(QStringLiteral("title"), QStringLiteral("title"));
    custom.fields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));
    custom.timeoutMs = 9000;
    custom.cookie = QStringLiteral("sid=storage-secret");
    custom.headers.insert(QStringLiteral("Authorization"),
                          QStringLiteral("Bearer storage-token"));

    QVERIFY(registry.upsert(custom));

    QFile stored(registry.storagePath());
    QVERIFY(stored.open(QIODevice::ReadOnly));
    const QString raw = QString::fromUtf8(stored.readAll());
    stored.close();
    QVERIFY(!raw.contains(QStringLiteral("sid=storage-secret")));
    QVERIFY(!raw.contains(QStringLiteral("storage-token")));

    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    const SourceConfig restored = reloaded.sourceById(QStringLiteral("test_custom"));
    QCOMPARE(restored.id, custom.id);
    QCOMPARE(restored.name, custom.name);
    QCOMPARE(restored.timeoutMs, 9000);
    if (SecretStore::isAvailable()) {
        QCOMPARE(restored.cookie, custom.cookie);
        QCOMPARE(restored.headers.value(QStringLiteral("Authorization")),
                 QStringLiteral("Bearer storage-token"));
    } else {
        QVERIFY(restored.cookie.isEmpty());
    }
}

void StorageTest::sourceRegistryReloadUsesPrimary()
{
    const QString primary = StoragePaths::dataFile(QStringLiteral("sources.json"));

    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(registry.save());
    QVERIFY(QFile::exists(primary));

    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.storagePath(), primary);
}

void StorageTest::sourceRegistryCorruptFileBacksUp()
{
    const QString path = StoragePaths::dataFile(QStringLiteral("sources.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("{ broken");
    file.close();

    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(!registry.lastError().isEmpty());
    QCOMPARE(registry.enabledCount(), 3);

    const QStringList backups =
        QDir(StoragePaths::dataDir())
            .entryList({QStringLiteral("sources.json.corrupt-*")}, QDir::Files);
    QCOMPARE(backups.size(), 1);

    // 损坏文件已被备份并恢复为可解析的预置源，下一次启动不会再报损坏
    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    QVERIFY(reloaded.lastError().isEmpty());
}

void StorageTest::sourceRegistryUnwritableDataDirFails()
{
    QTemporaryDir blocked;
    QVERIFY(blocked.isValid());
    // 把主数据目录指向普通文件，mkpath/打开都会失败，模拟安装目录只读。
    const QString blocker = QDir(blocked.path()).filePath(QStringLiteral("blocked"));
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("blocked");
    file.close();

    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    qputenv("MVP_DATA_DIR", blocker.toUtf8());

    SourceRegistry registry;
    const bool loaded = registry.load();
    const QString error = registry.lastError();

    if (oldDataDir.isEmpty())
        qunsetenv("MVP_DATA_DIR");
    else
        qputenv("MVP_DATA_DIR", oldDataDir);

    // 只写程序目录：主路径不可写就直接失败，不再偷偷改写到 AppData。
    QVERIFY(!loaded);
    QVERIFY(!error.isEmpty());
}

void StorageTest::downloadJobRoundTripEncryptsSensitiveHeaders()
{
    DownloadJob job;
    job.id = QStringLiteral("job-1");
    job.item.type = MediaType::Video;
    job.item.title = QStringLiteral("测试下载");
    job.item.url = QUrl(QStringLiteral("https://example.com/a.m3u8"));
    job.item.online = true;
    job.url = job.item.url;
    job.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/"));
    job.headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret-token"));
    job.filePath = m_tempDir.filePath(QStringLiteral("测试下载.ts"));
    job.bytesReceived = 1200;
    job.bytesTotal = 5000;
    job.maxBytes = 8192;
    job.segmentIndex = 2;
    job.segmentCount = 10;
    job.hls = true;
    job.committed = true;
    job.status = DownloadJob::Status::Paused;
    job.message = QStringLiteral("等待续传");
    job.createdAt = QDateTime::currentDateTime();

    const QJsonObject stored = job.toJson();
    // 敏感请求头仍保留键，但值必须是 DPAPI 密文，不能出现明文 token。
    const QString storedCookie =
        stored.value(QStringLiteral("headers")).toObject().value(QStringLiteral("Cookie")).toString();
    QVERIFY(!storedCookie.isEmpty());
    QVERIFY(storedCookie != job.headers.value(QStringLiteral("Cookie")));
    QVERIFY(storedCookie.startsWith(QStringLiteral("mvp-dpapi:v1:")));
    QVERIFY(stored.value(QStringLiteral("headers")).toObject()
                .contains(QStringLiteral("Referer")));
    // 直链 URL 也可能带签名/API key：JSON 里不能同时存在明文 url。
    QVERIFY(!stored.contains(QStringLiteral("url")));
    QVERIFY(stored.contains(QStringLiteral("urlProtected")));
    QVERIFY(!stored.value(QStringLiteral("item")).toObject().contains(QStringLiteral("url")));

    DownloadJob parsed;
    QString error;
    QVERIFY(DownloadJob::fromJson(stored, &parsed, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(parsed.id, job.id);
    QCOMPARE(parsed.item.title, job.item.title);
    QCOMPARE(parsed.filePath, job.filePath);
    QCOMPARE(parsed.bytesReceived, job.bytesReceived);
    QCOMPARE(parsed.bytesTotal, job.bytesTotal);
    QCOMPARE(parsed.maxBytes, job.maxBytes);
    QCOMPARE(parsed.segmentIndex, job.segmentIndex);
    QCOMPARE(parsed.segmentCount, job.segmentCount);
    QCOMPARE(parsed.hls, true);
    QCOMPARE(parsed.committed, true);
    QCOMPARE(parsed.url, job.url);
    QCOMPARE(parsed.item.url, job.url);
    QCOMPARE(parsed.status, DownloadJob::Status::Paused);
    QCOMPARE(parsed.headers.value(QStringLiteral("Referer")),
             job.headers.value(QStringLiteral("Referer")));
    QCOMPARE(parsed.headers.value(QStringLiteral("Cookie")),
             job.headers.value(QStringLiteral("Cookie")));
}

void StorageTest::downloadPathsAvoidOverwriteAndPickSuffix()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("downloads"));
    QVERIFY(QDir().mkpath(dir));

    const QString first = DownloadPaths::uniqueFilePath(dir, QStringLiteral("视频.mp4"));
    QCOMPARE(first, QDir(dir).filePath(QStringLiteral("视频.mp4")));

    QFile existing(first);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.close();

    const QString second = DownloadPaths::uniqueFilePath(dir, QStringLiteral("视频.mp4"));
    QCOMPARE(second, QDir(dir).filePath(QStringLiteral("视频 (1).mp4")));

    QCOMPARE(DownloadPaths::suffixForDownload(
                 QUrl(QStringLiteral("https://example.com/list.m3u8")), true, true),
             QStringLiteral("ts"));
    QCOMPARE(DownloadPaths::suffixForDownload(
                 QUrl(QStringLiteral("https://example.com/video.MP4?token=x")), false, true),
             QStringLiteral("mp4"));
    QCOMPARE(DownloadPaths::suffixForDownload(
                 QUrl(QStringLiteral("https://example.com/play?id=1")), false, true),
             QStringLiteral("mp4"));
    QCOMPARE(DownloadPaths::suffixForDownload(
                 QUrl(QStringLiteral("https://example.com/play?id=1")), false, false),
             QStringLiteral("mp3"));
}

void StorageTest::downloadPathsTreatPartialAsOccupied()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("downloads-partial"));
    QVERIFY(QDir().mkpath(dir));

    const QString base = QDir(dir).filePath(QStringLiteral("半成品.mp4"));
    QFile partial(base + QStringLiteral(".mvpdownload"));
    QVERIFY(partial.open(QIODevice::WriteOnly));
    partial.write("partial");
    partial.close();

    // 最终文件不存在，但同名半成品存在时也不能复用，否则两个任务会交错写。
    const QString reserved = DownloadPaths::uniqueFilePath(dir, QStringLiteral("半成品.mp4"));
    QCOMPARE(reserved, QDir(dir).filePath(QStringLiteral("半成品 (1).mp4")));

    // 队列内已预留的路径也算冲突，即使磁盘上还看不到文件。
    const QString queueReserved =
        DownloadPaths::uniqueFilePath(dir, QStringLiteral("队列占用.mp4"),
                                      [](const QString &candidate) {
                                          return candidate.endsWith(QStringLiteral("队列占用.mp4"));
                                      });
    QCOMPARE(queueReserved, QDir(dir).filePath(QStringLiteral("队列占用 (1).mp4")));
}

void StorageTest::downloadManagerReservesActiveTarget()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("downloads-queue"));
    QVERIFY(QDir().mkpath(dir));
    const QString base = QDir(dir).filePath(QStringLiteral("同名.mp3"));

    HttpClient http;
    DownloadManager manager(&http);
    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("同名");
    item.url = QUrl(QStringLiteral("http://127.0.0.1:1/a.mp3"));
    item.online = true;
    item.sourceId = QStringLiteral("test");
    QString ignored;
    ignored = manager.enqueue(item, item.url, {}, base, false, 1024);
    QVERIFY(!ignored.isEmpty());

    // 第一个任务已经预留 base，第二个同名任务必须自动换名。
    const QString reserved = manager.reserveFilePath(dir, QStringLiteral("同名.mp3"));
    QCOMPARE(reserved, QDir(dir).filePath(QStringLiteral("同名 (1).mp3")));
    manager.shutdownForExit();
}

void StorageTest::sourceRegistryRehydratesPlaylistCredentials()
{
    SourceRegistry registry;
    QVERIFY(registry.load());

    SourceConfig config;
    config.id = QStringLiteral("cred-test");
    config.name = QStringLiteral("凭据测试源");
    config.kind = QStringLiteral("config");
    config.mediaType = MediaType::Music;
    config.urlTemplate = QStringLiteral("https://source.example/api?q={keyword}");
    config.fields.insert(QStringLiteral("title"), QStringLiteral("title"));
    config.fields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));
    config.cookie = QStringLiteral("sid=playlist-secret");
    config.headers.insert(QStringLiteral("Authorization"),
                          QStringLiteral("Bearer playlist-token"));
    QVERIFY(registry.upsert(config));

    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("已保存条目");
    item.url = QUrl(QStringLiteral("https://source.example/a.mp3"));
    item.online = true;
    item.sourceId = config.id;

    // 模拟播放列表持久化：落盘前剥掉 Cookie，重启后从 JSON 读回。
    const QJsonObject stored = mediaItemToJson(item);
    const MediaItem restored = mediaItemFromJson(stored);
    QVERIFY(!restored.headers.contains(QStringLiteral("Cookie")));

    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    const MediaItem hydrated = reloaded.mediaItemWithCredentials(restored);
    QCOMPARE(hydrated.sourceOrigin, QUrl(QStringLiteral("https://source.example")));
    QCOMPARE(QUrl(hydrated.headers.value(QStringLiteral("Referer"))),
             QUrl(QStringLiteral("https://source.example/api")));
    QVERIFY(hydrated.headers.contains(QStringLiteral("Origin")));
    if (SecretStore::isAvailable()) {
        QCOMPARE(hydrated.headers.value(QStringLiteral("Cookie")),
                 QStringLiteral("sid=playlist-secret"));
        QCOMPARE(hydrated.headers.value(QStringLiteral("Authorization")),
                 QStringLiteral("Bearer playlist-token"));
    }
}

QTEST_GUILESS_MAIN(StorageTest)

#include "test_storage.moc"
