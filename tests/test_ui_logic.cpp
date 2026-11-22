#include "core/PlaybackController.h"
#include "core/PlaylistStore.h"
#include "media/MediaItem.h"
#include "net/HttpClient.h"
#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "ui/LyricsController.h"
#include "ui/ListMetrics.h"
#include "ui/PlaylistController.h"
#include "ui/SeekSlider.h"
#include "ui/SettingsController.h"
#include "ui/ThemeManager.h"

#include <QFile>
#include <QListView>
#include <QMainWindow>
#include <QSlider>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QSignalSpy>

#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QWidget>

namespace {

/// 歌词接口延迟 500ms 返回一行旧歌词；用来验证切走后旧请求被取消。
class DelayedLyricsServer : public QTcpServer
{
public:
    explicit DelayedLyricsServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        body = QByteArrayLiteral("{\"lrc\":{\"lyric\":\"[00:01]旧歌词\"}}");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool())
                        return;
                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    if (!pending.contains("\r\n\r\n")) {
                        socket->setProperty("pending", pending);
                        return;
                    }
                    socket->setProperty("responded", true);
                    ++requestCount;

                    const QByteArray head =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n";
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(500, this, [this, guard, head] {
                        if (guard.isNull())
                            return;
                        guard->write(head);
                        guard->write(body);
                        guard->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    int requestCount = 0;
};

} // namespace

class UiLogicTest : public QObject
{
    Q_OBJECT

private slots:
    void lyricsClearCancelsPendingRequest();
    void remoteItemRejectsLocalLyricFile();
    void progressSliderClickJumpsToAbsolutePosition();
    void listPageSizeFollowsViewport();
    void themeFontScaleChangesFixedPixelSizes();
    void playlistSaveFailureKeepsDirtyAndRetries();
    void previewItemNeverPersistsUnlessPromoted();
    void downloadedItemSwitchesPlaylistToLocalSource();
    void settingsVolumeKeepsManualValueEvenWhenEqualToOldDefault();
};

void UiLogicTest::lyricsClearCancelsPendingRequest()
{
    DelayedLyricsServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    HttpClient http;
    LyricsController controller(&http);
    QSignalSpy loaded(&controller, &LyricsController::linesLoaded);

    MediaItem song;
    song.type = MediaType::Music;
    song.title = QStringLiteral("测试歌曲");
    song.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    song.lyricUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/lyric")
                             .arg(server.serverPort()));

    controller.setCurrentItem(song);
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount > 0, 2000);

    // 模拟切到无歌词条目/视频：旧请求必须立刻取消，500ms 后不能再回填歌词。
    controller.clear();
    QTest::qWait(900);
    QCOMPARE(loaded.count(), 0);
}

void UiLogicTest::remoteItemRejectsLocalLyricFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("secret.lrc"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("[00:01]不应被远程条目读取"));
    file.close();

    HttpClient http;
    LyricsController controller(&http);
    QSignalSpy loaded(&controller, &LyricsController::linesLoaded);

    MediaItem remote;
    remote.type = MediaType::Music;
    remote.title = QStringLiteral("远程歌曲");
    remote.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    remote.lyricUrl = QUrl::fromLocalFile(path);
    remote.online = true;

    controller.setCurrentItem(remote);
    QTest::qWait(300);
    QCOMPARE(loaded.count(), 0);

    // 同一路径对本地条目仍然可用，避免把正常的本地歌词一起禁掉。
    MediaItem local = remote;
    local.online = false;
    controller.setCurrentItem(local);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 3000);
}

void UiLogicTest::progressSliderClickJumpsToAbsolutePosition()
{
    SeekSlider slider;
    slider.setOrientation(Qt::Horizontal);
    slider.setRange(0, 600000);
    slider.resize(420, 32);
    slider.show();
    QTest::qWait(50);

    QSignalSpy released(&slider, &QSlider::sliderReleased);
    // 点击进度条正中间：默认 QSlider 只会前进 pageStep=10ms，旧实现等于没反应。
    QTest::mouseClick(&slider, Qt::LeftButton, Qt::NoModifier, QPoint(210, 16));

    QVERIFY(released.count() >= 1);
    QVERIFY2(qAbs(slider.value() - 300000) <= 30000,
             qPrintable(QStringLiteral("value=%1").arg(slider.value())));
}

void UiLogicTest::listPageSizeFollowsViewport()
{
    // 300px 视口、30px 行高 => 10 行；窗口变矮后页大小必须跟着变，而不是固定 20。
    QCOMPARE(ListMetrics::pageSizeForViewport(300, 30), 10);
    QCOMPARE(ListMetrics::pageSizeForViewport(150, 30), 5);
    QCOMPARE(ListMetrics::pageSizeForViewport(330, 30, 2), 10);
    QCOMPARE(ListMetrics::pageSizeForViewport(0, 30), 1);
    QCOMPARE(ListMetrics::pageSizeForViewport(300, 0), 1);
}

void UiLogicTest::themeFontScaleChangesFixedPixelSizes()
{
    const QString qss = QStringLiteral(
        "QWidget { font-size: 13px; } QLabel { font-size: 15px; }");

    QCOMPARE(ThemeManager::scaledStyleSheet(qss, 13), qss);
    QCOMPARE(ThemeManager::scaledStyleSheet(qss, 16),
             QStringLiteral("QWidget { font-size: 16px; } QLabel { font-size: 18px; }"));
    QCOMPARE(ThemeManager::scaledStyleSheet(qss, 10),
             QStringLiteral("QWidget { font-size: 10px; } QLabel { font-size: 12px; }"));
}

void UiLogicTest::playlistSaveFailureKeepsDirtyAndRetries()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    // 主路径用一个“文件”占位，QDir::mkpath 和 QSaveFile 都会失败。
    // 现在只写程序目录，没有备用路径兜底，保存应当直接失败。
    const QString fakePrimary = temp.filePath(QStringLiteral("not-a-dir"));
    QFile blocker(fakePrimary);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    qputenv("MVP_DATA_DIR", fakePrimary.toUtf8());

    PlaybackController player;
    QTabWidget tabs;
    // QTabWidget 会接管页面所有权；这里必须用堆对象，避免析构时重复释放栈对象。
    QListView *musicView = new QListView;
    QListView *videoView = new QListView;
    tabs.addTab(musicView, QStringLiteral("音乐"));
    tabs.addTab(videoView, QStringLiteral("视频"));

    PlaylistController controller(&player, PlaylistUi{musicView, videoView, &tabs});

    MediaItem song;
    song.type = MediaType::Music;
    song.title = QStringLiteral("未保存测试");
    song.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    song.online = true;

    controller.addSearchItem(song, MediaType::Music);
    QVERIFY(controller.hasUnsavedChanges());
    QCOMPARE(tabs.tabText(0), QStringLiteral("音乐 *"));
    QVERIFY(tabs.tabToolTip(0).contains(QStringLiteral("未保存")));

    // 数据目录恢复可写后重试必须成功，并清除页签上的未保存标记。
    qputenv("MVP_DATA_DIR", temp.path().toUtf8());
    QVERIFY(controller.retrySave());
    QVERIFY(!controller.hasUnsavedChanges());
    QCOMPARE(tabs.tabText(0), QStringLiteral("音乐"));
    QCOMPARE(tabs.tabText(1), QStringLiteral("视频"));

    if (oldDataDir.isEmpty())
        qunsetenv("MVP_DATA_DIR");
    else
        qputenv("MVP_DATA_DIR", oldDataDir);
}

void UiLogicTest::previewItemNeverPersistsUnlessPromoted()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    qputenv("MVP_DATA_DIR", temp.path().toUtf8());

    PlaybackController player;
    QTabWidget tabs;
    QListView *musicView = new QListView;
    QListView *videoView = new QListView;
    tabs.addTab(musicView, QStringLiteral("音乐"));
    tabs.addTab(videoView, QStringLiteral("视频"));
    PlaylistController controller(&player, PlaylistUi{musicView, videoView, &tabs});

    MediaItem preview;
    preview.type = MediaType::Music;
    preview.title = QStringLiteral("试听条目");
    preview.url = QUrl(QStringLiteral("https://example.com/preview.mp3?token=old"));
    preview.online = true;
    preview.previewOnly = true;

    // 预览条目可以出现在模型里，但保存入口必须统一过滤，绝不落盘。
    QVERIFY(controller.musicModel()->addItem(preview));
    QVERIFY(!controller.hasUnsavedChanges());
    QVERIFY(controller.save());
    QVERIFY(PlaylistStore::load(MediaType::Music).isEmpty());

    // 用户把同一内容加入搜索列表：预览行提升为永久条目并立即保存，
    // 同时把已经过期的签名直链刷新成新地址。
    MediaItem fresh = preview;
    fresh.url = QUrl(QStringLiteral("https://example.com/preview.mp3?token=new"));
    fresh.previewOnly = false;
    controller.addSearchItem(fresh, MediaType::Music);
    const QVector<MediaItem> saved = PlaylistStore::load(MediaType::Music);
    QCOMPARE(saved.size(), 1);
    QVERIFY(!saved.first().previewOnly);
    QCOMPARE(saved.first().url, fresh.url);

    // 已持久化的同 identity 条目再次加入时也必须刷新地址，而不是只报“已存在”。
    MediaItem refreshed = fresh;
    refreshed.url = QUrl(QStringLiteral("https://example.com/preview.mp3?token=latest"));
    controller.addSearchItem(refreshed, MediaType::Music);
    const QVector<MediaItem> latest = PlaylistStore::load(MediaType::Music);
    QCOMPARE(latest.size(), 1);
    QCOMPARE(latest.first().url, refreshed.url);

    if (oldDataDir.isEmpty())
        qunsetenv("MVP_DATA_DIR");
    else
        qputenv("MVP_DATA_DIR", oldDataDir);
}

void UiLogicTest::downloadedItemSwitchesPlaylistToLocalSource()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    qputenv("MVP_DATA_DIR", temp.path().toUtf8());

    const QString localFile = temp.filePath(QStringLiteral("本地视频.mp4"));
    QFile file(localFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("video");
    file.close();

    PlaybackController player;
    QTabWidget tabs;
    QListView *musicView = new QListView;
    QListView *videoView = new QListView;
    tabs.addTab(musicView, QStringLiteral("音乐"));
    tabs.addTab(videoView, QStringLiteral("视频"));

    PlaylistController controller(&player, PlaylistUi{musicView, videoView, &tabs});

    // 搜索结果的类型是视频，但用户把它加进了音乐页；下载完成仍应命中同源条目。
    MediaItem online;
    online.type = MediaType::Video;
    online.title = QStringLiteral("下载后改本地");
    online.url = QUrl(QStringLiteral("https://example.com/episode.m3u8"));
    online.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    online.online = true;
    online.sourceId = QStringLiteral("src-1");
    online.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/"));
    online.alternateUrls.append(QStringLiteral("https://cdn.example.com/episode.mp4"));

    controller.addSearchItem(online, MediaType::Music);
    QCOMPARE(controller.musicModel()->size(), 1);

    MediaItem downloaded = online;
    downloaded.type = MediaType::Video;
    QVERIFY(controller.updateDownloadedItem(downloaded, localFile));

    const MediaItem updated = controller.musicModel()->itemAt(0);
    QCOMPARE(updated.title, online.title);
    QCOMPARE(updated.type, MediaType::Music);
    QCOMPARE(updated.url, QUrl::fromLocalFile(localFile));
    QVERIFY(!updated.online);
    QVERIFY(updated.sourceId.isEmpty());
    QVERIFY(updated.webUrl.isEmpty());
    QVERIFY(updated.headers.isEmpty());
    QVERIFY(updated.alternateUrls.isEmpty());

    // 文件不存在时不能把列表改成坏路径
    QVERIFY(!controller.updateDownloadedItem(downloaded, temp.filePath(QStringLiteral("不存在.mp4"))));

    if (oldDataDir.isEmpty())
        qunsetenv("MVP_DATA_DIR");
    else
        qputenv("MVP_DATA_DIR", oldDataDir);
}

void UiLogicTest::settingsVolumeKeepsManualValueEvenWhenEqualToOldDefault()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    qputenv("MVP_DATA_DIR", temp.path().toUtf8());

    PlaybackController player;
    HttpClient http;
    SourceRegistry registry;
    QMainWindow window;
    QSlider slider;
    // 复刻真实窗口里 PlaybackUiController 的滑块联动；restore() 同步滑块时
    // 不能被误记成用户手动调整音量。
    QObject::connect(&slider, &QSlider::valueChanged, &player,
                     &PlaybackController::setVolumePercent);

    // QSettings 默认构造依赖应用名；测试进程没有设置时 setDefaultVolume 写不进去，
    // restore() 会读回内置默认 60。
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayerTestUi"));
    AppSettings::setDefaultVolume(40);
    SettingsController settings(&player, &registry, &http, SettingsUi{&window, &slider});
    settings.restore();
    QCOMPARE(player.volumePercent(), 40);

    // 用户手动把音量调回旧默认值 40：旧实现会误判为“没调过”，
    // 设置页改成 70 后被覆盖；这里必须保留用户当前音量。
    player.setVolumePercent(40);
    AppSettings::setDefaultVolume(70);
    settings.apply();
    QCOMPARE(player.volumePercent(), 40);

    // 重新恢复后没有手动调整，默认值仍应正常即时生效。
    settings.restore();
    AppSettings::setDefaultVolume(85);
    settings.apply();
    QCOMPARE(player.volumePercent(), 85);

    if (oldDataDir.isEmpty())
        qunsetenv("MVP_DATA_DIR");
    else
        qputenv("MVP_DATA_DIR", oldDataDir);
}

QTEST_MAIN(UiLogicTest)

#include "test_ui_logic.moc"
