#include "core/PlaylistStore.h"
#include "net/SourceRegistry.h"
#include "storage/StoragePaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace {

QString fallbackFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/playlists.json");
}

bool writePlaylist(const QString &path, const QString &key, const QVector<MediaItem> &items)
{
    QJsonObject root;
    QJsonArray array;
    for (const MediaItem &item : items)
        array.append(mediaItemToJson(item));
    root.insert(key, array);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    return file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) >= 0;
}

MediaItem makeMusic(const QString &title)
{
    MediaItem item;
    item.type = MediaType::Music;
    item.title = title;
    item.url = QUrl(QStringLiteral("https://example.com/%1.mp3").arg(title));
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
    void playlistCorruptFileBacksUp();
    void playlistFallsBackToNewerCopy();
    void playlistReadsFallbackWhenPrimaryMissing();
    void sourceRegistrySeedsAndPersists();
    void sourceRegistryCorruptFileBacksUp();

private:
    QTemporaryDir m_tempDir;
};

void StorageTest::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayerTest"));
    QStandardPaths::setTestModeEnabled(true);
    qputenv("MVP_DATA_DIR", m_tempDir.path().toUtf8());
    QVERIFY(QDir(StoragePaths::dataDir()).exists()
            || QDir().mkpath(StoragePaths::dataDir()));
}

void StorageTest::init()
{
    QVERIFY(QDir(StoragePaths::dataDir()).removeRecursively());
    QVERIFY(QDir().mkpath(StoragePaths::dataDir()));
    const QString fallback = fallbackFilePath();
    if (!fallback.isEmpty())
        QFile::remove(fallback);
}

void StorageTest::cleanupTestCase()
{
    const QString fallback = fallbackFilePath();
    if (!fallback.isEmpty())
        QFile::remove(fallback);
    QStandardPaths::setTestModeEnabled(false);
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

void StorageTest::playlistFallsBackToNewerCopy()
{
    const QString primary = StoragePaths::dataFile(QStringLiteral("playlists.json"));
    const QString fallback = fallbackFilePath();
    QVERIFY(!fallback.isEmpty());
    QVERIFY(fallback != primary);

    QVERIFY(writePlaylist(primary, QStringLiteral("music"),
                          {makeMusic(QStringLiteral("旧列表"))}));
    // 模拟主路径可读但不可写：之后的新数据落在 fallback，主路径保留旧文件。
    // 明确把主文件时间调旧，避免 FAT/NTFS 时间戳精度影响判断。
    QFile oldFile(primary);
    QVERIFY(oldFile.open(QIODevice::ReadWrite));
    QVERIFY(oldFile.setFileTime(QDateTime::currentDateTime().addSecs(-10),
                                QFileDevice::FileModificationTime));
    oldFile.close();
    QVERIFY(writePlaylist(fallback, QStringLiteral("music"),
                          {makeMusic(QStringLiteral("新列表"))}));

    const QVector<MediaItem> items = PlaylistStore::load(MediaType::Music);
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().title, QStringLiteral("新列表"));
}

void StorageTest::playlistReadsFallbackWhenPrimaryMissing()
{
    const QString primary = StoragePaths::dataFile(QStringLiteral("playlists.json"));
    const QString fallback = fallbackFilePath();
    QVERIFY(!fallback.isEmpty());
    QVERIFY(!QFile::exists(primary));
    QVERIFY(writePlaylist(fallback, QStringLiteral("music"),
                          {makeMusic(QStringLiteral("备用列表"))}));

    const QVector<MediaItem> items = PlaylistStore::load(MediaType::Music);
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().title, QStringLiteral("备用列表"));
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

    QVERIFY(registry.upsert(custom));

    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    const SourceConfig restored = reloaded.sourceById(QStringLiteral("test_custom"));
    QCOMPARE(restored.id, custom.id);
    QCOMPARE(restored.name, custom.name);
    QCOMPARE(restored.timeoutMs, 9000);
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

QTEST_GUILESS_MAIN(StorageTest)

#include "test_storage.moc"
