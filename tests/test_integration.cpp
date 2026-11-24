/// @file test_integration.cpp
/// @brief 跨模块集成测试：本地 HTTP 服务、HLS 播放列表改写、流代理、下载、
///        缓存、歌词与搜索源的真实交互（不依赖外网，除非用例显式联网）。
#include "cache/HlsCacheBuilder.h"
#include "cache/LyricsCache.h"
#include "cache/MediaCache.h"
#include "core/PlaybackController.h"
#include "core/PlaylistModel.h"
#include "download/DownloadManager.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "lyrics/LyricsService.h"
#include "media/MediaItem.h"
#include "net/ConfigSearchProvider.h"
#include "net/ExternalSearchProvider.h"
#include "net/HttpClient.h"
#include "net/MaccmsWebProvider.h"
#include "net/NetLimits.h"
#include "net/SearchCoordinator.h"
#include "net/SourceConfig.h"
#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "storage/StoragePaths.h"
#include "stream/HlsPlaylist.h"
#include "stream/StreamProxy.h"
#include "util/HeaderPolicy.h"

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <functional>

namespace {

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 302:
        return "Found";
    case 404:
        return "Not Found";
    default:
        return "Error";
    }
}

/// 本地 HTTP 服务：记录请求头，允许每个用例决定响应内容和延迟。
/// 用真实 QTcpServer 而不是桩对象，才能验证 QNetworkAccessManager 实际发出/跟随的内容。
class LocalHttpServer : public QTcpServer
{
public:
    struct Request
    {
        QByteArray method;
        QByteArray path;
        QHash<QByteArray, QByteArray> headers;
    };

    QVector<Request> requests;
    int disconnects = 0;
    std::function<void(const Request &, QTcpSocket *)> handler;

    bool start()
    {
        if (!listen(QHostAddress::Any, 0))
            return false;
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    ++disconnects;
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray &buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const int end = buffer.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;

                    Request request;
                    const QByteArray head = buffer.left(end);
                    const QList<QByteArray> lines = head.split('\n');
                    if (!lines.isEmpty()) {
                        const QList<QByteArray> parts = lines.first().trimmed().split(' ');
                        if (parts.size() >= 2) {
                            request.method = parts.at(0);
                            request.path = parts.at(1);
                        }
                    }
                    for (int i = 1; i < lines.size(); ++i) {
                        const QByteArray line = lines.at(i).trimmed();
                        const int colon = line.indexOf(':');
                        if (colon <= 0)
                            continue;
                        request.headers.insert(line.left(colon).toLower(),
                                               line.mid(colon + 1).trimmed());
                    }

                    buffer.clear();
                    requests.append(request);
                    if (handler)
                        handler(request, socket);
                    else
                        respond(socket, 404, {}, "text/plain");
                });
            }
        });
        return true;
    }

    QUrl url(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(serverPort())
                        .arg(path));
    }

    QUrl urlAt(const QString &host, const QString &path) const
    {
        return QUrl(QStringLiteral("http://%1:%2%3")
                        .arg(host)
                        .arg(serverPort())
                        .arg(path));
    }

    static void respond(QTcpSocket *socket, int status, const QByteArray &body,
                        const QByteArray &contentType, qint64 declaredLength = -1,
                        const QByteArray &extraHeaders = {}, int delayMs = 0)
    {
        auto send = [socket, status, body, contentType, declaredLength, extraHeaders] {
            if (socket == nullptr || socket->state() == QAbstractSocket::UnconnectedState) {
                if (socket != nullptr)
                    socket->deleteLater();
                return;
            }
            const qint64 length = declaredLength >= 0 ? declaredLength : body.size();
            QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' '
                              + reasonPhrase(status) + "\r\n";
            head += "Content-Type: " + contentType + "\r\n";
            head += "Content-Length: " + QByteArray::number(length) + "\r\n";
            head += "Connection: close\r\n";
            head += extraHeaders;
            if (!head.endsWith("\r\n"))
                head += "\r\n";
            head += "\r\n";
            socket->write(head);
            socket->write(body);
            socket->disconnectFromHost();
        };

        if (delayMs > 0)
            QTimer::singleShot(delayMs, socket, send);
        else
            send();
    }

private:
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

SourceConfig makeSource(const QUrl &url, int timeoutMs = 0)
{
    SourceConfig config;
    config.id = QStringLiteral("local_test");
    config.name = QStringLiteral("本地测试源");
    config.kind = QStringLiteral("config");
    config.mediaType = MediaType::Music;
    config.urlTemplate = url.toString();
    config.itemsPath = QStringLiteral("items");
    config.fields.insert(QStringLiteral("title"), QStringLiteral("title"));
    config.fields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));
    config.timeoutMs = timeoutMs;
    return config;
}

QByteArray validSearchJson()
{
    return QByteArrayLiteral(
        "{\"items\":[{\"title\":\"本地歌曲\",\"url\":\"http://example.com/a.mp3\"}]}");
}

QList<QByteArray> g_logMessages;

void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_logMessages.append(message.toUtf8());
}

} // namespace

class IntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void redirectStripsSecretsAcrossHost();
    void getRedirectStripsSecretsAcrossHost();
    void explicitRefererQueryIsStripped();
    void redirectKeepsAuthorizationOnSameHost();
    void downloadReportsHttp404WithoutFile();
    void downloadRejectsHtmlMasqueradingAsMedia();
    void downloadAcceptsMediaContentTypeParameters();
    void downloadFailsOnShortContentLength();
    void downloadStopsAtInjectedByteLimit();
    void downloadRejectsShortHlsSegmentRange();
    void downloadResumesFromPartialFileWithRange();
    void downloadCancelLeavesExistingFinalFile();
    void downloadManagerKeepsExistingFinalFile();
    void downloadManagerSameNameGetsDistinctFiles();
    void downloadManagerImportsCompleteCache();
    void downloadManagerCleansPreexistingCacheWithoutSeed();
    void downloadManagerStagesMusicAndReusesPlaybackCache();
    void downloadManagerPausesAndResumesMusic();
    void downloadManagerPublishesAcrossVolumes();
    void downloadManagerRemovesLegacyDownloadCache();
    void downloadManagerMigratesLegacyStaging();
    void downloadHlsMergesAndRemuxes();
    void hlsCacheBuilderProducesSuffixlessMp4();
    void hlsCacheBuilderResumesAfterExit();
    void downloadManagerReusesPlaybackCacheThroughStaging();
    void hlsCacheBuilderMergesSingleFileAudio();
    void hlsCacheBuilderFollowsRealAppleStream();
    void lyricsServiceCachesOnlinePayload();
    void mediaRemuxConvertsTsToMp4();
    void downloadTaskAllowsVodWithoutEndList();
    void downloadTaskRequiresConsentWithoutVod();
    void downloadManagerForcesSnapshotOnlyAfterConsent();
    void streamProxyPreservesEncodedSegmentQuery();
    void streamProxyServesFirstSegmentOfLongPlaylist();
    void streamProxySelectsBitrateVariant();
    void playbackControllerSelectsHlsVariant();
    void streamProxyReleaseCacheFileClosesEntry();
    void searchTimeoutAbortsProvider();
    void maccmsAbortClosesInflightRequest();
    void externalAbortKillsProcessWithoutFinish();
    void externalProviderScopesCookieToConfiguredOrigin();
    void externalProviderHonorsLimit();
    void searchJsonOverLimitFails();
    void responseLimitAbortsInflight();
    void responseLimitOnHttpErrorKeepsStatus();
    void requestLogsRedactQuerySecrets();
    void downloadLimitSettingClampsAndPersists();
    void videoPositionRestoredAfterSwitch();

private:
    QTemporaryDir m_tempDir;
};

void IntegrationTest::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayerIntegrationTest"));
    QStandardPaths::setTestModeEnabled(true);
    qputenv("MVP_DATA_DIR", m_tempDir.path().toUtf8());
    QVERIFY(QDir(StoragePaths::dataDir()).exists()
            || QDir().mkpath(StoragePaths::dataDir()));
    QLoggingCategory::setFilterRules(QStringLiteral("mvp.*=true"));
}

void IntegrationTest::init()
{
    QVERIFY(QDir(StoragePaths::dataDir()).removeRecursively());
    QVERIFY(QDir().mkpath(StoragePaths::dataDir()));
}

void IntegrationTest::cleanupTestCase()
{
    QStandardPaths::setTestModeEnabled(false);
    qunsetenv("MVP_DATA_DIR");
}

void IntegrationTest::redirectStripsSecretsAcrossHost()
{
    LocalHttpServer first;
    LocalHttpServer second;
    QVERIFY(first.start());
    QVERIFY(second.start());

    const QString target = second.urlAt(QStringLiteral("127.0.0.2"),
                                        QStringLiteral("/final")).toString();
    first.handler = [target](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 302, {}, "text/plain", 0,
                                 "Location: " + target.toUtf8() + "\r\n");
    };
    second.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, "ok", "text/plain");
    };

    HttpClient http;
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), first.url(QStringLiteral("/")).toString());
    headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret-cookie"));
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret-token"));
    headers.insert(QStringLiteral("X-Source-Session"), QStringLiteral("session-keep"));

    bool done = false;
    http.fetch(first.url(QStringLiteral("/start")), {}, headers,
               [&done](QNetworkReply *reply) {
                   if (reply != nullptr)
                       reply->deleteLater();
                   done = true;
               });

    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(second.requests.size(), 1);
    const auto &request = second.requests.first();
    QVERIFY(!request.headers.contains("cookie"));
    QVERIFY(!request.headers.contains("authorization"));
    QVERIFY(!request.headers.contains("x-source-session"));
}

void IntegrationTest::getRedirectStripsSecretsAcrossHost()
{
    LocalHttpServer first;
    LocalHttpServer second;
    QVERIFY(first.start());
    QVERIFY(second.start());

    const QString target = second.urlAt(QStringLiteral("127.0.0.2"),
                                        QStringLiteral("/final")).toString();
    first.handler = [target](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 302, {}, "text/plain", 0,
                                 "Location: " + target.toUtf8() + "\r\n");
    };
    second.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, "ok", "text/plain");
    };

    HttpClient http;
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), first.url(QStringLiteral("/")).toString());
    headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret-cookie"));
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret-token"));
    headers.insert(QStringLiteral("X-Api-Key"), QStringLiteral("sk-secret"));

    QNetworkReply *reply = http.get(first.url(QStringLiteral("/start")), {}, headers);
    QVERIFY(reply != nullptr);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QVERIFY(finished.wait(5000));
    QCOMPARE(reply->error(), QNetworkReply::InsecureRedirectError);
    reply->deleteLater();

    // get() 使用 SameOriginRedirectPolicy：跨 host 302 直接拒绝，
    // 不把 Cookie/Authorization 交给第三方；同 host 跳转仍可跟随。
    QCOMPARE(second.requests.size(), 0);
}

void IntegrationTest::explicitRefererQueryIsStripped()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, "ok", "text/plain");
    };

    HttpClient http;
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"),
                   server.url(QStringLiteral("/search?api_key=secret&q=1")).toString());
    QNetworkReply *reply = http.get(server.url(QStringLiteral("/media")), {}, headers);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QVERIFY(finished.wait(5000));
    QCOMPARE(reply->error(), QNetworkReply::NoError);
    reply->deleteLater();

    QCOMPARE(server.requests.size(), 1);
    const QByteArray referer = server.requests.first().headers.value("referer");
    QVERIFY(!referer.contains("api_key"));
    QVERIFY(!referer.contains("secret"));
    QVERIFY(referer.contains("/search"));
}

void IntegrationTest::redirectKeepsAuthorizationOnSameHost()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QString target = server.url(QStringLiteral("/final")).toString();
    server.handler = [target](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path.startsWith("/final")) {
            LocalHttpServer::respond(socket, 200, "ok", "text/plain");
            return;
        }
        LocalHttpServer::respond(socket, 302, {}, "text/plain", 0,
                                 "Location: " + target.toUtf8() + "\r\n");
    };

    HttpClient http;
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), server.url(QStringLiteral("/")).toString());
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer same-host-token"));

    bool done = false;
    http.fetch(server.url(QStringLiteral("/start")), {}, headers,
               [&done](QNetworkReply *reply) {
                   if (reply != nullptr)
                       reply->deleteLater();
                   done = true;
               });

    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(server.requests.last().headers.value("authorization"),
             QByteArray("Bearer same-host-token"));
}

void IntegrationTest::downloadReportsHttp404WithoutFile()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 404, "missing", "text/plain");
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("missing.mp3"));
    QFile::remove(path);
    DownloadTask *task = DownloadTask::start(&http, server.url(QStringLiteral("/a.mp3")), path);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("404")));
    QVERIFY(!QFile::exists(path));
}

void IntegrationTest::downloadRejectsHtmlMasqueradingAsMedia()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, "<html>not media</html>", "text/html");
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("fake.mp3"));
    QFile::remove(path);
    DownloadTask *task = DownloadTask::start(&http, server.url(QStringLiteral("/fake.mp3")), path);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!QFile::exists(path));
}

void IntegrationTest::downloadAcceptsMediaContentTypeParameters()
{
    // application/mp4; codecs=... 是合法 MP4 响应；旧实现按整串精确比较，
    // 会把带参数的容器类型当成网页数据直接拒绝。
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray payload("mp4-payload");
    server.handler = [payload](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, payload,
                                 "application/mp4; codecs=avc1.42E01E", payload.size());
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("codec-params.mp4"));
    QFile::remove(path);
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/codec-params.mp4")), path);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(succeeded.count(), 1);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payload);
}

void IntegrationTest::downloadFailsOnShortContentLength()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        // 声明 200 字节，只给 100 字节就断开，模拟 CDN 截断。
        LocalHttpServer::respond(socket, 200, QByteArray(100, 'x'), "audio/mpeg", 200);
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("truncated.mp3"));
    QFile::remove(path);
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/truncated.mp3")), path);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!QFile::exists(path));
}

void IntegrationTest::downloadStopsAtInjectedByteLimit()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, QByteArray(4096, 'x'), "audio/mpeg");
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("too-big.mp3"));
    QFile::remove(path);
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/big.mp3")), path);
    task->setMaxBytes(1024);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("上限")));
    QVERIFY(!QFile::exists(path));
}

void IntegrationTest::downloadRejectsShortHlsSegmentRange()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "#EXT-X-BYTERANGE:100@0\n"
        "seg.ts\n"
        "#EXT-X-ENDLIST\n";

    server.handler = [playlist](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path.startsWith("/hls/list.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
            return;
        }
        // 期望 100 字节，实际只给 50 字节且 Range 声明完整，触发分片长度校验。
        LocalHttpServer::respond(socket, 206, QByteArray(50, 's'), "video/mp2t", 50,
                                 "Content-Range: bytes 0-99/200\r\n");
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("short-list.m3u8"));
    QFile::remove(path);
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/hls/list.m3u8")), path);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 6000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("分片长度不完整")));
    QVERIFY(!QFile::exists(m_tempDir.filePath(QStringLiteral("short-list.ts"))));
}

void IntegrationTest::downloadResumesFromPartialFileWithRange()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray full(1000, 'x');
    server.handler = [full](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        const QByteArray range = request.headers.value("range");
        if (range == "bytes=300-") {
            LocalHttpServer::respond(socket, 206, full.mid(300), "audio/mpeg", 700,
                                     "Content-Range: bytes 300-999/1000\r\n");
            return;
        }
        LocalHttpServer::respond(socket, 200, full, "audio/mpeg", full.size());
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("resume.mp3"));
    const QString partial = path + QStringLiteral(".mvpdownload");
    QFile::remove(path);
    QFile::remove(partial);
    {
        QFile file(partial);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(full.left(300)) == 300);
    }

    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/resume.mp3")), path);
    task->setResumeState(300, 0, false);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(succeeded.count(), 1);

    QVERIFY(!QFile::exists(partial));
    QFile result(path);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.size(), qint64(full.size()));
    QCOMPARE(result.readAll(), full);

    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().headers.value("range"), QByteArray("bytes=300-"));
}

void IntegrationTest::downloadCancelLeavesExistingFinalFile()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *) {
        // 故意不响应：让任务停在请求在途状态，随后由 cancel() 中止。
    };

    const QString path = m_tempDir.filePath(QStringLiteral("cancel-existing.mp3"));
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".mvpdownload"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("existing-final") == 14);
    }

    HttpClient http;
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/stall.mp3")), path);
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() > 0, 5000);
    task->cancel();
    QTest::qWait(100);

    // 取消只应删除本任务的 .mvpdownload，不能删除已有最终文件。
    QVERIFY(QFileInfo::exists(path));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("existing-final"));
    QVERIFY(!QFile::exists(path + QStringLiteral(".mvpdownload")));
}

void IntegrationTest::downloadManagerKeepsExistingFinalFile()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("dl-keep"));
    QVERIFY(QDir(dir).removeRecursively() || !QFileInfo::exists(dir));
    QVERIFY(QDir().mkpath(dir));
    const QString base = QDir(dir).filePath(QStringLiteral("同名.mp3"));
    {
        QFile file(base);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("user-file") == 9);
    }

    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        const QByteArray body("download-body");
        LocalHttpServer::respond(socket, 200, body, "audio/mpeg", body.size());
    };

    HttpClient http;
    DownloadManager manager(&http);
    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("同名");
    item.url = server.url(QStringLiteral("/same.mp3"));
    item.online = true;

    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });
    const QString id =
        manager.enqueue(item, item.url, {}, base, false, NetLimits::kDownloadBytes);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);

    const QString saved = completed.first();
    QCOMPARE(saved, QDir(dir).filePath(QStringLiteral("同名 (1).mp3")));
    QVERIFY(saved != base);
    QFile original(base);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), QByteArray("user-file"));
    QVERIFY(QFileInfo(saved).size() > 0);
    manager.shutdownForExit();
}

void IntegrationTest::downloadManagerSameNameGetsDistinctFiles()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("dl-concurrent"));
    QVERIFY(QDir(dir).removeRecursively() || !QFileInfo::exists(dir));
    QVERIFY(QDir().mkpath(dir));
    const QString base = QDir(dir).filePath(QStringLiteral("同名.mp3"));

    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        const QByteArray body = request.path == "/a.mp3" ? "body-a" : "body-b";
        LocalHttpServer::respond(socket, 200, body, "audio/mpeg", body.size());
    };

    HttpClient http;
    DownloadManager manager(&http);
    MediaItem first;
    first.type = MediaType::Music;
    first.title = QStringLiteral("同名");
    first.url = server.url(QStringLiteral("/a.mp3"));
    first.online = true;
    MediaItem second = first;
    second.url = server.url(QStringLiteral("/b.mp3"));

    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });
    QVERIFY(!manager.enqueue(first, first.url, {}, base, false, NetLimits::kDownloadBytes).isEmpty());
    QVERIFY(!manager.enqueue(second, second.url, {}, base, false, NetLimits::kDownloadBytes).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 10000);

    QVERIFY(completed.at(0) != completed.at(1));
    QVERIFY(completed.contains(base));
    QVERIFY(completed.contains(QDir(dir).filePath(QStringLiteral("同名 (1).mp3"))));
    QFile a(base);
    QFile b(QDir(dir).filePath(QStringLiteral("同名 (1).mp3")));
    QVERIFY(a.open(QIODevice::ReadOnly));
    QVERIFY(b.open(QIODevice::ReadOnly));
    QCOMPARE(a.readAll(), QByteArray("body-a"));
    QCOMPARE(b.readAll(), QByteArray("body-b"));
    manager.shutdownForExit();
}

void IntegrationTest::downloadManagerImportsCompleteCache()
{
    const QString dir = m_tempDir.filePath(QStringLiteral("dl-cache-import"));
    QVERIFY(QDir(dir).removeRecursively() || !QFileInfo::exists(dir));
    QVERIFY(QDir().mkpath(dir));

    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("缓存导入");
    item.url = QStringLiteral("https://example.com/import.mp3");
    item.lyricUrl = QStringLiteral("https://example.com/import.lrc");
    item.online = true;

    // 用真实缓存布局，确保“下载完成后删除该条播放缓存”走的是精确路径删除。
    const QString cacheBody = MediaCache::filePathForItem(item);
    QVERIFY(!cacheBody.isEmpty());
    QVERIFY(QDir().mkpath(QFileInfo(cacheBody).absolutePath()));
    const QByteArray expected("cached-media-body");
    {
        QFile file(cacheBody);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(expected), qint64(expected.size()));
    }
    QVERIFY(MediaCache::writeState(cacheBody, MediaCache::cacheIdentity(item), item.url,
                                   "audio/mpeg", expected.size(), true, ".mp3"));

    const QByteArray lyricPayload("lyrics-cache-must-survive");
    QVERIFY(LyricsCache::store(item, lyricPayload));

    // 下载目标必须有用户能识别的后缀；缓存正文本身可以无后缀。
    const QString target = QDir(dir).filePath(QStringLiteral("缓存导入.mp3"));

    AppSettings::setRemovePlaybackCacheAfterDownload(true);

    HttpClient http;
    DownloadManager manager(&http);

    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });
    const QString id = manager.importFromCache(item, cacheBody, target);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
    QCOMPARE(completed.first(), target);
    QVERIFY(!QFileInfo::exists(target + QStringLiteral(".mvpdownload")));

    QFile saved(target);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), expected);

    // 默认设置下，播放缓存正文、旁车 JSON 和歌词缓存都必须删掉；
    // 下载文件本身保留。
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(cacheBody), 3000);
    QVERIFY(!QFileInfo::exists(cacheBody + QStringLiteral(".json")));
    QByteArray cachedLyrics;
    QVERIFY(!LyricsCache::load(item, &cachedLyrics));

    for (const DownloadJob &job : manager.jobs()) {
        if (job.id == id) {
            QCOMPARE(job.status, DownloadJob::Status::Completed);
            QVERIFY(job.cleanupCachePath.isEmpty());
        }
    }
    manager.shutdownForExit();
}

void IntegrationTest::downloadManagerCleansPreexistingCacheWithoutSeed()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray full(1000, 'c');
    server.handler = [full](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, full, "audio/mpeg", full.size());
    };

    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("独立缓存清理");
    item.url = server.url(QStringLiteral("/cleanup.mp3"));
    item.lyricUrl = server.url(QStringLiteral("/cleanup.lrc"));
    item.online = true;

    // 预置一条完整播放缓存和歌词缓存，但下载时不传 CacheSeed，
    // 模拟“缓存存在但本次下载没有复用它”的普通网络下载。
    const QString cacheBody = MediaCache::filePathForItem(item);
    QVERIFY(!cacheBody.isEmpty());
    QVERIFY(QDir().mkpath(QFileInfo(cacheBody).absolutePath()));
    {
        QFile cache(cacheBody);
        QVERIFY(cache.open(QIODevice::WriteOnly));
        QCOMPARE(cache.write(full), qint64(full.size()));
    }
    QVERIFY(MediaCache::writeState(cacheBody, MediaCache::cacheIdentity(item), item.url,
                                   "audio/mpeg", full.size(), true, ".mp3"));
    QVERIFY(LyricsCache::store(item, QByteArrayLiteral("[00:01.00]cleaned")));

    const QString dir = m_tempDir.filePath(QStringLiteral("dl-cache-sweep"));
    // 这里必须删除测试自己的临时子目录；QDir() 默认是当前构建目录，
    // 早期误写会在测试时把整个 build 目录递归删掉。
    QVERIFY(QDir(dir).removeRecursively() || !QFileInfo::exists(dir));
    QVERIFY(QDir().mkpath(dir));
    const QString target = QDir(dir).filePath(QStringLiteral("独立缓存清理.mp3"));

    AppSettings::setRemovePlaybackCacheAfterDownload(true);
    HttpClient http;
    DownloadManager manager(&http);
    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });
    const QString id = manager.enqueue(item, item.url, {}, target, false,
                                       NetLimits::kDownloadBytes);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 6000);
    QCOMPARE(completed.first(), target);

    // 即使没有 cacheCopySourcePath / seedCacheBodyPath，也要按条目身份
    // 清掉预置的播放缓存与歌词缓存；下载文件保留。
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(cacheBody), 3000);
    QVERIFY(!QFileInfo::exists(cacheBody + QStringLiteral(".json")));
    QByteArray lyrics;
    QVERIFY(!LyricsCache::load(item, &lyrics));
    QVERIFY(QFileInfo::exists(target));

    for (const DownloadJob &job : manager.jobs()) {
        if (job.id == id) {
            QCOMPARE(job.status, DownloadJob::Status::Completed);
            QVERIFY(job.cleanupCachePath.isEmpty());
            QVERIFY(!job.cleanupMediaAndLyrics);
        }
    }
    manager.shutdownForExit();
}

void IntegrationTest::downloadManagerStagesMusicAndReusesPlaybackCache()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray full(1000, 'm');
    server.handler = [full](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        const QByteArray range = request.headers.value("range");
        if (range == "bytes=300-") {
            LocalHttpServer::respond(socket, 206, full.mid(300), "audio/mpeg", 700,
                                     "Content-Range: bytes 300-999/1000\r\n");
            return;
        }
        LocalHttpServer::respond(socket, 200, full, "audio/mpeg", full.size());
    };

    const QString dir = m_tempDir.filePath(QStringLiteral("music-staging"));
    QVERIFY(QDir().mkpath(dir));
    const QString target = QDir(dir).filePath(QStringLiteral("音乐暂存.mp3"));

    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("音乐暂存");
    item.url = server.url(QStringLiteral("/music.mp3"));
    item.online = true;

    // 模拟播放到 30% 后退出：缓存正文只有 300 字节，旁车元数据记录总长 1000。
    const QString cacheBody = MediaCache::filePathForItem(item);
    QVERIFY(!cacheBody.isEmpty());
    QVERIFY(QDir().mkpath(QFileInfo(cacheBody).absolutePath()));
    {
        QFile cache(cacheBody);
        QVERIFY(cache.open(QIODevice::WriteOnly));
        QCOMPARE(cache.write(full.left(300)), qint64(300));
    }
    QVERIFY(MediaCache::writeState(cacheBody, MediaCache::cacheIdentity(item), item.url,
                                   "audio/mpeg", full.size(), false, QStringLiteral(".mp3")));

    HttpClient http;
    DownloadManager manager(&http);
    DownloadManager::CacheSeed seed;
    seed.sourcePath = cacheBody;
    seed.bytes = 300;
    seed.cacheBodyPath = cacheBody;

    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });
    const QString id = manager.enqueue(item, item.url, {}, target, false,
                                       NetLimits::kDownloadBytes, nullptr, nullptr, &seed);
    QVERIFY(!id.isEmpty());

    const DownloadJob *job = nullptr;
    for (const DownloadJob &entry : manager.jobs()) {
        if (entry.id == id)
            job = &entry;
    }
    QVERIFY(job != nullptr);
    // 暂存区必须在应用数据目录，下载目录里不能再出现 .download-cache。
    const QString stagingPath = job->stagingPath;
    QVERIFY(stagingPath.startsWith(StoragePaths::dataFile(QStringLiteral("download-cache"))));
    QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral(".download-cache"))));

    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
    QCOMPARE(completed.first(), target);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().headers.value("range"), QByteArray("bytes=300-"));

    QFile saved(target);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), full);
    QVERIFY(!QFileInfo::exists(stagingPath));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(cacheBody), 3000);

    manager.shutdownForExit();
    server.close();
}

void IntegrationTest::downloadManagerPausesAndResumesMusic()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray full(1000, 'p');
    server.handler = [full](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        const QByteArray range = request.headers.value("range");
        if (range == "bytes=200-") {
            LocalHttpServer::respond(socket, 206, full.mid(200), "audio/mpeg", 800,
                                     "Content-Range: bytes 200-999/1000\r\n");
            return;
        }
        // 先只发 200 字节并保持连接，给测试留出暂停窗口。
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n"
                      "Content-Length: 1000\r\n\r\n");
        socket->write(full.left(200));
        socket->flush();
        QTimer::singleShot(500, socket, [socket, full] {
            if (socket->state() == QAbstractSocket::ConnectedState) {
                socket->write(full.mid(200));
                socket->disconnectFromHost();
            }
        });
    };

    const QString dir = m_tempDir.filePath(QStringLiteral("music-pause"));
    QVERIFY(QDir().mkpath(dir));
    const QString target = QDir(dir).filePath(QStringLiteral("暂停恢复.mp3"));

    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("暂停恢复");
    item.url = server.url(QStringLiteral("/pause.mp3"));
    item.online = true;

    HttpClient http;
    DownloadManager manager(&http);
    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });

    const QString id = manager.enqueue(item, item.url, {}, target, false,
                                       NetLimits::kDownloadBytes);
    QVERIFY(!id.isEmpty());

    // 等首块数据落盘后暂停；暂停必须保留半成品而不是删除。
    QTRY_VERIFY_WITH_TIMEOUT(manager.jobs().at(0).bytesReceived > 0, 5000);
    QVERIFY(manager.pauseJob(id));
    QCOMPARE(manager.jobs().at(0).status, DownloadJob::Status::Paused);
    QVERIFY(manager.jobs().at(0).pausedByUser);
    const QString partial = manager.jobs().at(0).partialPath();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(partial), 1000);
    QCOMPARE(completed.size(), 0);

    // 暂停期间不能有后台任务继续写盘或偷偷完成。
    QTest::qWait(600);
    QCOMPARE(manager.jobs().at(0).status, DownloadJob::Status::Paused);
    QCOMPARE(completed.size(), 0);

    QVERIFY(manager.resumeJob(id));
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
    QCOMPARE(completed.first(), target);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(server.requests.at(1).headers.value("range"), QByteArray("bytes=200-"));

    QFile saved(target);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.size(), qint64(full.size()));
    QCOMPARE(saved.readAll(), full);

    manager.shutdownForExit();
    server.close();
}

void IntegrationTest::downloadManagerPublishesAcrossVolumes()
{
    // 真实机器上应用数据在程序盘（F:），默认下载目录在用户盘（C:）。
    // 暂存完成后必须走分块复制，而不是同盘 rename；这里用测试数据盘
    // 与程序目录盘构造跨盘场景。
    const QString dataDir = StoragePaths::dataDir();
    const QString targetDir =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mvp-cross-volume"));
    QDir(targetDir).removeRecursively();
    QVERIFY(QDir().mkpath(targetDir));
    const QString target = QDir(targetDir).filePath(QStringLiteral("跨盘发布.mp3"));

    const QFileInfo dataInfo(dataDir);
    const QFileInfo targetInfo(targetDir);
    if (dataInfo.absolutePath().left(2).compare(targetInfo.absolutePath().left(2),
                                               Qt::CaseInsensitive) == 0) {
        QDir(targetDir).removeRecursively();
        QSKIP("测试数据目录与目标目录同盘，无法覆盖跨盘发布路径");
    }

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray full(4096, 'v');
    server.handler = [full](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        const QByteArray range = request.headers.value("range");
        if (range.startsWith("bytes=")) {
            bool ok = false;
            const qint64 start = range.mid(6).split('-').value(0).toLongLong(&ok);
            const int from = ok ? int(start) : 0;
            const QByteArray contentRange =
                "Content-Range: bytes " + QByteArray::number(from) + "-"
                + QByteArray::number(full.size() - 1) + "/"
                + QByteArray::number(full.size()) + "\r\n";
            LocalHttpServer::respond(socket, 206, full.mid(from), "audio/mpeg",
                                     full.size() - from, contentRange);
            return;
        }
        LocalHttpServer::respond(socket, 200, full, "audio/mpeg");
    };

    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("跨盘发布");
    item.url = server.url(QStringLiteral("/cross.mp3"));
    item.online = true;

    HttpClient http;
    DownloadManager manager(&http);
    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted,
            [&completed](const MediaItem &, const QString &path) { completed.append(path); });

    const QString id = manager.enqueue(item, item.url, {}, target, false,
                                       NetLimits::kDownloadBytes);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 8000);
    QCOMPARE(completed.first(), target);

    QFile saved(target);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.size(), qint64(full.size()));
    QCOMPARE(saved.readAll(), full);
    saved.close();

    // 发布会先写 `.mvpdownload` 再改名；收尾后不能留下任何临时文件。
    QCOMPARE(QDir(targetDir).entryList(QStringList() << QStringLiteral("*.mvpdownload"),
                                      QDir::Files).size(), 0);
    QCOMPARE(manager.jobs().at(0).status, DownloadJob::Status::Completed);

    manager.shutdownForExit();
    server.close();
    QDir(targetDir).removeRecursively();
}

void IntegrationTest::downloadManagerRemovesLegacyDownloadCache()
{
    const QString root = m_tempDir.filePath(QStringLiteral("legacy-root"));
    const QString musicDir = QDir(root).filePath(QStringLiteral("音乐"));
    const QString userFile = QDir(musicDir).filePath(QStringLiteral("保留的歌曲.mp3"));
    const QString legacyDir = QDir(musicDir).filePath(
        QStringLiteral(".download-cache/old-job"));
    const QString legacyPiece = QDir(legacyDir).filePath(QStringLiteral("piece.ts"));

    QVERIFY(QDir().mkpath(legacyDir));
    QFile keep(userFile);
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.write("keep");
    keep.close();
    QFile piece(legacyPiece);
    QVERIFY(piece.open(QIODevice::WriteOnly));
    piece.write("cache");
    piece.close();

    AppSettings::setDownloadDir(root);
    HttpClient http;
    DownloadManager manager(&http);

    // 启动后的延迟清理只删应用自己的 .download-cache，用户文件必须保留。
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(legacyDir), 3000);
    QVERIFY(QFileInfo::exists(userFile));
    QCOMPARE(QFileInfo(userFile).size(), qint64(4));

    manager.shutdownForExit();
    AppSettings::setDownloadDir(QString());
}

void IntegrationTest::downloadManagerMigratesLegacyStaging()
{
    const QString root = m_tempDir.filePath(QStringLiteral("legacy-migrate"));
    const QString videoDir = QDir(root).filePath(QStringLiteral("影视"));
    const QString legacyRoot = QDir(videoDir).filePath(QStringLiteral(".download-cache"));
    const QString migratedDir = QDir(legacyRoot).filePath(QStringLiteral("legacy-migrated"));
    const QString missingDir = QDir(legacyRoot).filePath(QStringLiteral("legacy-missing"));
    QVERIFY(QDir().mkpath(migratedDir));
    QVERIFY(QDir().mkpath(missingDir));

    const QByteArray partial(500, 'h');
    DownloadJob migrated;
    migrated.id = QStringLiteral("legacy-migrated");
    migrated.item.type = MediaType::Video;
    migrated.item.title = QStringLiteral("生化危机2002");
    migrated.item.url = QUrl(QStringLiteral("http://127.0.0.1:1/legacy.m3u8"));
    migrated.item.online = true;
    migrated.url = migrated.item.url;
    migrated.filePath = QDir(videoDir).filePath(QStringLiteral("生化危机2002.mp4"));
    migrated.stagingPath = QDir(migratedDir).filePath(QStringLiteral("生化危机2002.mp4.ts"));
    migrated.hls = true;
    migrated.status = DownloadJob::Status::Paused;
    migrated.pausedByUser = true;
    migrated.bytesReceived = partial.size();
    migrated.segmentIndex = 59;
    migrated.segmentCount = 649;
    migrated.message = QStringLiteral("正在下载 59/649 分片");
    QFile oldPartial(migrated.stagingPath + QStringLiteral(".mvpdownload"));
    QVERIFY(oldPartial.open(QIODevice::WriteOnly));
    oldPartial.write(partial);
    oldPartial.close();

    DownloadJob missing;
    missing.id = QStringLiteral("legacy-missing");
    missing.item = migrated.item;
    missing.item.title = QStringLiteral("缓存已删除");
    missing.url = migrated.url;
    missing.filePath = QDir(videoDir).filePath(QStringLiteral("缓存已删除.mp4"));
    missing.stagingPath = QDir(missingDir).filePath(QStringLiteral("缓存已删除.mp4.ts"));
    missing.hls = true;
    missing.status = DownloadJob::Status::Paused;
    missing.pausedByUser = true;
    missing.bytesReceived = 13 * 1024 * 1024;
    missing.segmentIndex = 59;
    missing.segmentCount = 649;
    missing.message = QStringLiteral("正在下载 59/649 分片");

    AppSettings::setDownloadDir(root);
    QDir().mkpath(StoragePaths::dataDir());
    QFile state(StoragePaths::dataFile(QStringLiteral("downloads.json")));
    QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QJsonArray jobs;
    jobs.append(migrated.toJson());
    jobs.append(missing.toJson());
    QJsonObject rootObject;
    rootObject.insert(QStringLiteral("version"), 1);
    rootObject.insert(QStringLiteral("jobs"), jobs);
    state.write(QJsonDocument(rootObject).toJson(QJsonDocument::Indented));
    state.close();

    HttpClient http;
    DownloadManager manager(&http);
    QCOMPARE(manager.jobs().size(), 2);

    const DownloadJob *migratedJob = nullptr;
    const DownloadJob *missingJob = nullptr;
    for (const DownloadJob &job : manager.jobs()) {
        if (job.id == migrated.id)
            migratedJob = &job;
        else if (job.id == missing.id)
            missingJob = &job;
    }
    QVERIFY(migratedJob != nullptr);
    QVERIFY(missingJob != nullptr);

    // 旧缓存搬到应用数据目录，分片进度保留；下载目录不再留下旧暂存目录。
    QVERIFY(migratedJob->stagingPath.contains(QStringLiteral("download-cache")));
    QVERIFY(!migratedJob->stagingPath.contains(QStringLiteral("/.download-cache/")));
    QVERIFY(QFileInfo(migratedJob->partialPath()).exists());
    QCOMPARE(QFileInfo(migratedJob->partialPath()).size(), qint64(partial.size()));
    QCOMPARE(migratedJob->segmentIndex, 59);
    QCOMPARE(migratedJob->segmentCount, 649);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(legacyRoot), 3000);

    // 旧缓存被用户手动删掉时，不能继续显示 59/649；必须清零并提示重新下载。
    QCOMPARE(missingJob->segmentIndex, 0);
    QCOMPARE(missingJob->segmentCount, 0);
    QCOMPARE(missingJob->bytesReceived, qint64(0));
    QVERIFY(missingJob->message.contains(QStringLiteral("缓存")));

    manager.shutdownForExit();
    AppSettings::setDownloadDir(QString());
}

void IntegrationTest::downloadHlsMergesAndRemuxes()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过 HLS 合并转封装测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString sourceTs = m_tempDir.filePath(QStringLiteral("hls-source.ts"));
    QFile::remove(sourceTs);
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                             QStringLiteral("-t"), QStringLiteral("2"),
                             QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                             QStringLiteral("-c:a"), QStringLiteral("mp2"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), sourceTs});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    QFile tsFile(sourceTs);
    QVERIFY(tsFile.open(QIODevice::ReadOnly));
    const QByteArray tsBytes = tsFile.readAll();
    QVERIFY(tsBytes.size() > 1024);
    const int split = tsBytes.size() / 2;

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "seg0.ts\n"
        "#EXTINF:1.0,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist, tsBytes, split](const LocalHttpServer::Request &request,
                                                QTcpSocket *socket) {
        if (request.path == "/hls/index.m3u8") {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == "/hls/seg0.ts") {
            LocalHttpServer::respond(socket, 200, tsBytes.left(split), "video/mp2t");
        } else if (request.path == "/hls/seg1.ts") {
            LocalHttpServer::respond(socket, 200, tsBytes.mid(split), "video/mp2t");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    HttpClient http;
    const QString mergedTs = m_tempDir.filePath(QStringLiteral("hls-merged.ts"));
    const QString mp4Path = m_tempDir.filePath(QStringLiteral("hls-merged.mp4"));
    QFile::remove(mergedTs);
    QFile::remove(mp4Path);
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/hls/index.m3u8")), mergedTs);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 10000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(succeeded.count(), 1);
    QVERIFY(task->isHls());
    QCOMPARE(server.requests.size(), 3);
    QCOMPARE(QFileInfo(mergedTs).size(), tsBytes.size());

    MediaRemux remux;
    QSignalSpy finished(&remux, &MediaRemux::finished);
    remux.remuxToMp4(mergedTs, mp4Path);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 30000);
    QCOMPARE(finished.first().at(0).toBool(), true);
    QVERIFY(QFileInfo(mp4Path).size() > 0);
}

void IntegrationTest::hlsCacheBuilderProducesSuffixlessMp4()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过 HLS 后台缓存测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString sourceTs = m_tempDir.filePath(QStringLiteral("hls-cache-source.ts"));
    QFile::remove(sourceTs);
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                             QStringLiteral("-t"), QStringLiteral("2"),
                             QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                             QStringLiteral("-c:a"), QStringLiteral("mp2"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), sourceTs});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    QFile tsFile(sourceTs);
    QVERIFY(tsFile.open(QIODevice::ReadOnly));
    const QByteArray tsBytes = tsFile.readAll();
    QVERIFY(tsBytes.size() > 1024);
    const int split = tsBytes.size() / 2;

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "seg0.ts\n"
        "#EXTINF:1.0,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist, tsBytes, split](const LocalHttpServer::Request &request,
                                                QTcpSocket *socket) {
        if (request.path == QStringLiteral("/cache/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/cache/seg0.ts")) {
            LocalHttpServer::respond(socket, 200, tsBytes.left(split), "video/mp2t");
        } else if (request.path == QStringLiteral("/cache/seg1.ts")) {
            LocalHttpServer::respond(socket, 200, tsBytes.mid(split), "video/mp2t");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("HLS 缓存测试");
    item.url = server.url(QStringLiteral("/cache/index.m3u8"));
    item.online = true;

    HttpClient http;
    HlsCacheBuilder builder(&http);
    bool completed = false;
    bool failed = false;
    QString completedPath;
    QString failureMessage;
    connect(&builder, &HlsCacheBuilder::completed, &builder,
            [&](const MediaItem &, const QString &path) {
                completed = true;
                completedPath = path;
            });
    connect(&builder, &HlsCacheBuilder::failed, &builder,
            [&](const MediaItem &, const QString &message) {
                failed = true;
                failureMessage = message;
            });

    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 30000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(MediaCache::completeFileForItem(item), completedPath);
    QVERIFY(QFileInfo(completedPath).suffix().isEmpty());

    MediaCache::State state;
    QVERIFY(MediaCache::readState(completedPath, &state));
    QCOMPARE(state.extension, QStringLiteral(".mp4"));
    QCOMPARE(state.identity, MediaCache::cacheIdentity(item));
    QVERIFY(QFileInfo(completedPath).size() > 0);

    // 合并 TS 只是中间产物，缓存成功必须只留无后缀正文 + .json。
    const QString tsPath = completedPath + QStringLiteral(".ts");
    QVERIFY(!QFileInfo::exists(tsPath));
    QVERIFY(!QFileInfo::exists(tsPath + QStringLiteral(".mvpdownload")));

    QVERIFY(MediaCache::removeForItem(item));
    QVERIFY(!QFileInfo::exists(completedPath));
}

void IntegrationTest::hlsCacheBuilderResumesAfterExit()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过 HLS 退出续传测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString sourceTs = m_tempDir.filePath(QStringLiteral("hls-resume-source.ts"));
    QFile::remove(sourceTs);
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                             QStringLiteral("-t"), QStringLiteral("2"),
                             QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                             QStringLiteral("-c:a"), QStringLiteral("mp2"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), sourceTs});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    QFile tsFile(sourceTs);
    QVERIFY(tsFile.open(QIODevice::ReadOnly));
    const QByteArray tsBytes = tsFile.readAll();
    QVERIFY(tsBytes.size() > 1024);
    const int split = tsBytes.size() / 2;

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "seg0.ts\n"
        "#EXTINF:1.0,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist, tsBytes, split](const LocalHttpServer::Request &request,
                                                QTcpSocket *socket) {
        if (request.path == QStringLiteral("/resume-cache/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/resume-cache/seg0.ts")) {
            LocalHttpServer::respond(socket, 200, tsBytes.left(split), "video/mp2t");
        } else if (request.path == QStringLiteral("/resume-cache/seg1.ts")) {
            // 第二片延迟，给测试留出“下到一半退出”的窗口。
            LocalHttpServer::respond(socket, 200, tsBytes.mid(split), "video/mp2t",
                                     -1, {}, 1500);
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("HLS 退出续传测试");
    item.url = server.url(QStringLiteral("/resume-cache/index.m3u8"));
    item.online = true;

    const QString base = MediaCache::filePathForItem(item);
    const QStringList trackFiles = {
        base + QStringLiteral(".video"),
        base + QStringLiteral(".ts"),
        base + QStringLiteral(".mp4"),
        base + QStringLiteral(".ts.mvpdownload"),
        base + QStringLiteral(".mp4.mvpdownload"),
        base + QStringLiteral(".video.mvpdownload"),
        base + QStringLiteral(".video.ts.mvpdownload"),
        base + QStringLiteral(".video.mp4.mvpdownload"),
        base + QStringLiteral(".partial.json"),
        base + QStringLiteral(".video.resume.json")
    };
    MediaCache::removeForItem(item);
    for (const QString &path : trackFiles)
        QFile::remove(path);

    HttpClient http;
    HlsCacheBuilder builder(&http);
    bool completed = false;
    bool failed = false;
    QString completedPath;
    QString failureMessage;
    connect(&builder, &HlsCacheBuilder::completed, &builder,
            [&](const MediaItem &, const QString &path) {
                completed = true;
                completedPath = path;
            });
    connect(&builder, &HlsCacheBuilder::failed, &builder,
            [&](const MediaItem &, const QString &message) {
                failed = true;
                failureMessage = message;
            });

    auto partialBytes = [&]() {
        qint64 bytes = 0;
        for (const QString &suffix : { QStringLiteral(".ts.mvpdownload"),
                                       QStringLiteral(".mp4.mvpdownload"),
                                       QStringLiteral(".video.mvpdownload"),
                                       QStringLiteral(".video.ts.mvpdownload"),
                                       QStringLiteral(".video.mp4.mvpdownload") })
            bytes = qMax(bytes, QFileInfo(base + suffix).size());
        return bytes;
    };
    auto resumedSegments = [&]() {
        QFile state(base + QStringLiteral(".video.resume.json"));
        if (!state.open(QIODevice::ReadOnly))
            return -1;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(state.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return -1;
        return document.object().value(QStringLiteral("segmentIndex")).toInt(-1);
    };

    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(resumedSegments() >= 1, 5000);

    builder.stopForExit();
    QTest::qWait(100);
    // 退出不能删除已下载的分片，续传状态也必须可读。
    QVERIFY(partialBytes() >= split);
    QVERIFY(QFileInfo::exists(base + QStringLiteral(".video.resume.json")));

    completed = false;
    failed = false;
    failureMessage.clear();
    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 30000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(MediaCache::completeFileForItem(item), completedPath);

    int firstSegmentRequests = 0;
    for (const LocalHttpServer::Request &request : server.requests) {
        if (request.path == QStringLiteral("/resume-cache/seg0.ts"))
            ++firstSegmentRequests;
    }
    // 第二次启动必须跳过已完成的第一片，而不是从头重下。
    QCOMPARE(firstSegmentRequests, 1);
    QVERIFY(!QFileInfo::exists(base + QStringLiteral(".video.resume.json")));
    QVERIFY(!QFileInfo::exists(base + QStringLiteral(".partial.json")));
    QVERIFY(!QFileInfo::exists(base + QStringLiteral(".video.ts.mvpdownload")));

    QVERIFY(MediaCache::removeForItem(item));
}

void IntegrationTest::downloadManagerReusesPlaybackCacheThroughStaging()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过下载暂存与播放缓存复用测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString sourceTs = m_tempDir.filePath(QStringLiteral("download-seed-source.ts"));
    QFile::remove(sourceTs);
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                             QStringLiteral("-t"), QStringLiteral("2"),
                             QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                             QStringLiteral("-c:a"), QStringLiteral("mp2"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), sourceTs});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    QFile tsFile(sourceTs);
    QVERIFY(tsFile.open(QIODevice::ReadOnly));
    const QByteArray tsBytes = tsFile.readAll();
    QVERIFY(tsBytes.size() > 1024);
    const int split = tsBytes.size() / 2;

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "seg0.ts\n"
        "#EXTINF:1.0,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist, tsBytes, split](const LocalHttpServer::Request &request,
                                                QTcpSocket *socket) {
        if (request.path == QStringLiteral("/download-seed/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/download-seed/seg0.ts")) {
            LocalHttpServer::respond(socket, 200, tsBytes.left(split), "video/mp2t");
        } else if (request.path == QStringLiteral("/download-seed/seg1.ts")) {
            LocalHttpServer::respond(socket, 200, tsBytes.mid(split), "video/mp2t",
                                     -1, {}, 1500);
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("下载暂存复用测试");
    item.url = server.url(QStringLiteral("/download-seed/index.m3u8"));
    item.online = true;
    const QString base = MediaCache::filePathForItem(item);
    MediaCache::removeForItem(item);

    // 第一段模拟“看着视频缓存到一半就退出”：只完成 seg0。
    HttpClient http;
    HlsCacheBuilder builder(&http);
    QVERIFY(builder.start(item));
    const QString resumeState = base + QStringLiteral(".video.resume.json");
    auto partialSegments = [&]() {
        QFile state(resumeState);
        if (!state.open(QIODevice::ReadOnly))
            return -1;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(state.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return -1;
        return document.object().value(QStringLiteral("segmentIndex")).toInt(-1);
    };
    QTRY_VERIFY_WITH_TIMEOUT(partialSegments() >= 1, 5000);
    builder.stopForExit();
    QTest::qWait(100);

    MediaCache::HlsTrackSeed cacheSeed;
    QVERIFY(MediaCache::hlsTrackSeedForItem(item, &cacheSeed));
    QVERIFY(cacheSeed.bytes >= split);
    QCOMPARE(cacheSeed.segmentIndex, 1);
    QVERIFY(!cacheSeed.fingerprint.isEmpty());

    // 手动下载必须把播放缓存已完成分片复制到暂存区，再从 seg1 继续；
    // 下载目录里不能提前出现最终文件或半成品。
    const QString downloadDir = m_tempDir.filePath(QStringLiteral("download-seed-output"));
    QVERIFY(QDir(downloadDir).removeRecursively() || !QFileInfo::exists(downloadDir));
    QVERIFY(QDir().mkpath(downloadDir));
    const QString target = QDir(downloadDir).filePath(QStringLiteral("下载暂存复用测试.mp4"));

    DownloadManager manager(&http);
    DownloadManager::CacheSeed seed;
    seed.sourcePath = cacheSeed.dataPath;
    seed.bytes = cacheSeed.bytes;
    seed.segmentIndex = cacheSeed.segmentIndex;
    seed.fingerprint = cacheSeed.fingerprint;
    seed.cacheBodyPath = cacheSeed.bodyPath;
    QStringList completed;
    connect(&manager, &DownloadManager::jobCompleted, &manager,
            [&completed](const MediaItem &, const QString &path) {
                completed.append(path);
            });
    const QString id = manager.enqueue(item, item.url, {}, target, true,
                                       NetLimits::kDownloadBytes, nullptr, nullptr, &seed);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 30000);

    const QString saved = completed.first();
    QVERIFY(saved.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive));
    QVERIFY(QFileInfo(saved).size() > 0);
    QCOMPARE(QFileInfo(saved).absolutePath(), QDir(downloadDir).absolutePath());
    // 暂存目录按任务 id 隔离，发布成功后必须整体清理。
    const QString stagingDir =
        QDir(downloadDir).filePath(QStringLiteral(".download-cache/") + id);
    QVERIFY(!QDir(stagingDir).exists());

    int firstSegmentRequests = 0;
    for (const LocalHttpServer::Request &request : server.requests) {
        if (request.path == QStringLiteral("/download-seed/seg0.ts"))
            ++firstSegmentRequests;
    }
    // seg0 只由播放缓存下载过一次；手动下载必须从 seg1 开始。
    QCOMPARE(firstSegmentRequests, 1);

    // 默认设置下，最终 MP4 发布成功后清理播放缓存和轨道现场。
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(resumeState), 3000);

    QStringList finalJobs;
    for (const DownloadJob &job : manager.jobs())
        finalJobs.append(job.status == DownloadJob::Status::Completed ? "ok" : "bad");
    QCOMPARE(finalJobs.size(), 1);
    QCOMPARE(finalJobs.first(), QStringLiteral("ok"));
    manager.shutdownForExit();
}

void IntegrationTest::hlsCacheBuilderMergesSingleFileAudio()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过单文件 HLS 音轨合并测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString videoPath = m_tempDir.filePath(QStringLiteral("single-video.mp4"));
    const QString audioPath = m_tempDir.filePath(QStringLiteral("single-audio.mp4"));
    QFile::remove(videoPath);
    QFile::remove(audioPath);
    auto generate = [&ffmpeg](const QStringList &args) {
        QProcess process;
        process.start(ffmpeg, args);
        return process.waitForFinished(30000) && process.exitCode() == 0;
    };
    QVERIFY(generate({QStringLiteral("-y"), QStringLiteral("-nostdin"),
                      QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                      QStringLiteral("-t"), QStringLiteral("2"),
                      QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
                      QStringLiteral("-q:v"), QStringLiteral("5"),
                      QStringLiteral("-f"), QStringLiteral("mp4"), videoPath}));
    QVERIFY(generate({QStringLiteral("-y"), QStringLiteral("-nostdin"),
                      QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                      QStringLiteral("-t"), QStringLiteral("2"),
                      QStringLiteral("-c:a"), QStringLiteral("aac"),
                      QStringLiteral("-b:a"), QStringLiteral("64k"),
                      QStringLiteral("-f"), QStringLiteral("mp4"), audioPath}));
    const qint64 videoBytes = QFileInfo(videoPath).size();
    QFile videoFile(videoPath);
    QVERIFY(videoFile.open(QIODevice::ReadOnly));
    const QByteArray videoBody = videoFile.readAll();
    QFile audioFile(audioPath);
    QVERIFY(audioFile.open(QIODevice::ReadOnly));
    const QByteArray audioBody = audioFile.readAll();
    QVERIFY(videoBytes > 0);
    QVERIFY(audioBody.size() > 0);

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray masterBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud1\",NAME=\"English\",DEFAULT=YES,"
        "URI=\"audio/prog.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=160x90,CODECS=\"mp4v\" "
        "AUDIO=\"aud1\"\n"
        "low/prog.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=160x90,CODECS=\"mp4v\" "
        "AUDIO=\"aud1\"\n"
        "high/prog.m3u8\n");
    const QByteArray videoPlaylist = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXTINF:2.0,\n"
        "#EXT-X-BYTERANGE:%1@0\n"
        "video.mp4\n"
        "#EXT-X-ENDLIST\n").arg(videoBytes).toUtf8();
    const QByteArray audioPlaylist = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXTINF:2.0,\n"
        "#EXT-X-BYTERANGE:%1@0\n"
        "audio.mp4\n"
        "#EXT-X-ENDLIST\n").arg(audioBody.size()).toUtf8();

    server.handler = [masterBody, videoPlaylist, audioPlaylist, videoBody,
                      audioBody](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QStringLiteral("/media/master.m3u8")) {
            LocalHttpServer::respond(socket, 200, masterBody,
                                     "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/media/high/prog.m3u8")
                   || request.path == QStringLiteral("/media/low/prog.m3u8")) {
            LocalHttpServer::respond(socket, 200, videoPlaylist,
                                     "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/media/audio/prog.m3u8")) {
            LocalHttpServer::respond(socket, 200, audioPlaylist,
                                     "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/media/high/video.mp4")) {
            LocalHttpServer::respond(socket, 200, videoBody, "video/mp4");
        } else if (request.path == QStringLiteral("/media/audio/audio.mp4")) {
            LocalHttpServer::respond(socket, 200, audioBody, "audio/mp4");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("单文件音轨合并测试");
    item.url = server.url(QStringLiteral("/media/master.m3u8"));
    item.online = true;

    HttpClient http;
    HlsCacheBuilder builder(&http);
    const QUrl highUrl = server.url(QStringLiteral("/media/high/prog.m3u8"));
    const QUrl audioUrl = server.url(QStringLiteral("/media/audio/prog.m3u8"));
    builder.setPlaybackTrackResolvers([highUrl] { return highUrl; },
                                      [audioUrl] { return audioUrl; });

    bool completed = false;
    bool failed = false;
    QString completedPath;
    QString failureMessage;
    connect(&builder, &HlsCacheBuilder::completed, &builder,
            [&](const MediaItem &, const QString &path) {
                completed = true;
                completedPath = path;
            });
    connect(&builder, &HlsCacheBuilder::failed, &builder,
            [&](const MediaItem &, const QString &message) {
                failed = true;
                failureMessage = message;
            });

    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 40000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(MediaCache::completeFileForItem(item), completedPath);
    QVERIFY(QFileInfo(completedPath).size() > videoBytes);

    // 单文件视频和独立音轨都走了整文件直下，不再发分片请求；
    // 中间 .video/.audio 轨道文件必须清理干净。
    int mediaRequests = 0;
    for (const auto &request : server.requests) {
        if (request.path.endsWith(QByteArrayLiteral("video.mp4"))
            || request.path.endsWith(QByteArrayLiteral("audio.mp4"))) {
            ++mediaRequests;
        }
        QVERIFY(!request.path.contains(QByteArrayLiteral(".ts")));
    }
    QCOMPARE(mediaRequests, 2);
    QVERIFY(!QFileInfo::exists(completedPath + QStringLiteral(".video")));
    QVERIFY(!QFileInfo::exists(completedPath + QStringLiteral(".audio")));

    // 用 ffmpeg 实际解码一遍，确认产物同时含视频和独立音轨。
    QProcess probe;
    probe.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-i"),
                         completedPath, QStringLiteral("-f"), QStringLiteral("null"),
                         QStringLiteral("-")});
    QVERIFY(probe.waitForFinished(30000));
    const QString probeLog = QString::fromUtf8(probe.readAllStandardError());
    QVERIFY2(probe.exitCode() == 0, qPrintable(probeLog));
    QVERIFY2(probeLog.contains(QStringLiteral("Video:")), qPrintable(probeLog));
    QVERIFY2(probeLog.contains(QStringLiteral("Audio:")), qPrintable(probeLog));

    QVERIFY(MediaCache::removeForItem(item));
}

void IntegrationTest::hlsCacheBuilderFollowsRealAppleStream()
{
    // 真实公网流默认跳过，避免 CI/离线环境失败；手动验收时设置该环境变量。
    if (!qEnvironmentVariableIsSet("MVP_REAL_APPLE_HLS"))
        QSKIP("未设置 MVP_REAL_APPLE_HLS，跳过真实 Apple 单文件 HLS 验收");
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过真实 Apple 单文件 HLS 验收");

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("Apple 单文件 HLS 验收");
    item.url = QUrl(QStringLiteral(
        "https://devstreaming-cdn.apple.com/videos/streaming/examples/"
        "img_bipbop_adv_example_fmp4/master.m3u8"));
    item.online = true;
    // 模拟播放器当前打开的是 v4 视频和 a1 音轨；缓存必须跟随这两个地址，
    // 而不是主列表里带宽最高的 v9。
    const QUrl video(QStringLiteral(
        "https://devstreaming-cdn.apple.com/videos/streaming/examples/"
        "img_bipbop_adv_example_fmp4/v4/prog_index.m3u8"));
    const QUrl audio(QStringLiteral(
        "https://devstreaming-cdn.apple.com/videos/streaming/examples/"
        "img_bipbop_adv_example_fmp4/a1/prog_index.m3u8"));

    HttpClient http;
    HlsCacheBuilder builder(&http);
    builder.setPlaybackTrackResolvers([video] { return video; },
                                      [audio] { return audio; });
    bool completed = false;
    bool failed = false;
    QString completedPath;
    QString failureMessage;
    QStringList details;
    connect(&builder, &HlsCacheBuilder::completed, &builder,
            [&](const MediaItem &, const QString &path) {
                completed = true;
                completedPath = path;
            });
    connect(&builder, &HlsCacheBuilder::failed, &builder,
            [&](const MediaItem &, const QString &message) {
                failed = true;
                failureMessage = message;
            });
    connect(&builder, &HlsCacheBuilder::progressChanged, &builder,
            [&](const MediaItem &, int, const QString &detail) { details.append(detail); });

    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 300000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(MediaCache::completeFileForItem(item), completedPath);
    QVERIFY(QFileInfo(completedPath).size() > 70'000'000);
    QVERIFY(!QFileInfo::exists(completedPath + QStringLiteral(".video")));
    QVERIFY(!QFileInfo::exists(completedPath + QStringLiteral(".audio")));
    QVERIFY(!details.isEmpty());

    MediaCache::State state;
    QVERIFY(MediaCache::readState(completedPath, &state));
    QCOMPARE(state.extension, QStringLiteral(".mp4"));
    QCOMPARE(state.contentType, QByteArrayLiteral("video/mp4"));

    QProcess probe;
    probe.start(MediaRemux::ffmpegExecutable(),
                {QStringLiteral("-hide_banner"), QStringLiteral("-i"), completedPath});
    QVERIFY(probe.waitForFinished(60000));
    const QString probeLog = QString::fromUtf8(probe.readAllStandardError());
    QVERIFY2(probeLog.contains(QStringLiteral("Video:")), qPrintable(probeLog));
    QVERIFY2(probeLog.contains(QStringLiteral("Audio:")), qPrintable(probeLog));

    QVERIFY(MediaCache::removeForItem(item));
}

void IntegrationTest::lyricsServiceCachesOnlinePayload()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray payload = QByteArrayLiteral(
        "{\"lrc\":{\"lyric\":\"[00:01.00]第一句\\n[00:02.00]第二句\"}}");
    server.handler = [payload](const LocalHttpServer::Request &request,
                               QTcpSocket *socket) {
        if (request.path.startsWith("/lyric"))
            LocalHttpServer::respond(socket, 200, payload, "application/json");
        else
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    MediaItem music;
    music.type = MediaType::Music;
    music.title = QStringLiteral("在线歌词缓存测试");
    music.url = QUrl(QStringLiteral("https://music.example.com/song.mp3?id=42"));
    music.lyricUrl = server.url(QStringLiteral("/lyric?id=42"));
    music.online = true;

    HttpClient http;
    LyricsService service(&http);
    int ready = 0;
    connect(&service, &LyricsService::lyricsReady, &service,
            [&](const QVector<LrcLine> &) { ++ready; });
    service.load(music.lyricUrl, music);
    QTRY_COMPARE_WITH_TIMEOUT(ready, 1, 5000);
    QCOMPARE(server.requests.size(), 1);

    QByteArray cachedPayload;
    QVERIFY(LyricsCache::load(music, &cachedPayload));
    QCOMPARE(cachedPayload, payload);

    // 第二次加载同一首歌必须命中磁盘缓存，不再请求歌词接口。
    LyricsService cachedService(&http);
    int cachedReady = 0;
    connect(&cachedService, &LyricsService::lyricsReady, &cachedService,
            [&](const QVector<LrcLine> &) { ++cachedReady; });
    cachedService.load(music.lyricUrl, music);
    QCOMPARE(cachedReady, 1);
    QCOMPARE(server.requests.size(), 1);

    QVERIFY(LyricsCache::removeForItem(music));
}

void IntegrationTest::downloadTaskAllowsVodWithoutEndList()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1.0,\n"
        "seg0.bin\n";
    const QByteArray segment("vod-segment");
    server.handler = [playlist, segment](const LocalHttpServer::Request &request,
                                         QTcpSocket *socket) {
        if (request.path == QStringLiteral("/vod/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/vod/seg0.bin")) {
            LocalHttpServer::respond(socket, 200, segment, "application/octet-stream");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    HttpClient http;
    const QString target = m_tempDir.filePath(QStringLiteral("vod-no-endlist.ts"));
    QFile::remove(target);
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/vod/index.m3u8")), target);
    QSignalSpy snapshot(task, &DownloadTask::snapshotAccepted);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 10000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(succeeded.count(), 1);
    QCOMPARE(snapshot.count(), 1);
    QVERIFY(QFileInfo(target).size() > 0);
}

void IntegrationTest::downloadTaskRequiresConsentWithoutVod()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1.0,\n"
        "seg0.bin\n";
    const QByteArray segment("forced-segment");
    server.handler = [playlist, segment](const LocalHttpServer::Request &request,
                                         QTcpSocket *socket) {
        if (request.path == QStringLiteral("/unknown/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/unknown/seg0.bin")) {
            LocalHttpServer::respond(socket, 200, segment, "application/octet-stream");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    HttpClient http;
    const QString target = m_tempDir.filePath(QStringLiteral("unknown-no-endlist.ts"));
    QFile::remove(target);

    // 没有 VOD 声明时默认拒绝，并明确告诉调用方需要确认。
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/unknown/index.m3u8")), target);
    QSignalSpy consent(task, &DownloadTask::snapshotConsentRequired);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() > 0, 10000);
    QCOMPARE(consent.count(), 1);
    QVERIFY(!QFileInfo::exists(target));

    // 用户确认后允许按当前分片下载，但结果必须标记为快照。
    QFile::remove(target);
    DownloadTask *forced =
        DownloadTask::start(&http, server.url(QStringLiteral("/unknown/index.m3u8")), target);
    forced->setAllowIncompletePlaylist(true);
    QSignalSpy forcedSnapshot(forced, &DownloadTask::snapshotAccepted);
    QSignalSpy forcedSucceeded(forced, &DownloadTask::succeeded);
    QSignalSpy forcedFailed(forced, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(forcedSucceeded.count() + forcedFailed.count() > 0, 10000);
    QCOMPARE(forcedFailed.count(), 0);
    QCOMPARE(forcedSucceeded.count(), 1);
    QCOMPARE(forcedSnapshot.count(), 1);
    QVERIFY(QFileInfo(target).size() > 0);
}

void IntegrationTest::downloadManagerForcesSnapshotOnlyAfterConsent()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1.0,\n"
        "seg0.bin\n";
    const QByteArray segment("manager-forced-segment");
    server.handler = [playlist, segment](const LocalHttpServer::Request &request,
                                         QTcpSocket *socket) {
        if (request.path == QStringLiteral("/mgmt/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/mgmt/seg0.bin")) {
            LocalHttpServer::respond(socket, 200, segment, "application/octet-stream");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    HttpClient http;
    DownloadManager manager(&http);
    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("无结束标记任务");
    item.url = server.url(QStringLiteral("/mgmt/index.m3u8"));
    item.online = true;
    const QString target = m_tempDir.filePath(QStringLiteral("manager-no-endlist.ts"));
    QFile::remove(target);

    const QString id = manager.enqueue(item, item.url, {}, target, true, 4096);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(manager.jobs().at(0).status,
                              DownloadJob::Status::Failed, 10000);
    QVERIFY(manager.jobs().at(0).needsSnapshotConsent);
    QVERIFY(!manager.jobs().at(0).snapshot);

    // 普通重试仍然拒绝，不能绕过产品确认。
    QVERIFY(manager.retryJob(id));
    QTRY_COMPARE_WITH_TIMEOUT(manager.jobs().at(0).status,
                              DownloadJob::Status::Failed, 10000);
    QVERIFY(manager.jobs().at(0).needsSnapshotConsent);

    // 显式授权后才允许快照下载，并持久化 snapshot 标记。
    QVERIFY(manager.retryJob(id, true));
    QTRY_VERIFY_WITH_TIMEOUT(manager.jobs().at(0).snapshot, 10000);
    QVERIFY(manager.jobs().at(0).allowIncomplete);
    QVERIFY(!manager.jobs().at(0).needsSnapshotConsent);

    manager.removeJob(id);
}

void IntegrationTest::mediaRemuxConvertsTsToMp4()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过内置转封装测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    QVERIFY(!ffmpeg.isEmpty());

    const QString tsPath = m_tempDir.filePath(QStringLiteral("remux-sample.ts"));
    const QString mp4Path = m_tempDir.filePath(QStringLiteral("remux-sample.mp4"));
    QFile::remove(tsPath);
    QFile::remove(mp4Path);

    // 用 FFmpeg 自己生成 1 秒 MPEG-TS（LGPL 构建没有 libx264，用 mpeg2video）。
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                             QStringLiteral("-t"), QStringLiteral("1"),
                             QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                             QStringLiteral("-c:a"), QStringLiteral("mp2"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), tsPath});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    QVERIFY(QFileInfo(tsPath).size() > 0);

    MediaRemux remux;
    QSignalSpy finished(&remux, &MediaRemux::finished);
    remux.remuxToMp4(tsPath, mp4Path);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 30000);
    QCOMPARE(finished.first().at(0).toBool(), true);
    QCOMPARE(finished.first().at(1).toString(), mp4Path);
    QVERIFY(QFileInfo(mp4Path).size() > 0);
}

void IntegrationTest::streamProxyPreservesEncodedSegmentQuery()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    // 签名查询串故意包含 %26/%3D：代理如果把 query 再解码一次，会变成
    // token=a&b=c，上游收到的签名就和播放列表里不一致。
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "seg.ts?token=a%26b%3Dc\n"
        "#EXT-X-ENDLIST\n";

    server.handler = [](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path.startsWith("/hls/seg.ts")) {
            LocalHttpServer::respond(socket, 200, QByteArray(32, 's'), "video/mp2t");
            return;
        }
        LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QUrl playlistUrl = server.url(QStringLiteral("/hls/list.m3u8"));
    const QByteArray rewritten = HlsPlaylist::rewrite(
        playlist, playlistUrl,
        [&proxy](const QUrl &absolute) { return proxy.proxyUrlFor(absolute); });

    QString segmentLine;
    for (const QString &line : QString::fromUtf8(rewritten).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#'))) {
            segmentLine = trimmed;
            break;
        }
    }
    QVERIFY(!segmentLine.isEmpty());

    const QUrl segmentUrl = QUrl::fromEncoded(segmentLine.toUtf8());
    HttpClient http;
    QNetworkReply *reply = http.get(segmentUrl);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
    QVERIFY(HttpClient::isHttpSuccess(reply));
    reply->deleteLater();

    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().path,
             QByteArray("/hls/seg.ts?token=a%26b%3Dc"));
    proxy.shutdown();
}

void IntegrationTest::streamProxyServesFirstSegmentOfLongPlaylist()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path.startsWith("/hls/seg")) {
            LocalHttpServer::respond(socket, 200, QByteArray(64, 's'), "video/mp2t");
            return;
        }
        LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    // 复刻正片级长播放列表：2 秒一片、6000 片。旧令牌池只有 4096 个位置，
    // 重写后开头分片的 token 已被淘汰，播放器请求第一片就会 404。
    QByteArray playlist = "#EXTM3U\n#EXT-X-VERSION:3\n"
                          "#EXT-X-TARGETDURATION:2\n#EXT-X-PLAYLIST-TYPE:VOD\n";
    for (int i = 0; i < 6000; ++i) {
        playlist += "#EXTINF:2.0,\nseg";
        playlist += QByteArray::number(i);
        playlist += ".ts\n";
    }
    playlist += "#EXT-X-ENDLIST\n";

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl playlistUrl = server.url(QStringLiteral("/hls/long.m3u8"));
    const QByteArray rewritten = HlsPlaylist::rewrite(
        playlist, playlistUrl,
        [&proxy](const QUrl &absolute) { return proxy.proxyUrlFor(absolute); });

    QString firstSegment;
    for (const QString &line : QString::fromUtf8(rewritten).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#'))) {
            firstSegment = trimmed;
            break;
        }
    }
    QVERIFY(!firstSegment.isEmpty());

    HttpClient http;
    QNetworkReply *reply = http.get(QUrl::fromEncoded(firstSegment.toUtf8()));
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
    QVERIFY(HttpClient::isHttpSuccess(reply));
    reply->deleteLater();

    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().path, QByteArray("/hls/seg0.ts"));
    proxy.shutdown();
}

void IntegrationTest::streamProxySelectsBitrateVariant()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray master =
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"main\",URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,AUDIO=\"aud\"\n"
        "low/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x720,AUDIO=\"aud\"\n"
        "mid/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,AUDIO=\"aud\"\n"
        "high/index.m3u8\n";
    const QByteArray media =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXTINF:4,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";

    server.handler = [master, media](const LocalHttpServer::Request &request,
                                     QTcpSocket *socket) {
        if (request.path == QByteArray("/hls/master.m3u8")) {
            LocalHttpServer::respond(socket, 200, master, "application/vnd.apple.mpegurl");
        } else if (request.path == QByteArray("/hls/low/index.m3u8")
                   || request.path == QByteArray("/hls/mid/index.m3u8")
                   || request.path == QByteArray("/hls/high/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, media, "application/vnd.apple.mpegurl");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl masterUrl = server.url(QStringLiteral("/hls/master.m3u8"));
    const QUrl proxiedMaster = proxy.proxyUrlFor(masterUrl);
    QVERIFY(proxiedMaster.isValid());

    HttpClient http;
    auto fetch = [&http](const QUrl &url, QByteArray *body) {
        auto *reply = http.get(url);
        bool finished = false;
        bool success = false;
        QObject::connect(reply, &QNetworkReply::finished, reply, [&, reply] {
            success = HttpClient::isHttpSuccess(reply);
            if (success)
                *body = reply->readAll();
            finished = true;
            reply->deleteLater();
        });
        QElapsedTimer timer;
        timer.start();
        while (!finished && timer.elapsed() < 5000)
            QTest::qWait(20);
        return finished && success;
    };

    QByteArray first;
    QVERIFY(fetch(proxiedMaster, &first));
    const StreamProxy::VariantState initial = proxy.variantState();
    QCOMPARE(initial.variants.size(), 3);
    QCOMPARE(initial.selectedIndex, -1);
    QCOMPARE(initial.variants.at(1).bandwidth, 2400000);

    int childLines = 0;
    for (const QString &line : QString::fromUtf8(first).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#'))) {
            ++childLines;
        }
    }
    QCOMPARE(childLines, 3);

    // 锁定 720P：代理再返回 master 时只能剩一个视频变体。
    proxy.setVariantIndex(1);
    QCOMPARE(proxy.variantState().selectedIndex, 1);

    QByteArray locked;
    QVERIFY(fetch(proxiedMaster, &locked));
    childLines = 0;
    QString lockedChild;
    for (const QString &line : QString::fromUtf8(locked).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('#'))) {
            ++childLines;
            if (lockedChild.isEmpty())
                lockedChild = trimmed;
        }
    }
    QCOMPARE(childLines, 1);
    QVERIFY(!lockedChild.isEmpty());

    // 播放器抓到子列表后，实际播放序号必须回指到刚锁定的 720P 变体，
    // 码率栏“当前”标签依赖这个反查结果。
    QByteArray childBody;
    QVERIFY(fetch(QUrl::fromEncoded(lockedChild.toUtf8()), &childBody));
    const StreamProxy::VariantState playing = proxy.variantState();
    QCOMPARE(playing.currentIndex, 1);

    proxy.setVariantIndex(-1);
    QCOMPARE(proxy.variantState().selectedIndex, -1);
    proxy.shutdown();
}

void IntegrationTest::playbackControllerSelectsHlsVariant()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过 HLS 码率切换测试");

    const QString root = m_tempDir.filePath(QStringLiteral("bitrate-hls"));
    QDir(root).removeRecursively();
    QVERIFY(QDir().mkpath(root + QStringLiteral("/low")));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/high")));

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    auto generate = [&ffmpeg](const QString &size, const QString &bitrate,
                              const QString &dir) -> bool {
        QProcess generator;
        generator.start(ffmpeg,
                        {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                         QStringLiteral("-loglevel"), QStringLiteral("error"),
                         QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"),
                         QStringLiteral("testsrc=size=%1:rate=15").arg(size),
                         QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                         QStringLiteral("-t"), QStringLiteral("8"),
                         QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                         QStringLiteral("-b:v"), bitrate,
                         QStringLiteral("-c:a"), QStringLiteral("aac"),
                         QStringLiteral("-b:a"), QStringLiteral("64k"),
                         QStringLiteral("-hls_time"), QStringLiteral("2"),
                         QStringLiteral("-hls_playlist_type"), QStringLiteral("vod"),
                         QStringLiteral("-hls_segment_filename"),
                         QDir(dir).filePath(QStringLiteral("seg%03d.ts")),
                         QDir(dir).filePath(QStringLiteral("index.m3u8"))});
        return generator.waitForFinished(30000) && generator.exitCode() == 0;
    };
    QVERIFY2(generate(QStringLiteral("160x90"), QStringLiteral("300k"),
                      root + QStringLiteral("/low")),
             "无法生成低码率 HLS");
    QVERIFY2(generate(QStringLiteral("320x180"), QStringLiteral("800k"),
                      root + QStringLiteral("/high")),
             "无法生成高码率 HLS");

    const QString masterPath = root + QStringLiteral("/master.m3u8");
    QFile masterFile(masterPath);
    QVERIFY(masterFile.open(QIODevice::WriteOnly | QIODevice::Text));
    masterFile.write("#EXTM3U\n"
                     "#EXT-X-VERSION:3\n"
                     "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=160x90\n"
                     "low/index.m3u8\n"
                     "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=320x180\n"
                     "high/index.m3u8\n");
    masterFile.close();

    LocalHttpServer server;
    QVERIFY(server.start());
    const QString rootCopy = root;
    server.handler = [rootCopy](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        QString path = QString::fromUtf8(request.path).section(QLatin1Char('?'), 0, 0);
        if (!path.startsWith(QLatin1String("/hls/"))) {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
            return;
        }
        const QString relative = path.mid(5);
        if (relative.contains(QLatin1String(".."))) {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
            return;
        }
        QFile file(rootCopy + QLatin1Char('/') + relative);
        if (!file.open(QIODevice::ReadOnly)) {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
            return;
        }
        const QByteArray body = file.readAll();
        const QByteArray type = relative.endsWith(QLatin1String(".m3u8"))
                                    ? QByteArrayLiteral("application/vnd.apple.mpegurl")
                                    : QByteArrayLiteral("video/mp2t");
        LocalHttpServer::respond(socket, 200, body, type);
    };

    PlaylistModel playlist;
    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("bitrate-switch");
    item.url = server.url(QStringLiteral("/hls/master.m3u8"));
    item.online = true;
    item.sourceOrigin = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    item.durationMs = 8000;
    playlist.addItem(item);

    PlaybackController player;
    player.setPlaylist(&playlist);
    player.playRow(0);

    QElapsedTimer timer;
    timer.start();
    while (player.bitrateSnapshot().options.size() < 2 && timer.elapsed() < 12000)
        QTest::qWait(100);
    const auto initial = player.bitrateSnapshot();
    QVERIFY2(initial.options.size() == 2,
             qPrintable(QStringLiteral("码率选项 %1 个").arg(initial.options.size())));
    QCOMPARE(initial.selectedIndex, -1);

    // 等真正播过 3 秒，确保保存的不是“片头忽略”区间。
    timer.restart();
    while (player.position() < 3000 && timer.elapsed() < 15000)
        QTest::qWait(100);
    if (player.position() < 3000)
        QSKIP("当前环境没有可用的 HLS 多媒体后端，跳过码率切换测试");
    const qint64 savedPosition = player.position();

    player.selectBitrate(1);
    QCOMPARE(player.bitrateSnapshot().selectedIndex, 1);

    timer.restart();
    while (player.bitrateSnapshot().currentIndex != 1 && timer.elapsed() < 15000)
        QTest::qWait(100);
    QVERIFY2(player.bitrateSnapshot().currentIndex == 1,
             qPrintable(QStringLiteral("切换后实际码率序号 %1")
                            .arg(player.bitrateSnapshot().currentIndex)));

    timer.restart();
    while (player.position() + 1500 < savedPosition && timer.elapsed() < 15000)
        QTest::qWait(100);
    QVERIFY2(player.position() + 1500 >= savedPosition,
             qPrintable(QStringLiteral("切码率后位置 %1，保存位置 %2")
                            .arg(player.position())
                            .arg(savedPosition)));

    // 回到自动：代理必须恢复完整 master，而不是继续锁着 800k。
    player.selectBitrate(-1);
    QCOMPARE(player.bitrateSnapshot().selectedIndex, -1);
    player.stop();
}

void IntegrationTest::streamProxyReleaseCacheFileClosesEntry()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray body(4096, 'a');
    server.handler = [body](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QByteArray("/audio.mp3"))
            LocalHttpServer::respond(socket, 200, body, "audio/mpeg");
        else
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    const QString cachePath =
        m_tempDir.filePath(QStringLiteral("proxy-release/media"));
    QVERIFY(QDir().mkpath(QFileInfo(cachePath).absolutePath()));

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl proxied = proxy.proxyUrlFor(server.url(QStringLiteral("/audio.mp3")), {},
                                           cachePath, QStringLiteral("test#release"));
    QVERIFY(proxied.isValid());

    HttpClient http;
    QNetworkReply *reply = http.get(proxied);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
    QVERIFY(HttpClient::isHttpSuccess(reply));
    QCOMPARE(reply->readAll().size(), body.size());
    reply->deleteLater();

    // 代理为支持拖动会保留缓存条目和打开的文件句柄；删除条目前必须能把
    // 对应条目释放掉，否则 Windows 上 MediaCache::removeForItem 会一直失败。
    QTRY_COMPARE_WITH_TIMEOUT(proxy.cacheStats().entries, 1, 3000);
    proxy.releaseCacheFile(cachePath);
    QCOMPARE(proxy.cacheStats().entries, 0);
    QVERIFY(QFileInfo::exists(cachePath));
    QVERIFY(QFile::remove(cachePath));
    proxy.shutdown();
}

void IntegrationTest::searchTimeoutAbortsProvider()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    // 收到请求后故意不响应，让单源计时器成为唯一出口。
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *) {};

    SourceRegistry registry;
    QVERIFY(registry.load());
    QVERIFY(registry.replaceAll({makeSource(server.url(QStringLiteral("/search?q={keyword}")), 1000)}));

    HttpClient http;
    SearchCoordinator coordinator(&registry, &http);

    QStringList failedSources;
    bool finished = false;
    connect(&coordinator, &SearchCoordinator::searchSucceeded, this,
            [&failedSources](const QVector<MediaItem> &, int, const QStringList &errors) {
                failedSources = errors;
            });
    connect(&coordinator, &SearchCoordinator::searchFinished, this,
            [&finished] { finished = true; });

    QElapsedTimer timer;
    timer.start();
    coordinator.search(QStringLiteral("测试"), 5);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 4000);
    QVERIFY(timer.elapsed() < 3500);
    QCOMPARE(failedSources.size(), 1);
    QVERIFY(failedSources.first().contains(QStringLiteral("单源超过")));
    QCOMPARE(server.requests.size(), 1);
}

void IntegrationTest::maccmsAbortClosesInflightRequest()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    // 首页请求故意不响应；旧实现会一直挂到 QNAM 的 15 秒传输超时。
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *) {};

    SourceConfig config;
    config.id = QStringLiteral("maccms_local");
    config.name = QStringLiteral("本地网页源");
    config.kind = QStringLiteral("builtin");
    config.provider = QStringLiteral("maccms_web");
    config.mediaType = MediaType::Video;
    config.urlTemplate = server.url(QStringLiteral("/")).toString();

    MaccmsWebProvider provider(config, nullptr);
    provider.search(QStringLiteral("测试"), 5);
    QTRY_COMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().path, QByteArray("/"));

    QElapsedTimer timer;
    timer.start();
    provider.abort();
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnects >= 1, 2500);
    QVERIFY(timer.elapsed() < 2500);
}

void IntegrationTest::externalAbortKillsProcessWithoutFinish()
{
    SourceConfig config;
    config.id = QStringLiteral("external_slow");
    config.name = QStringLiteral("慢解析器");
    config.kind = QStringLiteral("external");
    config.command = QStringLiteral("cmd.exe");
    // 不重定向输出，避免 cmd 对 > 的参数解析差异；ping 30 次足够让进程活着。
    config.arguments = QStringLiteral("/c ping 127.0.0.1 -n 30");
    config.timeoutMs = 30000;

    ExternalSearchProvider provider(config);
    bool finished = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&finished](const QString &, const QVector<MediaItem> &, int, const QString &) {
                finished = true;
            });

    provider.search(QStringLiteral("测试"), 5);
    QProcess *process = provider.findChild<QProcess *>();
    QVERIFY(process != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::Running, 3000);

    provider.abort();
    QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, 2500);
    QTest::qWait(100);
    QVERIFY(!finished);
}

void IntegrationTest::responseLimitAbortsInflight()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray huge(6 * 1024 * 1024, 'x');
    server.handler = [huge](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, huge, "application/json");
    };

    HttpClient http;
    QNetworkReply *reply = http.get(server.url(QStringLiteral("/huge")));
    QVERIFY(reply != nullptr);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!reply->isFinished())
        QVERIFY(finished.wait(5000));
    QVERIFY(reply->property("mvpReplyLimitExceeded").toBool());
    // 200 响应被上限截断仍算“成功读到合法正文”，由 readAllLimited 报超限。
    QVERIFY(HttpClient::isHttpSuccess(reply));

    bool tooLarge = false;
    const QByteArray body = HttpClient::readAllLimited(reply, NetLimits::kJsonBytes, &tooLarge);
    QVERIFY(tooLarge);
    QVERIFY(body.isEmpty());
    reply->deleteLater();
}

void IntegrationTest::responseLimitOnHttpErrorKeepsStatus()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray huge(4 * 1024 * 1024, 'e');
    server.handler = [huge](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 404, huge, "text/plain");
    };

    HttpClient http;
    QNetworkReply *reply = http.get(server.url(QStringLiteral("/missing")));
    QVERIFY(reply != nullptr);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!reply->isFinished())
        QVERIFY(finished.wait(5000));
    QVERIFY(reply->property("mvpReplyLimitExceeded").toBool());
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);

    // 404 的大错误页被上限截断，仍要保留 HTTP 失败状态，不能被当成成功。
    QVERIFY(!HttpClient::isHttpSuccess(reply));
    QVERIFY(HttpClient::describeError(reply).contains(QStringLiteral("HTTP 404")));

    bool tooLarge = false;
    const QByteArray body = HttpClient::readAllLimited(reply, NetLimits::kJsonBytes, &tooLarge);
    QVERIFY(tooLarge);
    QVERIFY(body.isEmpty());
    reply->deleteLater();
}

void IntegrationTest::externalProviderScopesCookieToConfiguredOrigin()
{
    const QString script = m_tempDir.filePath(QStringLiteral("external_scope.cmd"));
    QFile file(script);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("@echo {\"items\":[{\"title\":\"scope test\","
              "\"streamUrl\":\"https://cdn.example/a.mp3\","
              "\"webUrl\":\"https://evil.example/page\"}]}\r\n");
    file.close();

    SourceConfig config;
    config.id = QStringLiteral("external_scope");
    config.name = QStringLiteral("外部解析器作用域");
    config.kind = QStringLiteral("external");
    config.mediaType = MediaType::Music;
    config.command = QStringLiteral("cmd.exe");
    config.arguments = QStringLiteral("/c ") + QDir::toNativeSeparators(script);
    config.urlTemplate = QStringLiteral("https://trusted.example/api?q={keyword}");
    config.cookie = QStringLiteral("session=trusted");
    config.timeoutMs = 5000;

    ExternalSearchProvider provider(config);
    QVector<MediaItem> items;
    QString error;
    bool done = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&](const QString &, const QVector<MediaItem> &result, int, const QString &message) {
                items = result;
                error = message;
                done = true;
            });

    provider.search(QStringLiteral("测试"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(done, 4000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(items.size(), 1);

    const MediaItem &item = items.first();
    // 解析器输出的 webUrl 指向第三方，但 Cookie 作用域必须来自用户配置的源站。
    QCOMPARE(item.sourceOrigin, QUrl(QStringLiteral("https://trusted.example")));
    QCOMPARE(item.headers.value(QStringLiteral("Cookie")), QStringLiteral("session=trusted"));
    QCOMPARE(item.headers.value(QStringLiteral("Referer")),
             QStringLiteral("https://trusted.example/api"));
    QVERIFY(!item.headers.value(QStringLiteral("Referer")).contains(QStringLiteral("evil")));

    // CDN 与源站不同 host：请求层必须剥掉 Cookie，避免把源站凭据带给第三方。
    const QHash<QString, QString> safe = HeaderPolicy::sanitizedForTarget(
        QUrl(QStringLiteral("https://cdn.example/a.mp3")), item.headers, item.sourceOrigin);
    QVERIFY(!safe.contains(QStringLiteral("Cookie")));
}

void IntegrationTest::externalProviderHonorsLimit()
{
    const QString script = m_tempDir.filePath(QStringLiteral("external_limit.cmd"));
    QFile file(script);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("@echo {\"items\":[{\"title\":\"one\",\"streamUrl\":\"https://cdn.example/1.mp3\"},"
              "{\"title\":\"two\",\"streamUrl\":\"https://cdn.example/2.mp3\"},"
              "{\"title\":\"three\",\"streamUrl\":\"https://cdn.example/3.mp3\"}]}\r\n");
    file.close();

    SourceConfig config;
    config.id = QStringLiteral("external_limit");
    config.name = QStringLiteral("外部解析器截断");
    config.kind = QStringLiteral("external");
    config.mediaType = MediaType::Music;
    config.command = QStringLiteral("cmd.exe");
    config.arguments = QStringLiteral("/c ") + QDir::toNativeSeparators(script);
    config.urlTemplate = QStringLiteral("https://trusted.example/api?q={keyword}");
    config.timeoutMs = 5000;

    ExternalSearchProvider provider(config);
    QVector<MediaItem> items;
    QString error;
    bool done = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&](const QString &, const QVector<MediaItem> &result, int, const QString &message) {
                items = result;
                error = message;
                done = true;
            });

    // 解析器没使用参数里的 {limit}，一次吐出 3 条；调用方只要 1 条。
    provider.search(QStringLiteral("测试"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(done, 4000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.first().title, QStringLiteral("one"));
}

void IntegrationTest::searchJsonOverLimitFails()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, QByteArray(NetLimits::kJsonBytes + 1, 'x'),
                                 "application/json");
    };

    HttpClient http;
    ConfigSearchProvider provider(makeSource(server.url(QStringLiteral("/search?q={keyword}"))),
                                  &http);
    QString error;
    bool finished = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&error, &finished](const QString &, const QVector<MediaItem> &, int,
                                const QString &message) {
                error = message;
                finished = true;
            });

    provider.search(QStringLiteral("测试"), 5);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    QVERIFY(error.contains(QStringLiteral("2 MB 上限")));
}

void IntegrationTest::requestLogsRedactQuerySecrets()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &, QTcpSocket *socket) {
        LocalHttpServer::respond(socket, 200, validSearchJson(), "application/json");
    };

    const QString url = QStringLiteral("http://127.0.0.1:%1/search?token=SECRET123&sign=SIGNED&q={keyword}")
                            .arg(server.serverPort());
    HttpClient http;
    ConfigSearchProvider provider(makeSource(QUrl(url)), &http);

    bool finished = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&finished](const QString &, const QVector<MediaItem> &, int, const QString &) {
                finished = true;
            });

    g_logMessages.clear();
    QtMessageHandler previous = qInstallMessageHandler(captureMessages);
    provider.search(QStringLiteral("测试"), 5);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    qInstallMessageHandler(previous);

    const QString all = QString::fromUtf8(g_logMessages.join('\n'));
    QVERIFY(all.contains(QStringLiteral("token=***")));
    QVERIFY(!all.contains(QStringLiteral("SECRET123")));
    QVERIFY(!all.contains(QStringLiteral("SIGNED")));
}

void IntegrationTest::downloadLimitSettingClampsAndPersists()
{
    const int original = AppSettings::downloadLimitMb();
    AppSettings::setDownloadLimitMb(4096);
    QCOMPARE(AppSettings::downloadLimitMb(), 4096);
    // 误填 0 不能变成“所有下载立刻超限”，统一兜底到 512 MB。
    AppSettings::setDownloadLimitMb(0);
    QCOMPARE(AppSettings::downloadLimitMb(), 512);
    AppSettings::setDownloadLimitMb(original);
}

void IntegrationTest::videoPositionRestoredAfterSwitch()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过视频进度恢复测试");

    const QString mediaPath = m_tempDir.filePath(QStringLiteral("resume-source.mp4"));
    QFile::remove(mediaPath);
    QProcess generator;
    generator.start(MediaRemux::ffmpegExecutable(),
                     {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                      QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                      QStringLiteral("-t"), QStringLiteral("20"),
                      QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
                      QStringLiteral("-q:v"), QStringLiteral("5"),
                      QStringLiteral("-c:a"), QStringLiteral("aac"),
                      QStringLiteral("-b:a"), QStringLiteral("64k"),
                      QStringLiteral("-movflags"), QStringLiteral("+faststart"),
                      QStringLiteral("-f"), QStringLiteral("mp4"), mediaPath});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);

    PlaylistModel playlist;
    MediaItem first;
    first.type = MediaType::Video;
    first.title = QStringLiteral("resume-a");
    first.url = QUrl::fromLocalFile(mediaPath);
    first.online = false;
    MediaItem second = first;
    second.title = QStringLiteral("resume-b");
    // 两条必须是不同文件路径：同一 URL 时播放器可能复用旧位置，
    // 掩盖“新 source 从 0 开始却把旧位置当成恢复成功”的回归。
    const QString secondPath = m_tempDir.filePath(QStringLiteral("resume-source-b.mp4"));
    QFile::remove(secondPath);
    QVERIFY(QFile::copy(mediaPath, secondPath));
    second.url = QUrl::fromLocalFile(secondPath);
    playlist.addItem(first);
    playlist.addItem(second);

    PlaybackController player;
    player.setPlaylist(&playlist);
    player.playRow(0);

    // 等第一条真正播过 3 秒，确保保存的不是“片头忽略”区间。
    QElapsedTimer timer;
    timer.start();
    while (player.position() < 3000 && timer.elapsed() < 12000)
        QTest::qWait(100);
    if (player.position() < 3000)
        QSKIP("当前环境没有可用的多媒体后端，跳过视频进度恢复测试");
    const qint64 savedPosition = player.position();

    // 切到第二条再切回第一条，模拟用户手动换集。
    player.playRow(1);
    QTest::qWait(800);
    player.playRow(0);
    timer.restart();
    while (player.position() + 1500 < savedPosition && timer.elapsed() < 12000)
        QTest::qWait(100);

    QVERIFY2(player.position() + 1500 >= savedPosition,
             qPrintable(QStringLiteral("切回后位置 %1，保存位置 %2")
                            .arg(player.position())
                            .arg(savedPosition)));

    // 再模拟一次“恢复还没落地就切走”：切到第二条后立刻切回第一条，
    // 再在 seek 完成前切走；第一条的进度不能被片头 0 覆盖。
    player.playRow(1);
    QTest::qWait(800);
    player.playRow(0);
    QTest::qWait(200);
    player.playRow(1);
    QTest::qWait(300);
    QVERIFY2(playlist.itemAt(0).positionMs + 1500 >= savedPosition,
             qPrintable(QStringLiteral("快速切走后进度被覆盖成 %1，原位置 %2")
                            .arg(playlist.itemAt(0).positionMs)
                            .arg(savedPosition)));
    player.stop();
}

QTEST_GUILESS_MAIN(IntegrationTest)

#include "test_integration.moc"
