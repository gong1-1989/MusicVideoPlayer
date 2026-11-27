/// @file test_storage.cpp
/// @brief 存储与安全测试：播放列表/下载任务持久化、下载路径、播放缓存、
///        源配置与 DPAPI 凭据保护。
#include "core/PlaylistModel.h"
#include "core/PlaylistStore.h"
#include "download/DownloadJob.h"
#include "download/DownloadManager.h"
#include "download/DownloadPaths.h"
#include "cache/MediaCache.h"
#include "net/HttpClient.h"
#include "net/SourceRegistry.h"
#include "storage/StoragePaths.h"
#include "util/SecretStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTcpServer>
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
    void playlistMissingPartitionsIsCorrupt();
    void playlistSnapshotRecoversAfterCorruption();
    void playlistSnapshotReadErrorBlocksSave();
    void playlistSnapshotCorruptBlocksSave();
    void playlistVideoProgressRoundTrip();
    void playlistVideoProgressRemovedWithItem();
    void sourceRegistrySeedsAndPersists();
    void sourceRegistryRemovesRetiredSeeds();
    void sourceRegistryReloadUsesPrimary();
    void sourceRegistryCorruptFileBacksUp();
    void sourceRegistryCorruptBackupIsPreserved();
    void sourceRegistryRestoresSnapshot();
    void sourceRegistryCorruptBackupNameCollision();
    void sourceRegistryUnwritableDataDirFails();
    void sourceRegistryDirectoryPathFailsWithoutOverwrite();
    void downloadJobRoundTripEncryptsSensitiveHeaders();
    void downloadPathsAvoidOverwriteAndPickSuffix();
    void downloadPathsTreatPartialAsOccupied();
    void downloadManagerReservesActiveTarget();
    void downloadManagerRejectsOversizedStateFile();
    void downloadManagerRejectsStateWithoutJobsArray();
    void downloadManagerKeepsPartialOwnedByActiveTarget();
    void downloadManagerDelayedRemoveSkipsReservedPath();
    void downloadManagerDoesNotResumeSnapshot();
    void sourceRegistryRehydratesPlaylistCredentials();
    void mediaCacheRefusesAmbiguousLegacyQueryCache();

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

void StorageTest::playlistSnapshotRecoversAfterCorruption()
{
    const QString primary = PlaylistStore::filePath();
    const QString snapshot = PlaylistStore::backupFilePath();
    QVERIFY(PlaylistStore::saveAll({makeMusic(QStringLiteral("快照恢复"))}, {}));
    QVERIFY(QFile::exists(snapshot));

    // 主文件损坏后，读取结果必须从最近一次成功快照恢复，而不是返回空列表。
    QFile file(primary);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate));
    file.write("{ broken");
    file.close();

    const PlaylistStore::LoadResult result = PlaylistStore::loadAll();
    QVERIFY(!result.storageBlocked);
    QVERIFY(result.recoveredFromBackup);
    QVERIFY(!result.message.isEmpty());
    QCOMPARE(result.music.size(), 1);
    QCOMPARE(result.music.first().title, QStringLiteral("快照恢复"));
    QVERIFY(QFile::exists(result.corruptBackupPath));
}

void StorageTest::playlistSnapshotReadErrorBlocksSave()
{
    // 主文件缺失、快照路径被目录占用，读取状态是 ReadError 而不是“没有快照”。
    // 快照里可能仍是用户列表的最后一份有效数据，此时必须冻结保存，避免退出
    // 或下一次修改用空/新列表覆盖它。
    const QString primary = PlaylistStore::filePath();
    const QString snapshot = PlaylistStore::backupFilePath();
    QVERIFY(!QFileInfo::exists(primary));
    QVERIFY(QDir().mkpath(snapshot));

    const PlaylistStore::LoadResult result = PlaylistStore::loadAll();
    QVERIFY(result.storageBlocked);
    QVERIFY(result.music.isEmpty());
    QVERIFY(result.video.isEmpty());
    QVERIFY(result.message.contains(QStringLiteral("禁止保存")));
}

void StorageTest::playlistSnapshotCorruptBlocksSave()
{
    // 主文件缺失、快照内容损坏时，快照仍可能是用户列表的最后一份数据。
    // 此时不能把它当成“空列表”继续允许保存，否则退出/下一次修改会把
    // 损坏但可人工恢复的快照直接覆盖掉。
    const QString primary = PlaylistStore::filePath();
    const QString snapshot = PlaylistStore::backupFilePath();
    QVERIFY(!QFileInfo::exists(primary));
    QFile file(snapshot);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write("{ not json") > 0);
    file.close();

    const PlaylistStore::LoadResult result = PlaylistStore::loadAll();
    QVERIFY(result.storageBlocked);
    QVERIFY(result.music.isEmpty());
    QVERIFY(result.video.isEmpty());
    QVERIFY(result.message.contains(QStringLiteral("禁止保存")));
    // 损坏快照必须改名保留，而不是原地覆盖或直接删除。
    QVERIFY(!QFile::exists(snapshot));
    const QFileInfoList backups = QDir(StoragePaths::dataDir())
                                      .entryInfoList({QStringLiteral("playlists.json.bak.corrupt-*")},
                                                     QDir::Files);
    QVERIFY(!backups.isEmpty());
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
    // 现在只保留网易云、大米星球、Audius 三个长期可用内置源，默认全部启用。
    QCOMPARE(registry.enabledSources().size(), 3);
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

void StorageTest::sourceRegistryRemovesRetiredSeeds()
{
    // 模拟旧版本 sources.json：历史示例/试验源仍留在文件里。
    const QString path = StoragePaths::dataFile(QStringLiteral("sources.json"));
    QJsonArray array;
    for (const QString &id : {QStringLiteral("itunes"), QStringLiteral("radio_browser"),
                              QStringLiteral("ccmixter"), QStringLiteral("web_studioslob"),
                              QStringLiteral("movie_example")}) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), id);
        object.insert(QStringLiteral("name"), id);
        object.insert(QStringLiteral("enabled"), true);
        array.append(object);
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(QJsonDocument(array).toJson());
    file.close();

    SourceRegistry registry;
    QVERIFY(registry.load());
    for (const QString &id : {QStringLiteral("itunes"), QStringLiteral("radio_browser"),
                              QStringLiteral("ccmixter"), QStringLiteral("web_studioslob"),
                              QStringLiteral("movie_example")}) {
        QVERIFY2(registry.sourceById(id).id.isEmpty(), qPrintable(id));
    }
    QCOMPARE(registry.sources().size(), 3);

    // 清理结果应回写，下一次启动不再出现旧示例源。
    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.sources().size(), 3);
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
    QCOMPARE(registry.enabledSources().size(), 3);

    const QStringList backups =
        QDir(StoragePaths::dataDir())
            .entryList({QStringLiteral("sources.json.corrupt-*")}, QDir::Files);
    QCOMPARE(backups.size(), 1);

    // 损坏文件已被备份并恢复为可解析的预置源，下一次启动不会再报损坏
    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    QVERIFY(reloaded.lastError().isEmpty());
}

void StorageTest::sourceRegistryCorruptBackupIsPreserved()
{
    // 主文件缺失、快照损坏时，快照仍可能是用户自定义源的最后一份数据。
    // 不能直接写预置源把它覆盖掉；应先把损坏快照改名保留，再恢复预置源。
    const QString primary = StoragePaths::dataFile(QStringLiteral("sources.json"));
    const QString snapshot = StoragePaths::dataFile(QStringLiteral("sources.json.bak"));
    QVERIFY(!QFileInfo::exists(primary));
    QFile file(snapshot);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(file.write("{ broken backup") > 0);
    file.close();

    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(registry.lastError().contains(QStringLiteral("备份")));
    QCOMPARE(registry.enabledSources().size(), 3);

    const QStringList backups =
        QDir(StoragePaths::dataDir())
            .entryList({QStringLiteral("sources.json.bak.corrupt-*")}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    // 新快照由恢复后的预置源重新生成，损坏快照已另存。
    QVERIFY(QFileInfo::exists(snapshot));
}

void StorageTest::sourceRegistryRestoresSnapshot()
{
    const QString primary = StoragePaths::dataFile(QStringLiteral("sources.json"));
    const QString snapshot = StoragePaths::dataFile(QStringLiteral("sources.json.bak"));

    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(registry.save());
    QVERIFY(QFile::exists(primary));
    QVERIFY(QFile::exists(snapshot));
    const int expectedEnabled = registry.enabledSources().size();

    // 主文件损坏但快照仍有效：应优先从最近一次成功保存恢复，而不是退回预置源。
    QFile file(primary);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("{ broken primary");
    file.close();

    SourceRegistry reloaded;
    QVERIFY(reloaded.load());
    QVERIFY(reloaded.lastError().contains(QStringLiteral("备份恢复")));
    QCOMPARE(reloaded.enabledSources().size(), expectedEnabled);
    QVERIFY(!reloaded.lastError().isEmpty());

    // 恢复后必须把快照内容回写主文件，下一次启动不再依赖备份。
    SourceRegistry again;
    QVERIFY(again.load());
    QVERIFY(again.lastError().isEmpty());
    QCOMPARE(again.enabledSources().size(), expectedEnabled);
}

void StorageTest::sourceRegistryCorruptBackupNameCollision()
{
    const QString path = StoragePaths::dataFile(QStringLiteral("sources.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("{ broken collision");
    file.close();

    // 预先占住当前秒及后两秒的固定时间戳备份名，模拟同一秒内连续损坏。
    const QDateTime now = QDateTime::currentDateTime();
    QStringList occupied;
    for (int i = 0; i < 3; ++i) {
        const QString backup =
            path + QStringLiteral(".corrupt-")
            + now.addSecs(i).toString(QStringLiteral("yyyyMMddHHmmss"));
        QVERIFY(QFile::copy(path, backup));
        occupied.append(backup);
    }

    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(registry.lastError().contains(QStringLiteral("已备份")));

    const QStringList backups =
        QDir(StoragePaths::dataDir())
            .entryList({QStringLiteral("sources.json.corrupt-*")}, QDir::Files);
    // 三个预占文件 + 本次带序号的新备份，不能因为撞名而放弃备份/启动失败。
    QVERIFY(backups.size() >= occupied.size() + 1);
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

void StorageTest::sourceRegistryDirectoryPathFailsWithoutOverwrite()
{
    const QString path = StoragePaths::dataFile(QStringLiteral("sources.json"));
    // 目录占了 sources.json 的位置：不是“内容损坏”，不能备份/删除/覆盖，
    // 也不能用预置源把现场抹掉，只能明确报错让用户处理。
    QVERIFY(QDir().mkpath(path));

    SourceRegistry registry;
    QVERIFY(!registry.load());
    QVERIFY(registry.lastError().contains(QStringLiteral("不是普通文件")));
    QVERIFY(QDir(path).exists());

    const QStringList backups =
        QDir(StoragePaths::dataDir())
            .entryList({QStringLiteral("sources.json.corrupt-*")}, QDir::Files);
    QVERIFY(backups.isEmpty());
}

void StorageTest::downloadJobRoundTripEncryptsSensitiveHeaders()
{
    DownloadJob job;
    job.id = QStringLiteral("job-1");
    job.item.type = MediaType::Video;
    job.item.title = QStringLiteral("测试下载");
    job.item.url = QUrl(QStringLiteral("https://example.com/a.m3u8"));
    job.item.alternateUrls.append(QStringLiteral("https://cdn.example.com/a.m3u8?sign=secret"));
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
    job.snapshot = true;
    job.allowIncomplete = true;
    job.needsSnapshotConsent = true;
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
    QVERIFY(!stored.value(QStringLiteral("item")).toObject().contains(QStringLiteral("altUrls")));

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
    QCOMPARE(parsed.snapshot, true);
    QCOMPARE(parsed.allowIncomplete, true);
    QCOMPARE(parsed.needsSnapshotConsent, true);
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

    // 下载根目录下按媒体类型固定分“音乐”“影视”，根目录为空时保持空值
    QCOMPARE(DownloadPaths::mediaDirectory(dir, false),
             QDir(dir).filePath(QStringLiteral("音乐")));
    QCOMPARE(DownloadPaths::mediaDirectory(dir, true),
             QDir(dir).filePath(QStringLiteral("影视")));
    QCOMPARE(DownloadPaths::mediaDirectory(QString(), true), QString());
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

    // 队列里另一个任务的最终名可能是 `HLS占用.mp4.ts`（或它的中间名），
    // 只按最终名比较会让当前任务拿到 `HLS占用.mp4`，随后与派生文件撞名。
    const QString hlsReserved =
        DownloadPaths::uniqueFilePath(dir, QStringLiteral("HLS占用.mp4"),
                                      [](const QString &candidate) {
                                          return candidate.endsWith(
                                              QStringLiteral("HLS占用.mp4.ts"));
                                      });
    QCOMPARE(hlsReserved, QDir(dir).filePath(QStringLiteral("HLS占用 (1).mp4")));
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

void StorageTest::playlistMissingPartitionsIsCorrupt()
{
    const QString path = StoragePaths::dataFile(QStringLiteral("playlists.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    // 合法 JSON 对象，但缺少 music/video 数组：旧实现会当成空列表正常加载，
    // 退出保存时用空列表覆盖仍可能可恢复的备份。
    file.write("{\"version\": 1}");
    file.close();

    const PlaylistStore::LoadResult result = PlaylistStore::loadAll();
    QVERIFY(result.storageBlocked);
    QVERIFY(result.music.isEmpty());
    QVERIFY(result.video.isEmpty());
    QVERIFY(!result.corruptBackupPath.isEmpty());
}

void StorageTest::downloadManagerRejectsStateWithoutJobsArray()
{
    const QString statePath = StoragePaths::dataFile(QStringLiteral("downloads.json"));
    QFile file(statePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // 结构损坏时不能静默读成空队列，否则下一次保存会覆盖全部下载记录。
    QVERIFY(file.write("{\"version\": 1, \"jobs\": {}}") > 0);
    file.close();

    HttpClient http;
    DownloadManager manager(&http);
    QVERIFY(manager.isEmpty());
    QVERIFY(!QFile::exists(statePath));
    QVERIFY(QFile::exists(statePath + QStringLiteral(".corrupt")));
}

void StorageTest::downloadManagerRejectsOversizedStateFile()
{
    const QString statePath = StoragePaths::dataFile(QStringLiteral("downloads.json"));
    QFile file(statePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // 超过 16 MB 的状态不能整体读进内存；加载前应先备份并放弃。
    QVERIFY(file.write(QByteArray(16 * 1024 * 1024 + 1, ' ')) == 16 * 1024 * 1024 + 1);
    file.close();

    HttpClient http;
    DownloadManager manager(&http);
    QVERIFY(manager.isEmpty());
    QVERIFY(!QFile::exists(statePath));
    QVERIFY(QFile::exists(statePath + QStringLiteral(".corrupt")));
}

void StorageTest::downloadManagerKeepsPartialOwnedByActiveTarget()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("downloads-conflict"));
    QVERIFY(QDir().mkpath(dir));
    const QString target = QDir(dir).filePath(QStringLiteral("同名.mp3"));
    const QString oldPartial = target + QStringLiteral(".mvpdownload");

    DownloadJob first;
    first.id = QStringLiteral("first");
    first.item.type = MediaType::Music;
    first.item.title = QStringLiteral("同名");
    first.item.url = QUrl(QStringLiteral("http://127.0.0.1:1/a.mp3"));
    first.item.online = true;
    first.url = first.item.url;
    first.filePath = target;
    first.status = DownloadJob::Status::Paused;

    DownloadJob second = first;
    second.id = QStringLiteral("second");

    QJsonArray jobs{first.toJson(), second.toJson()};
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), jobs);
    QFile state(StoragePaths::dataFile(QStringLiteral("downloads.json")));
    QVERIFY(state.open(QIODevice::WriteOnly));
    state.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    state.close();

    QFile partial(oldPartial);
    QVERIFY(partial.open(QIODevice::WriteOnly));
    partial.write("partial");
    partial.close();

    HttpClient http;
    DownloadManager manager(&http);
    QCOMPARE(manager.jobs().size(), 2);
    QVERIFY(manager.jobs().at(1).filePath != target);
    // 旧路径仍由第一条 Paused 任务持有，第二条改到新路径后不能删除这条
    // 共享续传点，否则第一条任务恢复时会从 0 重下。
    QVERIFY(QFile::exists(oldPartial));
}

void StorageTest::downloadManagerDelayedRemoveSkipsReservedPath()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("downloads-delayed-remove"));
    QVERIFY(QDir().mkpath(dir));
    const QString target = QDir(dir).filePath(QStringLiteral("同路径.mp3"));
    const QString partial = target + QStringLiteral(".mvpdownload");

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    DownloadJob old;
    old.id = QStringLiteral("old");
    old.item.type = MediaType::Music;
    old.item.title = QStringLiteral("同路径");
    old.item.url = QUrl(QStringLiteral("http://127.0.0.1:1/a.mp3"));
    old.item.online = true;
    old.url = old.item.url;
    old.filePath = target;
    old.status = DownloadJob::Status::Paused;

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), QJsonArray{old.toJson()});
    QFile state(StoragePaths::dataFile(QStringLiteral("downloads.json")));
    QVERIFY(state.open(QIODevice::WriteOnly));
    state.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    state.close();

    QFile locked(partial);
    QVERIFY(locked.open(QIODevice::ReadWrite));
    QVERIFY(locked.write("old partial") > 0);
    QVERIFY(locked.flush());

    HttpClient http;
    DownloadManager manager(&http);
    QCOMPARE(manager.jobs().size(), 1);

    // 旧半成品仍被本测试句柄占着，首次同步删除会失败，进入延迟重试。
    QVERIFY(manager.removeJob(QStringLiteral("old")));
    locked.close();

    // 延迟窗口内立刻发起同目标新任务；用挂起不响应的本地端口让任务保持 active。
    const QUrl hanging(QStringLiteral("http://127.0.0.1:%1/a.mp3").arg(server.serverPort()));
    QString newId;
    newId = manager.enqueue(old.item, hanging, {}, target, false, 1024);
    QVERIFY(!newId.isEmpty());

    // 新架构下所有任务都写应用暂存区；延迟删除必须认这个真正在用的半成品。
    QString freshPartial;
    for (const DownloadJob &job : manager.jobs()) {
        if (job.id == newId) {
            freshPartial = job.partialPath();
            break;
        }
    }
    QVERIFY(!freshPartial.isEmpty());
    QVERIFY(QDir().mkpath(QFileInfo(freshPartial).absolutePath()));
    QFile fresh(freshPartial);
    QVERIFY(fresh.open(QIODevice::WriteOnly));
    QVERIFY(fresh.write("new partial") > 0);
    fresh.close();

    // 旧任务延迟删除的是它自己的旧路径；新架构下新任务使用独立暂存目录，
    // 不能被旧任务的延迟清理误删。
    QTest::qWait(140);
    QVERIFY(QFile::exists(freshPartial));

    manager.shutdownForExit();
    server.close();
}

void StorageTest::downloadManagerDoesNotResumeSnapshot()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("downloads-snapshot"));
    QVERIFY(QDir().mkpath(dir));
    const QString target = QDir(dir).filePath(QStringLiteral("快照.ts"));

    DownloadJob job;
    job.id = QStringLiteral("snapshot");
    job.item.type = MediaType::Video;
    job.item.title = QStringLiteral("快照");
    job.item.url = QUrl(QStringLiteral("http://127.0.0.1:1/a.m3u8"));
    job.item.online = true;
    job.url = job.item.url;
    job.filePath = target;
    job.hls = true;
    job.snapshot = true;
    job.segmentIndex = 3;
    job.segmentCount = 6;
    job.status = DownloadJob::Status::Downloading;

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), QJsonArray{job.toJson()});
    QFile state(StoragePaths::dataFile(QStringLiteral("downloads.json")));
    QVERIFY(state.open(QIODevice::WriteOnly));
    state.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    state.close();

    QFile partial(target + QStringLiteral(".mvpdownload"));
    QVERIFY(partial.open(QIODevice::WriteOnly));
    partial.write("partial");
    partial.close();

    HttpClient http;
    DownloadManager manager(&http);
    QCOMPARE(manager.jobs().size(), 1);
    // 快照列表可能跨启动变化，不能拿旧分片序号自动续传。
    QCOMPARE(manager.jobs().at(0).status, DownloadJob::Status::Failed);
    QVERIFY(!manager.jobs().at(0).committed);
    QVERIFY(manager.jobs().at(0).message.contains(QStringLiteral("快照任务")));
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

void StorageTest::mediaCacheRefusesAmbiguousLegacyQueryCache()
{
    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("旧缓存歌曲");
    item.url = QUrl(QStringLiteral("https://example.com/song?id=1&token=old"));
    item.online = true;

    // 旧版 identity 会丢掉整个查询串；新版保留 id、剔除 token。
    // 旧 meta 只存了不带 query 的地址，无法判断 `id=1` 与 `id=2` 哪个
    // 才是这份缓存的主人，宁可拒绝迁移，也不能把别的歌搬过来。
    const QString oldIdentity = QStringLiteral("url:https://example.com/song");
    const QString oldDigest =
        QCryptographicHash::hash(oldIdentity.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(32);
    const QString oldDir = MediaCache::rootDir() + QLatin1Char('/') + oldDigest;
    QVERIFY(QDir().mkpath(oldDir));
    const QString oldMedia = oldDir + QStringLiteral("/media.mp3");
    {
        QFile file(oldMedia);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray(1234, 'x')) == 1234);
    }
    QJsonObject meta;
    meta.insert(QStringLiteral("version"), 1);
    meta.insert(QStringLiteral("url"), QStringLiteral("https://example.com/song"));
    meta.insert(QStringLiteral("total"), 1234);
    meta.insert(QStringLiteral("complete"), true);
    meta.insert(QStringLiteral("contentType"), QStringLiteral("audio/mpeg"));
    QFile metaFile(oldDir + QStringLiteral("/meta.json"));
    QVERIFY(metaFile.open(QIODevice::WriteOnly));
    metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
    metaFile.close();

    const QString migrated = MediaCache::completeFileForItem(item);
    QVERIFY(migrated.isEmpty());
    QVERIFY(QDir(oldDir).exists());
}

QTEST_GUILESS_MAIN(StorageTest)

#include "test_storage.moc"
