/// @file test_integration.cpp
/// @brief 跨模块集成测试：本地 HTTP 服务、HLS 播放列表改写、流代理、下载、
///        缓存、歌词与搜索源的真实交互（不依赖外网，除非用例显式联网）。
#include "cache/HlsCacheBuilder.h"
#include "cache/HlsCacheManifest.h"
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
#include "net/MediaPageResolver.h"
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
#include <QSaveFile>
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

int countFullRequests(const LocalHttpServer &server)
{
    int total = 0;
    for (const LocalHttpServer::Request &request : server.requests) {
        if (request.headers.value("range").isEmpty())
            ++total;
    }
    return total;
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
    void downloadTaskUsesMappedProxyDespitePublicOrigin();
    void hlsCacheBuilderUsesActualFallbackSource();
    void downloadResumesFromPartialFileWithRange();
    void downloadRestartsAfterUnsatisfiedRangeTotalMismatch();
    void downloadHlsExitCheckpointsAtCompletedSegmentBoundary();
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
    void downloadManagerKeepsPendingCleanupRecord();
    void downloadManagerKeepsRecordWhenPendingCacheCleanupFails();
    void downloadHlsMergesAndRemuxes();
    void hlsCacheBuilderProducesSuffixlessMp4();
    void hlsCacheBuilderDropsPreloadIndexForEncryptedMediaList();
    void hlsCacheBuilderResumesAfterExit();
    void hlsCacheBuilderPreservesResumeOnTrackFailure();
    void hlsCacheBuilderRejectsStaleCompletedTrack();
    void downloadManagerReusesPlaybackCacheThroughStaging();
    void hlsCacheBuilderMergesSingleFileAudio();
    void hlsCacheBuilderKeepsTrackPathsDistinct();
    void mediaCacheRemovesHashedFallback();
    void mediaCacheRemovalSkipsOtherPartial();
    void hlsCacheBuilderFollowsRealAppleStream();
    void lyricsServiceCachesOnlinePayload();
    void mediaRemuxConvertsTsToMp4();
    void downloadTaskAllowsVodWithoutEndList();
    void downloadTaskRequiresConsentWithoutVod();
    void downloadManagerForcesSnapshotOnlyAfterConsent();
    void streamProxyPreservesEncodedSegmentQuery();
    void streamProxyServesFirstSegmentOfLongPlaylist();
    void streamProxyDeduplicatesConcurrentHlsSegments();
    void streamProxyRetriesAfterFailedSegmentCache();
    void streamProxyReleaseCacheFileClosesEntry();
    void searchTimeoutAbortsProvider();
    void maccmsAbortClosesInflightRequest();
    void maccmsAcceptsDirectTsStream();
    void maccmsLateSuggestDoesNotPolluteNextSearch();
    void externalAbortKillsProcessWithoutFinish();
    void externalProviderScopesCookieToConfiguredOrigin();
    void externalProviderHonorsLimit();
    void configSearchEmitsPartialBeforeFinal();
    void searchJsonOverLimitFails();
    void responseLimitAbortsInflight();
    void responseLimitOnHttpErrorKeepsStatus();
    void requestLogsRedactQuerySecrets();
    void downloadLimitSettingClampsAndPersists();
    void videoPositionRestoredAfterSwitch();
    void pageResolutionDoesNotReviveStoppedOrSwitchedPlayback();

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

void IntegrationTest::downloadTaskUsesMappedProxyDespitePublicOrigin()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray segmentBytes(512, 'p');
    // 分片地址故意写成公网主机，实际请求由 mapper 改到本机代理；这正是
    // 播放缓存链路的形式。旧实现会用公网源站再校验代理地址并直接拒绝。
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "https://cdn.example.com/seg0.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist, segmentBytes](const LocalHttpServer::Request &request,
                                               QTcpSocket *socket) {
        if (request.path == QStringLiteral("/hls/mapped.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/segment.ts")) {
            LocalHttpServer::respond(socket, 200, segmentBytes, "video/mp2t");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("mapped-proxy.mp4"));
    QFile::remove(path);
    QFile::remove(m_tempDir.filePath(QStringLiteral("mapped-proxy.ts")));
    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/hls/mapped.m3u8")), path);
    QVERIFY(task != nullptr);
    task->setHlsSegmentUrlMapper(
        [url = server.url(QStringLiteral("/segment.ts"))](const QUrl &) { return url; });
    // 播放列表先按本机地址正常解析；解析完成后切换到公网源站，复现
    // “播放列表公网、分片被映射到本机代理”的校验路径。
    connect(task, &DownloadTask::playlistFingerprint, task, [task](const QString &) {
        task->setOriginUrl(QUrl(QStringLiteral("https://cdn.example.com/")));
    });

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 6000);
    QVERIFY2(failed.count() == 0, qPrintable(failed.isEmpty()
                                                 ? QString()
                                                 : failed.first().first().toString()));
    QCOMPARE(succeeded.count(), 1);
    const QString completed = succeeded.first().first().toString();
    QFile result(completed);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll(), segmentBytes);
    QVERIFY(server.requests.size() >= 2);
}

void IntegrationTest::hlsCacheBuilderUsesActualFallbackSource()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1.0,\n"
        "seg0.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QStringLiteral("/fallback/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
            return;
        }
        LocalHttpServer::respond(socket, 200, QByteArray(256, 'f'), "video/mp2t");
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("HLS 备用线路缓存测试");
    // 条目首选地址是分享页（HTML）；播放器实际换到了备用 m3u8。
    item.url = server.url(QStringLiteral("/share/page.html"));
    item.online = true;
    MediaCache::removeForItem(item);

    HttpClient http;
    HlsCacheBuilder builder(&http);
    // 不传实际地址时必须拒绝缓存 HTML；传备用 m3u8 后应启动。
    QVERIFY(!builder.start(item));
    QVERIFY(builder.start(item, server.url(QStringLiteral("/fallback/index.m3u8"))));
    builder.stopForExit();
    MediaCache::removeForItem(item);
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

void IntegrationTest::downloadRestartsAfterUnsatisfiedRangeTotalMismatch()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    const QByteArray full(800, 'r');
    server.handler = [full](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        const QByteArray range = request.headers.value("range");
        if (range == "bytes=1000-") {
            // 本地半成品比服务器当前资源还长：416 + 总长 800。
            // 继续保留这 1000 字节只会让后续每次重试都重复 416。
            LocalHttpServer::respond(socket, 416, {}, "text/plain", 0,
                                     "Content-Range: bytes */800\r\n");
            return;
        }
        LocalHttpServer::respond(socket, 200, full, "audio/mpeg", full.size());
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("resume-416.mp3"));
    const QString partial = path + QStringLiteral(".mvpdownload");
    QFile::remove(path);
    QFile::remove(partial);
    {
        QFile file(partial);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray(1000, 'x')) == 1000);
    }

    DownloadTask *task = DownloadTask::start(&http, server.url(QStringLiteral("/resume-416.mp3")), path);
    task->setResumeState(1000, 0, false);

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(succeeded.count(), 1);

    QFile result(path);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.size(), qint64(full.size()));
    QCOMPARE(result.readAll(), full);

    // 第一次带 Range 被 416 拒绝，第二次必须改成无 Range 的完整请求。
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(server.requests.first().headers.value("range"), QByteArray("bytes=1000-"));
    QVERIFY(!server.requests.at(1).headers.contains("range"));
}

void IntegrationTest::downloadHlsExitCheckpointsAtCompletedSegmentBoundary()
{
    const QByteArray seg0(65536, 'a');
    const QByteArray seg1(262144, 'b');
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

    LocalHttpServer server;
    QVERIFY(server.start());
    int seg1Requests = 0;
    server.handler = [&, playlist, seg0, seg1](const LocalHttpServer::Request &request,
                                               QTcpSocket *socket) {
        if (request.path == QStringLiteral("/exit/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
            return;
        }
        if (request.path == QStringLiteral("/exit/seg0.ts")) {
            LocalHttpServer::respond(socket, 200, seg0, "video/mp2t", seg0.size());
            return;
        }
        if (request.path == QStringLiteral("/exit/seg1.ts")) {
            ++seg1Requests;
            if (seg1Requests == 1) {
                // 第一片完整、第二片只发一个 chunk 并保持连接，制造“下到一半退出”。
                const QByteArray chunk = seg1.left(65536);
                QByteArray head =
                    "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n"
                    "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
                head += QByteArray::number(chunk.size(), 16).toUpper() + "\r\n";
                socket->write(head);
                socket->write(chunk);
                socket->write("\r\n");
                socket->flush();
                return;
            }
            LocalHttpServer::respond(socket, 200, seg1, "video/mp2t", seg1.size());
            return;
        }
        LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    HttpClient http;
    const QString path = m_tempDir.filePath(QStringLiteral("hls-exit.ts"));
    const QString partial = path + QStringLiteral(".mvpdownload");
    QFile::remove(path);
    QFile::remove(partial);

    DownloadTask *task =
        DownloadTask::start(&http, server.url(QStringLiteral("/exit/index.m3u8")), path);
    QSignalSpy checkpoint(task, &DownloadTask::checkpoint);
    QSignalSpy fingerprint(task, &DownloadTask::playlistFingerprint);
    QSignalSpy failed(task, &DownloadTask::failed);

    // 先等第一片完整结束（checkpoint 只在完整分片边界发出），再等第二片
    // 的第一个 chunk 让 progress 再增加一次。
    QTRY_VERIFY_WITH_TIMEOUT(checkpoint.count() >= 1, 5000);
    QVERIFY(fingerprint.count() > 0);
    const QString resumeFingerprint = fingerprint.last().at(0).toString();
    // 不能用 progress 次数判断第二片是否已经到数据：HLS 的 progress 首参是
    // “已完成分片数”，第一片结束和进入第二片都可能是同一个值，第二片的
    // 首个 chunk 也可能在 checkpoint 之后、这里取基准之前就到达。改成等
    // 暂存文件真实超过第一片长度，既证明半片已写入，也不依赖信号到达时机。
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(partial).size() > seg0.size(), 5000);
    task->stopForExit();
    QTest::qWait(100);

    // 文件里确实有第二片半片（stopForExit 已 flush 关闭），但检查点必须退回到
    // 第一片末尾；否则重启会把这个半片当成完整前缀，从第二片开头重复写入。
    QVERIFY(QFileInfo(partial).size() > seg0.size());
    QVERIFY(checkpoint.count() > 0);
    const QList<QVariant> lastCheckpoint = checkpoint.last();
    QCOMPARE(lastCheckpoint.at(0).toLongLong(), qint64(seg0.size()));
    QCOMPARE(lastCheckpoint.at(1).toInt(), 1);
    QCOMPARE(failed.count(), 0);

    // 按该检查点续传：openTargetFile 会截掉半片，第二片必须从头发送。
    DownloadTask *resumed =
        DownloadTask::start(&http, server.url(QStringLiteral("/exit/index.m3u8")), path);
    resumed->setResumeState(seg0.size(), 1, true);
    resumed->setResumeFingerprint(resumeFingerprint);
    QSignalSpy resumedSucceeded(resumed, &DownloadTask::succeeded);
    QSignalSpy resumedFailed(resumed, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(resumedSucceeded.count() + resumedFailed.count() > 0, 5000);
    QCOMPARE(resumedFailed.count(), 0);
    QCOMPARE(resumedSucceeded.count(), 1);
    QCOMPARE(seg1Requests, 2);

    QFile result(path);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll(), seg0 + seg1);
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
    // 上次失败/退出可能留下同名暂存文件；这次完整复制必须能替换它，
    // 不能被 Windows rename 的“目标已存在”卡住而永久失败。
    const QString stagingPath = [&manager, &id]() {
        for (const DownloadJob &job : manager.jobs()) {
            if (job.id == id)
                return job.stagingPath;
        }
        return QString();
    }();
    QVERIFY(!stagingPath.isEmpty());
    QFile staleStaging(stagingPath);
    QVERIFY(staleStaging.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(staleStaging.write("stale-staging"), qint64(13));
    staleStaging.close();
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

void IntegrationTest::downloadManagerKeepsPendingCleanupRecord()
{
    QDir().mkpath(StoragePaths::dataDir());
    const QString statePath = StoragePaths::dataFile(QStringLiteral("downloads.json"));
    QFile state(statePath);
    QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QJsonArray encoded;
    DownloadJob pending;
    pending.id = QStringLiteral("pending-cleanup");
    pending.item.type = MediaType::Music;
    pending.item.title = QStringLiteral("待清理缓存");
    pending.filePath = m_tempDir.filePath(QStringLiteral("pending-cleanup.mp3"));
    pending.status = DownloadJob::Status::Completed;
    // 在缓存根目录下建一个同名目录：removeCachePath() 的 QFile::remove 会
    // 失败并保持 cleanupCachePath，模拟播放器仍占用缓存、清理需要重试。
    pending.cleanupCachePath =
        MediaCache::rootDir() + QStringLiteral("/pending-cleanup/media");
    QVERIFY(QDir().mkpath(pending.cleanupCachePath));
    encoded.append(pending.toJson());
    for (int i = 0; i < 100; ++i) {
        DownloadJob job;
        job.id = QStringLiteral("completed-%1").arg(i);
        job.item.type = MediaType::Music;
        job.item.title = QStringLiteral("历史完成 %1").arg(i);
        job.filePath = m_tempDir.filePath(QStringLiteral("completed-%1.mp3").arg(i));
        job.status = DownloadJob::Status::Completed;
        encoded.append(job.toJson());
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), encoded);
    state.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    state.close();

    {
        HttpClient http;
        DownloadManager manager(&http);
        QCOMPARE(manager.jobs().size(), 100);
        bool keptPending = false;
        for (const DownloadJob &job : manager.jobs()) {
            if (job.id == pending.id) {
                keptPending = true;
                break;
            }
        }
        // 清理标记还没完成的记录不能被历史裁剪丢掉；否则对应播放缓存/歌词
        // 缓存永远没有记录可供 cleanupPendingCaches 继续清理。
        QVERIFY(keptPending);

        // “清除已完成”同样不能把清理失败、仍带标记的记录删掉，否则缓存清理
        // 会失去唯一的重试入口；普通已完成记录应被清空。
        manager.clearCompleted();
        QCOMPARE(manager.jobs().size(), 1);
        QCOMPARE(manager.jobs().constFirst().id, pending.id);
        QVERIFY(!manager.jobs().constFirst().cleanupCachePath.isEmpty());
        manager.shutdownForExit();
    }
    QFile::remove(statePath);
}

void IntegrationTest::downloadManagerKeepsRecordWhenPendingCacheCleanupFails()
{
    QDir().mkpath(StoragePaths::dataDir());
    const QString statePath = StoragePaths::dataFile(QStringLiteral("downloads.json"));
    QFile state(statePath);
    QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));

    DownloadJob pending;
    pending.id = QStringLiteral("pending-remove");
    pending.item.type = MediaType::Music;
    pending.item.title = QStringLiteral("待移除的缓存记录");
    pending.filePath = m_tempDir.filePath(QStringLiteral("pending-remove.mp3"));
    pending.status = DownloadJob::Status::Completed;
    pending.cleanupCachePath =
        MediaCache::rootDir() + QStringLiteral("/remove-job-pending/media");
    QVERIFY(QDir().mkpath(pending.cleanupCachePath));

    QFile downloaded(pending.filePath);
    QVERIFY(downloaded.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(downloaded.write("user-download") > 0);
    downloaded.close();

    QJsonArray encoded;
    encoded.append(pending.toJson());
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), encoded);
    state.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    state.close();

    {
        HttpClient http;
        DownloadManager manager(&http);
        QCOMPARE(manager.jobs().size(), 1);

        // 缓存路径仍是目录时 removeCachePath() 会失败：记录必须保留，
        // 否则 cleanupPendingCaches() 再也找不到这条待清理缓存。
        QVERIFY(!manager.removeJob(pending.id));
        QCOMPARE(manager.jobs().size(), 1);
        QVERIFY(!manager.jobs().constFirst().cleanupCachePath.isEmpty());
        QVERIFY(QFileInfo::exists(pending.filePath));

        // 解除占用后再次移除：缓存清理成功，记录才可以删除；用户下载目录
        // 里的成品文件始终不能被删。
        QVERIFY(QDir(pending.cleanupCachePath).removeRecursively());
        QVERIFY(manager.removeJob(pending.id));
        QVERIFY(manager.jobs().isEmpty());
        QVERIFY(QFileInfo::exists(pending.filePath));
        manager.shutdownForExit();
    }
    QFile::remove(pending.filePath);
    QFile::remove(statePath);
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

void IntegrationTest::hlsCacheBuilderDropsPreloadIndexForEncryptedMediaList()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray playlist =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:1.0,\n"
        "seg0.ts\n"
        "#EXT-X-ENDLIST\n";
    server.handler = [playlist](const LocalHttpServer::Request &request,
                                QTcpSocket *socket) {
        if (request.path == QStringLiteral("/encrypted-media/index.m3u8"))
            LocalHttpServer::respond(socket, 200, playlist,
                                     "application/vnd.apple.mpegurl");
        else
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("加密媒体列表索引清理测试");
    item.url = server.url(QStringLiteral("/encrypted-media/index.m3u8"));
    item.online = true;

    const QString base = MediaCache::filePathForItem(item);
    const QString journal = base + QStringLiteral(".preload.jsonl");
    QVERIFY(MediaCache::removeForItem(item));
    QVERIFY(QDir().mkpath(QFileInfo(base).absolutePath()));
    // 目录里有旧索引时，必须留下身份标记；否则 resolvedCachePath 会把
    // 非空目录当成路径冲突，builder.start() 会改选 " (2)" 目录。
    QVERIFY(MediaCache::writePartialIdentity(base, MediaCache::cacheIdentity(item)));
    QFile file(journal);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"i\":\"stale\",\"u\":\"http://example.com/seg.ts\","
               "\"p\":\"/tmp/stale\",\"o\":0,\"s\":1}\n");
    file.close();
    QVERIFY(QFileInfo::exists(journal));

    HttpClient http;
    HlsCacheBuilder builder(&http);
    QVERIFY(builder.start(item));
    // 源本身就是带 EXT-X-KEY 的媒体列表：必须立即丢弃旧分片索引，
    // 不能等下载任务因缺 KEY 失败后才由通用清理兜底。
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(journal), 5000);
    builder.stopForExit();
    QVERIFY(MediaCache::removeForItem(item));
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

void IntegrationTest::hlsCacheBuilderPreservesResumeOnTrackFailure()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过 HLS 失败续传测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString sourceTs = m_tempDir.filePath(QStringLiteral("hls-failure-source.ts"));
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

    bool failSecondSegment = true;
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
    server.handler = [&failSecondSegment, playlist, tsBytes, split](
                         const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QStringLiteral("/failure-cache/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/failure-cache/seg0.ts")) {
            LocalHttpServer::respond(socket, 200, tsBytes.left(split), "video/mp2t");
        } else if (request.path == QStringLiteral("/failure-cache/seg1.ts")) {
            if (failSecondSegment)
                LocalHttpServer::respond(socket, 500, QByteArrayLiteral("server error"),
                                         "text/plain");
            else
                LocalHttpServer::respond(socket, 200, tsBytes.mid(split), "video/mp2t");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("HLS 失败续传测试");
    item.url = server.url(QStringLiteral("/failure-cache/index.m3u8"));
    item.online = true;

    const QString base = MediaCache::filePathForItem(item);
    QVERIFY(MediaCache::removeForItem(item));

    HttpClient http;
    HlsCacheBuilder builder(&http);
    bool completed = false;
    bool failed = false;
    QString failureMessage;
    connect(&builder, &HlsCacheBuilder::completed, &builder,
            [&](const MediaItem &, const QString &) { completed = true; });
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
    QTRY_VERIFY_WITH_TIMEOUT(failed, 15000);
    QVERIFY2(!completed, qPrintable(failureMessage));
    // 失败不能把已经下完的第一片和续传状态一起清掉，否则重试只能从零开始。
    QVERIFY(QFileInfo::exists(base + QStringLiteral(".video.resume.json")));
    QVERIFY(partialBytes() >= split);
    QVERIFY(resumedSegments() >= 1);

    failSecondSegment = false;
    completed = false;
    failed = false;
    failureMessage.clear();
    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 30000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);

    int firstSegmentRequests = 0;
    for (const LocalHttpServer::Request &request : server.requests) {
        // 迁移旧轨道时会发一个 bytes=0-0 探测请求，它不是重新下载分片；
        // 只有不带 Range 的完整分片请求才说明续传点被丢弃。
        if (request.path == QStringLiteral("/failure-cache/seg0.ts")
            && request.headers.value("range").isEmpty())
            ++firstSegmentRequests;
    }
    // 第二次启动必须接着第一片之后下载，而不是整段重下。
    QCOMPARE(firstSegmentRequests, 1);
    QVERIFY(!QFileInfo::exists(base + QStringLiteral(".video.resume.json")));

    QVERIFY(MediaCache::removeForItem(item));
}

void IntegrationTest::hlsCacheBuilderRejectsStaleCompletedTrack()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过过期轨道复用测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString sourceTs = m_tempDir.filePath(QStringLiteral("hls-stale-source.ts"));
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
    int seg0Requests = 0;
    int seg1Requests = 0;
    server.handler = [&, playlist, tsBytes, split](const LocalHttpServer::Request &request,
                                                    QTcpSocket *socket) {
        if (request.path == QStringLiteral("/stale-cache/index.m3u8")) {
            LocalHttpServer::respond(socket, 200, playlist, "application/vnd.apple.mpegurl");
        } else if (request.path == QStringLiteral("/stale-cache/seg0.ts")) {
            ++seg0Requests;
            LocalHttpServer::respond(socket, 200, tsBytes.left(split), "video/mp2t");
        } else if (request.path == QStringLiteral("/stale-cache/seg1.ts")) {
            ++seg1Requests;
            LocalHttpServer::respond(socket, 200, tsBytes.mid(split), "video/mp2t");
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("过期完成轨道测试");
    item.url = server.url(QStringLiteral("/stale-cache/index.m3u8"));
    item.online = true;

    // 用与 HlsCacheBuilder 相同的严格解析重算当前列表指纹，供“指纹匹配但
    // 长度截断/完整”两个分支使用。
    const QUrl playlistUrl = item.url;
    const QString liveFingerprint =
        HlsCacheManifest::mediaFingerprint(playlist, playlistUrl, QStringList(), playlistUrl);
    QVERIFY(!liveFingerprint.isEmpty());

    const QString base = MediaCache::filePathForItem(item);
    MediaCache::removeForItem(item);
    QDir().mkpath(QFileInfo(base).absolutePath());
    // 非空目录且没有身份标记会被 builder 改选 " (2)"，伪造状态会落不到
    // 实际读取的目录；先登记身份，让 start() 命中这里。
    QVERIFY(MediaCache::writePartialIdentity(base, MediaCache::cacheIdentity(item)));

    // 伪造“上次只切了 1 片就标记完成”的轨道和续传状态；当前播放列表是 2 片。
    // 如果只按 completed + 文件存在复用，这段垃圾数据会被直接送去转封装。
    const QString staleTarget = base + QStringLiteral(".video");
    QFile stale(staleTarget);
    QVERIFY(stale.open(QIODevice::WriteOnly));
    QCOMPARE(stale.write(QByteArray(4096, 'x')), qint64(4096));
    stale.close();
    QJsonObject resume;
    resume.insert(QStringLiteral("version"), 1);
    resume.insert(QStringLiteral("targetPath"), staleTarget);
    resume.insert(QStringLiteral("bytesWritten"), qint64(4096));
    resume.insert(QStringLiteral("segmentIndex"), 1);
    resume.insert(QStringLiteral("segmentCount"), 1);
    resume.insert(QStringLiteral("fingerprint"), QStringLiteral("stale-fingerprint"));
    resume.insert(QStringLiteral("completed"), true);
    const QByteArray resumePayload = QJsonDocument(resume).toJson();
    QSaveFile state(staleTarget + QStringLiteral(".resume.json"));
    QVERIFY(state.open(QIODevice::WriteOnly));
    QCOMPARE(state.write(resumePayload), qint64(resumePayload.size()));
    QVERIFY(state.commit());

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
    // 分片数不一致必须重新下载，而不是把旧成品轨道当当前节目发布。
    QCOMPARE(seg0Requests, 1);
    QCOMPARE(seg1Requests, 1);

    QVERIFY(MediaCache::removeForItem(item));

    // 第二种损坏：分片数恰好一致，但成品轨道被截断。只检查 completed +
    // 文件存在会直接复用这段短文件，转封装必然失败；长度必须与续传记录一致。
    const QString truncatedBase = MediaCache::filePathForItem(item);
    const QString truncatedTarget = truncatedBase + QStringLiteral(".video");
    QDir().mkpath(QFileInfo(truncatedBase).absolutePath());
    QVERIFY(MediaCache::writePartialIdentity(truncatedBase, MediaCache::cacheIdentity(item)));
    QFile truncated(truncatedTarget);
    QVERIFY(truncated.open(QIODevice::WriteOnly));
    QCOMPARE(truncated.write(QByteArray(1024, 'x')), qint64(1024));
    truncated.close();
    QJsonObject truncatedResume = resume;
    truncatedResume.insert(QStringLiteral("targetPath"), truncatedTarget);
    truncatedResume.insert(QStringLiteral("bytesWritten"), qint64(4096));
    truncatedResume.insert(QStringLiteral("segmentIndex"), 2);
    truncatedResume.insert(QStringLiteral("segmentCount"), 2);
    // 指纹与当前列表一致，确保这个分支验证的是“文件长度不一致”，
    // 而不是被结构指纹检查提前拦下。
    truncatedResume.insert(QStringLiteral("fingerprint"), liveFingerprint);
    const QByteArray truncatedPayload = QJsonDocument(truncatedResume).toJson();
    QSaveFile truncatedState(truncatedTarget + QStringLiteral(".resume.json"));
    QVERIFY(truncatedState.open(QIODevice::WriteOnly));
    QCOMPARE(truncatedState.write(truncatedPayload), qint64(truncatedPayload.size()));
    QVERIFY(truncatedState.commit());

    seg0Requests = 0;
    seg1Requests = 0;
    completed = false;
    failed = false;
    completedPath.clear();
    failureMessage.clear();
    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 30000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(seg0Requests, 1);
    QCOMPARE(seg1Requests, 1);

    QVERIFY(MediaCache::removeForItem(item));

    // 第三种：分片数和成品文件长度都恰好一致，但指纹来自另一份分片结构。
    // 只比较分片数 + 文件长度会把这段旧轨道直接送去转封装；结构指纹必须
    // 让缓存重新请求分片。
    QDir().mkpath(QFileInfo(truncatedBase).absolutePath());
    QVERIFY(MediaCache::writePartialIdentity(truncatedBase, MediaCache::cacheIdentity(item)));
    QFile staleFull(truncatedTarget);
    QVERIFY(staleFull.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(staleFull.write(tsBytes), qint64(tsBytes.size()));
    staleFull.close();
    QJsonObject staleFullResume = resume;
    staleFullResume.insert(QStringLiteral("targetPath"), truncatedTarget);
    staleFullResume.insert(QStringLiteral("bytesWritten"), qint64(tsBytes.size()));
    staleFullResume.insert(QStringLiteral("segmentIndex"), 2);
    staleFullResume.insert(QStringLiteral("segmentCount"), 2);
    staleFullResume.insert(QStringLiteral("fingerprint"), QStringLiteral("stale-fingerprint"));
    const QByteArray staleFullPayload = QJsonDocument(staleFullResume).toJson();
    QSaveFile staleFullState(truncatedTarget + QStringLiteral(".resume.json"));
    QVERIFY(staleFullState.open(QIODevice::WriteOnly));
    QCOMPARE(staleFullState.write(staleFullPayload), qint64(staleFullPayload.size()));
    QVERIFY(staleFullState.commit());

    seg0Requests = 0;
    seg1Requests = 0;
    completed = false;
    failed = false;
    completedPath.clear();
    failureMessage.clear();
    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 30000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(seg0Requests, 1);
    QCOMPARE(seg1Requests, 1);

    QVERIFY(MediaCache::removeForItem(item));

    // 第四种：分片结构指纹和文件长度都匹配，必须直接复用成品轨道，不能
    // 再请求分片；否则新校验会把正常续传也误判成过期。
    QDir().mkpath(QFileInfo(truncatedBase).absolutePath());
    QVERIFY(MediaCache::writePartialIdentity(truncatedBase, MediaCache::cacheIdentity(item)));
    QFile currentFull(truncatedTarget);
    QVERIFY(currentFull.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(currentFull.write(tsBytes), qint64(tsBytes.size()));
    currentFull.close();
    QJsonObject currentResume = resume;
    currentResume.insert(QStringLiteral("targetPath"), truncatedTarget);
    currentResume.insert(QStringLiteral("bytesWritten"), qint64(tsBytes.size()));
    currentResume.insert(QStringLiteral("segmentIndex"), 2);
    currentResume.insert(QStringLiteral("segmentCount"), 2);
    currentResume.insert(QStringLiteral("fingerprint"), liveFingerprint);
    const QByteArray currentPayload = QJsonDocument(currentResume).toJson();
    QSaveFile currentState(truncatedTarget + QStringLiteral(".resume.json"));
    QVERIFY(currentState.open(QIODevice::WriteOnly));
    QCOMPARE(currentState.write(currentPayload), qint64(currentPayload.size()));
    QVERIFY(currentState.commit());

    seg0Requests = 0;
    seg1Requests = 0;
    completed = false;
    failed = false;
    completedPath.clear();
    failureMessage.clear();
    QVERIFY(builder.start(item));
    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 30000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QCOMPARE(seg0Requests, 0);
    QCOMPARE(seg1Requests, 0);

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

void IntegrationTest::hlsCacheBuilderKeepsTrackPathsDistinct()
{
    if (!MediaRemux::isAvailable())
        QSKIP("未找到 ffmpeg，跳过独立音轨目标路径测试");

    const QString ffmpeg = MediaRemux::ffmpegExecutable();
    const QString videoTs = m_tempDir.filePath(QStringLiteral("track-video.ts"));
    const QString audioTs = m_tempDir.filePath(QStringLiteral("track-audio.ts"));
    QFile::remove(videoTs);
    QFile::remove(audioTs);

    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                             QStringLiteral("-t"), QStringLiteral("1"),
                             QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                             QStringLiteral("-an"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), videoTs});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    generator.start(ffmpeg, {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                             QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-f"), QStringLiteral("lavfi"),
                             QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
                             QStringLiteral("-t"), QStringLiteral("1"),
                             QStringLiteral("-c:a"), QStringLiteral("mp2"),
                             QStringLiteral("-vn"),
                             QStringLiteral("-f"), QStringLiteral("mpegts"), audioTs});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);

    QFile videoFile(videoTs);
    QFile audioFile(audioTs);
    QVERIFY(videoFile.open(QIODevice::ReadOnly));
    QVERIFY(audioFile.open(QIODevice::ReadOnly));
    const QByteArray videoBody = videoFile.readAll();
    const QByteArray audioBody = audioFile.readAll();

    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray master =
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud1\",NAME=\"A\",DEFAULT=YES,"
        "URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=160x90,CODECS=\"mp2v\","
        "AUDIO=\"aud1\"\n"
        "video.m3u8\n";
    const QByteArray videoPlaylist =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1.0,\nvideo-seg.ts\n#EXT-X-ENDLIST\n";
    const QByteArray audioPlaylist =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1.0,\naudio-seg.ts\n#EXT-X-ENDLIST\n";
    server.handler = [videoBody, audioBody, master, videoPlaylist, audioPlaylist](
                         const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QStringLiteral("/tracks/master.m3u8"))
            LocalHttpServer::respond(socket, 200, master, "application/vnd.apple.mpegurl");
        else if (request.path == QStringLiteral("/tracks/video.m3u8"))
            LocalHttpServer::respond(socket, 200, videoPlaylist,
                                     "application/vnd.apple.mpegurl");
        else if (request.path == QStringLiteral("/tracks/audio.m3u8"))
            LocalHttpServer::respond(socket, 200, audioPlaylist,
                                     "application/vnd.apple.mpegurl");
        else if (request.path == QStringLiteral("/tracks/video-seg.ts"))
            LocalHttpServer::respond(socket, 200, videoBody, "video/mp2t", -1, {}, 1200);
        else if (request.path == QStringLiteral("/tracks/audio-seg.ts"))
            LocalHttpServer::respond(socket, 200, audioBody, "video/mp2t", -1, {}, 1200);
        else
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("双轨路径测试");
    item.url = server.url(QStringLiteral("/tracks/master.m3u8"));
    item.online = true;

    HttpClient http;
    HlsCacheBuilder builder(&http);
    const QUrl videoUrl = server.url(QStringLiteral("/tracks/video.m3u8"));
    const QUrl audioUrl = server.url(QStringLiteral("/tracks/audio.m3u8"));
    builder.setPlaybackTrackResolvers([videoUrl] { return videoUrl; },
                                      [audioUrl] { return audioUrl; });

    const QString base = MediaCache::filePathForItem(item);
    const QString videoState = base + QStringLiteral(".video.resume.json");
    const QString audioState = base + QStringLiteral(".audio.resume.json");
    // 模拟旧缺陷留下的状态：两条轨道都写在公共 media.ts。修复后这类状态必须
    // 被拒绝，否则再次启动仍会把双轨指到同一文件。
    QVERIFY(QDir().mkpath(QFileInfo(base).absolutePath()));
    // 目录非空但没有身份标记时 start() 会认为目录属于别的条目并改到 " (2)"，
    // 这里先登记身份，让旧续传状态落在真正的缓存目录里。
    QVERIFY(MediaCache::writePartialIdentity(base, MediaCache::cacheIdentity(item)));
    auto writeLegacyResume = [](const QString &path, const QString &target) {
        QJsonObject object;
        object.insert(QStringLiteral("version"), 1);
        object.insert(QStringLiteral("targetPath"), target);
        object.insert(QStringLiteral("bytesWritten"), 4096);
        object.insert(QStringLiteral("segmentIndex"), 1);
        object.insert(QStringLiteral("segmentCount"), 1);
        object.insert(QStringLiteral("fingerprint"), QStringLiteral("legacy"));
        object.insert(QStringLiteral("completed"), true);
        QFile file(path);
        return file.open(QIODevice::WriteOnly)
               && file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
    };
    QVERIFY(writeLegacyResume(videoState, base + QStringLiteral(".ts")));
    QVERIFY(writeLegacyResume(audioState, base + QStringLiteral(".ts")));

    // 旧缺陷还可能把分片索引指向公共 media.ts；文件本身存在时，播放代理会
    // 在修复后的新轨道下载完成前继续命中这段旧字节。
    const QString legacyTrack = base + QStringLiteral(".ts");
    const QString legacyMp4 = base + QStringLiteral(".mp4");
    QFile legacyBody(legacyTrack);
    QVERIFY(legacyBody.open(QIODevice::WriteOnly));
    QVERIFY(legacyBody.write(QByteArrayLiteral("legacy-media-bytes")) > 0);
    legacyBody.close();
    QFile legacyMp4Body(legacyMp4);
    QVERIFY(legacyMp4Body.open(QIODevice::WriteOnly));
    QVERIFY(legacyMp4Body.write(QByteArrayLiteral("legacy-mp4-bytes")) > 0);
    legacyMp4Body.close();
    const QUrl legacySegment = server.url(QStringLiteral("/tracks/video-seg.ts"));
    const QString identity = MediaCache::cacheIdentity(item);
    HlsPreloadEntry legacyEntry;
    legacyEntry.url = legacySegment;
    legacyEntry.filePath = legacyTrack;
    legacyEntry.offset = 0;
    legacyEntry.size = 8;
    legacyEntry.contentType = QByteArrayLiteral("video/mp2t");
    HlsPreloadIndex::instance().openJournal(base + QStringLiteral(".preload.jsonl"));
    HlsPreloadIndex::instance().record(identity, legacyEntry);
    HlsPreloadSlice staleSlice;
    QVERIFY(HlsPreloadIndex::instance().find(identity, legacySegment, QByteArray(),
                                             &staleSlice));

    bool completed = false;
    bool failed = false;
    QString failureMessage;
    connect(&builder, &HlsCacheBuilder::completed, &builder,
            [&](const MediaItem &, const QString &) { completed = true; });
    connect(&builder, &HlsCacheBuilder::failed, &builder,
            [&](const MediaItem &, const QString &message) {
                failed = true;
                failureMessage = message;
            });
    QVERIFY(builder.start(item));
    // start 会重新打开索引日志并清掉公共 media.ts 目标，旧切片不能继续命中。
    QVERIFY(!HlsPreloadIndex::instance().find(identity, legacySegment, QByteArray(),
                                              &staleSlice));

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(videoState) && QFileInfo::exists(audioState),
                             10000);
    auto targetPathOf = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QString();
        return QJsonDocument::fromJson(file.readAll()).object()
            .value(QStringLiteral("targetPath")).toString();
    };
    // 续传状态先在固定基名登记，分片列表确认后才改成 .ts；等两条都完成切换。
    QString videoTarget;
    QString audioTarget;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            videoTarget = targetPathOf(videoState);
            audioTarget = targetPathOf(audioState);
            return videoTarget.endsWith(QStringLiteral(".ts"))
                   && audioTarget.endsWith(QStringLiteral(".ts"))
                   // 旧状态里的公共 media.ts 必须已被新任务改写掉。
                   && videoTarget != base + QStringLiteral(".ts")
                   && audioTarget != base + QStringLiteral(".ts");
        }()),
        10000);
    // 两条轨道必须以各自基名落盘；若都退化成 media.ts，会并发写同一个文件。
    QCOMPARE(videoTarget, base + QStringLiteral(".video.ts"));
    QCOMPARE(audioTarget, base + QStringLiteral(".audio.ts"));
    QVERIFY(videoTarget != audioTarget);

    QTRY_VERIFY_WITH_TIMEOUT(completed || failed, 40000);
    QVERIFY2(!failed, qPrintable(failureMessage));
    QVERIFY(completed);
    QVERIFY(QFileInfo(MediaCache::completeFileForItem(item)).size() > 0);

    // 旧版本缺陷留下的公共 media.ts/.mp4 必须随轨道清理一起回收。
    builder.cancelAndRemove();
    QVERIFY(!builder.start(MediaItem()));
    QVERIFY(!QFileInfo::exists(legacyTrack));
    QVERIFY(!QFileInfo::exists(legacyMp4));
}

void IntegrationTest::mediaCacheRemovesHashedFallback()
{
    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("回退缓存清理");
    item.url = QUrl(QStringLiteral("https://example.com/watch?id=1"));
    item.online = true;

    const QString root = QDir::cleanPath(MediaCache::rootDir());
    const QString base = MediaCache::entryBaseName(item);
    QVERIFY(QDir().mkpath(root));
    // 占满 1000 个常规候选位，并给每个候选写入别的身份，逼 filePathForItem 走短哈希兜底。
    for (int index = 0; index < 1000; ++index) {
        const QString name = index <= 0
                                 ? base
                                 : QStringLiteral("%1 (%2)").arg(base).arg(index + 1);
        const QString body = root + QLatin1Char('/') + name + QStringLiteral("/media");
        QVERIFY(QDir().mkpath(QFileInfo(body).absolutePath()));
        QFile file(body);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("x", 1) == 1);
        file.close();
        QVERIFY(MediaCache::writeState(body, QStringLiteral("other-%1").arg(index), item.url,
                                      QByteArrayLiteral("video/mp4"), 1, true));
    }

    const QString fallback = MediaCache::filePathForItem(item);
    QVERIFY(fallback.contains(QStringLiteral("__")));
    QVERIFY(QDir().mkpath(QFileInfo(fallback).absolutePath()));
    QFile fallbackBody(fallback);
    QVERIFY(fallbackBody.open(QIODevice::WriteOnly));
    QVERIFY(fallbackBody.write("fallback", 8) == 8);
    fallbackBody.close();
    QVERIFY(MediaCache::writeState(fallback, MediaCache::cacheIdentity(item), item.url,
                                  QByteArrayLiteral("video/mp4"), 8, true));

    QVERIFY(MediaCache::removeForItem(item));
    QVERIFY(!QFileInfo::exists(fallback));
    QVERIFY(!QFileInfo::exists(MediaCache::metaPathForFile(fallback)));
    // 兜底路径之前的第一个候选属于别的条目，不能被误删。
    const QString first = root + QLatin1Char('/') + base + QStringLiteral("/media");
    QVERIFY(QFileInfo::exists(first));
    QVERIFY(QFileInfo::exists(MediaCache::metaPathForFile(first)));
}

void IntegrationTest::mediaCacheRemovalSkipsOtherPartial()
{
    MediaItem keep;
    keep.type = MediaType::Video;
    keep.title = QStringLiteral("同名半成品保护");
    keep.url = QUrl(QStringLiteral("https://example.com/watch?id=1"));
    keep.online = true;

    MediaItem remove;
    remove.type = MediaType::Video;
    remove.title = keep.title;
    remove.url = QUrl(QStringLiteral("https://example.com/watch?id=2"));
    remove.online = true;

    // 同一标题下先让另一条目占用候选 0 的半成品目录。
    const QString body = MediaCache::filePathForItem(keep);
    QVERIFY(QDir().mkpath(QFileInfo(body).absolutePath()));
    QFile file(body);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("partial", 7) == 7);
    file.close();
    QVERIFY(MediaCache::writePartialIdentity(body, MediaCache::cacheIdentity(keep)));

    // 删除另一条同名条目不能把这份半成品当成孤儿清掉。
    QVERIFY(MediaCache::removeForItem(remove));
    QVERIFY(QFileInfo::exists(body));
    QCOMPARE(MediaCache::readPartialIdentity(body), MediaCache::cacheIdentity(keep));
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

void IntegrationTest::streamProxyDeduplicatesConcurrentHlsSegments()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray payload(4096, 'h');
    server.handler = [payload](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QByteArray("/hls/shared-seg.ts")) {
            // 故意延迟完整响应，让播放器和后台缓存两个客户端在响应头之前
            // 同时到达代理，覆盖最容易各自回源的竞态窗口。
            LocalHttpServer::respond(socket, 200, payload, "video/mp2t", -1, {}, 250);
        } else {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
        }
    };

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl proxied = proxy.proxyUrlFor(server.url(QStringLiteral("/hls/shared-seg.ts")));
    QVERIFY(proxied.isValid());

    HttpClient first;
    HttpClient second;
    QNetworkReply *one = first.get(proxied);
    QNetworkReply *two = second.get(proxied);
    QVERIFY(one != nullptr);
    QVERIFY(two != nullptr);
    QSignalSpy firstFinished(one, &QNetworkReply::finished);
    QSignalSpy secondFinished(two, &QNetworkReply::finished);
    QTRY_VERIFY_WITH_TIMEOUT(firstFinished.count() > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(secondFinished.count() > 0, 5000);

    // 两个客户端共用同一个代理分片条目，上游只能被请求一次。
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().path, QByteArray("/hls/shared-seg.ts"));
    QVERIFY(HttpClient::isHttpSuccess(one));
    QVERIFY(HttpClient::isHttpSuccess(two));
    QCOMPARE(one->readAll(), payload);
    QCOMPARE(two->readAll(), payload);

    one->deleteLater();
    two->deleteLater();
    proxy.shutdown();
}

void IntegrationTest::streamProxyRetriesAfterFailedSegmentCache()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QByteArray payload(131072, 'f');
    const qint64 probeBytes = 16384;
    server.handler = [payload, probeBytes](const LocalHttpServer::Request &request,
                                           QTcpSocket *socket) {
        if (request.path != QByteArray("/hls/fail-seg.ts")) {
            LocalHttpServer::respond(socket, 404, {}, "text/plain");
            return;
        }
        const QByteArray range = request.headers.value("range");
        if (range.isEmpty()) {
            LocalHttpServer::respond(socket, 200, payload, "video/mp2t");
            return;
        }
        if (range == "bytes=0-" + QByteArray::number(probeBytes - 1)) {
            LocalHttpServer::respond(
                socket, 206, payload.left(int(probeBytes)), "video/mp2t", probeBytes,
                "Content-Range: bytes 0-" + QByteArray::number(probeBytes - 1) + "/"
                    + QByteArray::number(payload.size()) + "\r\n");
            return;
        }
        // 首片探测成功后让后续分片失败，制造一个“已标记 failed 但仍留在缓存表”
        // 的分片条目；下一次没有 Range 的播放请求必须能重新回源，而不是继续 502。
        LocalHttpServer::respond(socket, 500, {}, "text/plain");
    };

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl proxied = proxy.proxyUrlFor(server.url(QStringLiteral("/hls/fail-seg.ts")));
    QVERIFY(proxied.isValid());

    HttpClient first;
    QNetworkReply *failedReply = first.get(proxied, "bytes=0-");
    QVERIFY(failedReply != nullptr);
    QSignalSpy failedFinished(failedReply, &QNetworkReply::finished);
    QTRY_VERIFY_WITH_TIMEOUT(failedFinished.count() > 0, 5000);
    QCOMPARE(failedReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 502);
    failedReply->deleteLater();
    // 首次请求会拆成“探测 + 3 个后续分片”，不能按总请求数断言；只确认
    // 失败阶段没有额外发起无 Range 的整段回源。
    QCOMPARE(countFullRequests(server), 0);

    HttpClient second;
    QNetworkReply *retry = second.get(proxied);
    QVERIFY(retry != nullptr);
    QSignalSpy retryFinished(retry, &QNetworkReply::finished);
    QTRY_VERIFY_WITH_TIMEOUT(retryFinished.count() > 0, 5000);
    QVERIFY(HttpClient::isHttpSuccess(retry));
    QCOMPARE(retry->readAll(), payload);
    QCOMPARE(countFullRequests(server), 1);

    retry->deleteLater();
    proxy.shutdown();
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
    // replaceAll 会补回缺失的内置源，而内置源默认启用；本用例要验证
    // “单源 1 秒超时”是唯一出口，必须停用真实网络源，否则 searchFinished
    // 会被网易云等公网请求拖到数秒后，超时断言就失去意义。
    for (const SourceConfig &source : registry.sources()) {
        if (source.id != QStringLiteral("local_test") && source.enabled)
            QVERIFY(registry.setEnabled(source.id, false));
    }
    QCOMPARE(registry.enabledSources().size(), 1);

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
    bool finishedEmitted = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&finishedEmitted](const QString &, const QVector<MediaItem> &, int, const QString &) {
                finishedEmitted = true;
            });
    provider.search(QStringLiteral("测试"), 5);
    QTRY_COMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().path, QByteArray("/"));

    // 同一实例再次搜索：上一轮的首页请求必须被收掉，不能把取消回调
    // 当成新一轮的响应，也不能在第二轮刚开始时提前发 finished。
    provider.search(QStringLiteral("第二次"), 5);
    QTRY_COMPARE(server.requests.size(), 2);
    QVERIFY(!finishedEmitted);

    QElapsedTimer timer;
    timer.start();
    provider.abort();
    QTRY_VERIFY_WITH_TIMEOUT(server.disconnects >= 1, 2500);
    QVERIFY(timer.elapsed() < 2500);
}

void IntegrationTest::maccmsAcceptsDirectTsStream()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path == QByteArrayLiteral("/")) {
            LocalHttpServer::respond(socket, 200, QByteArrayLiteral("<html>ok</html>"),
                                    "text/html; charset=utf-8");
            return;
        }
        if (request.path.startsWith(QByteArrayLiteral("/index.php/ajax/suggest"))) {
            LocalHttpServer::respond(
                socket, 200,
                QByteArrayLiteral("{\"list\":[{\"id\":1,\"name\":\"直链测试\"}]}"),
                "application/json");
            return;
        }
        if (request.path == QByteArrayLiteral("/voddetail/1.html")) {
            LocalHttpServer::respond(
                socket, 200,
                QByteArrayLiteral("<a href=\"/vodplay/1-1-1.html\">第1集</a>"),
                "text/html; charset=utf-8");
            return;
        }
        if (request.path == QByteArrayLiteral("/vodplay/1-1-1.html")) {
            LocalHttpServer::respond(
                socket, 200,
                QByteArrayLiteral(
                    "<script>var player_aaaa = {\"encrypt\":0,\"from\":\"test\","
                    "\"url\":\"https://cdn.example.com/video/media.ts\"};</script>"),
                "text/html; charset=utf-8");
            return;
        }
        LocalHttpServer::respond(socket, 404, QByteArrayLiteral("missing"), "text/plain");
    };

    SourceConfig config;
    config.id = QStringLiteral("maccms_direct_ts");
    config.name = QStringLiteral("本地直链源");
    config.kind = QStringLiteral("builtin");
    config.provider = QStringLiteral("maccms_web");
    config.mediaType = MediaType::Video;
    config.urlTemplate = server.url(QStringLiteral("/")).toString();

    MaccmsWebProvider provider(config, nullptr);
    QVector<MediaItem> results;
    QString error;
    bool finished = false;
    connect(&provider, &SearchProviderBase::finished, this,
            [&finished, &results, &error](const QString &, const QVector<MediaItem> &items,
                                          int, const QString &message) {
                finished = true;
                results = items;
                error = message;
            });

    provider.search(QStringLiteral("直链测试"), 5);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().url, QUrl(QStringLiteral("https://cdn.example.com/video/media.ts")));
}

void IntegrationTest::maccmsLateSuggestDoesNotPolluteNextSearch()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    int suggestCount = 0;
    server.handler = [&suggestCount](const LocalHttpServer::Request &request,
                                     QTcpSocket *socket) {
        if (request.path == QByteArrayLiteral("/")) {
            LocalHttpServer::respond(socket, 200, QByteArrayLiteral("<html>ok</html>"),
                                    "text/html; charset=utf-8");
            return;
        }
        if (request.path.startsWith(QByteArrayLiteral("/index.php/ajax/suggest"))) {
            ++suggestCount;
            if (suggestCount == 1) {
                // 第一轮联想结果故意迟到，此时用户已经发起了第二轮搜索。
                LocalHttpServer::respond(
                    socket, 200,
                    QByteArrayLiteral("{\"list\":[{\"id\":7,\"name\":\"上一轮结果\"}]}"),
                    "application/json", -1, {}, 400);
            } else {
                LocalHttpServer::respond(socket, 200,
                                        QByteArrayLiteral("{\"list\":[]}"),
                                        "application/json");
            }
            return;
        }
        LocalHttpServer::respond(socket, 404, QByteArrayLiteral("missing"), "text/plain");
    };

    SourceConfig config;
    config.id = QStringLiteral("maccms_late_suggest");
    config.name = QStringLiteral("本地延迟源");
    config.kind = QStringLiteral("builtin");
    config.provider = QStringLiteral("maccms_web");
    config.mediaType = MediaType::Video;
    config.urlTemplate = server.url(QStringLiteral("/")).toString();

    MaccmsWebProvider provider(config, nullptr);
    int finishedCount = 0;
    QVector<MediaItem> results;
    connect(&provider, &SearchProviderBase::finished, this,
            [&finishedCount, &results](const QString &, const QVector<MediaItem> &items,
                                       int, const QString &) {
                ++finishedCount;
                results = items;
            });

    provider.search(QStringLiteral("第一次"), 5);
    QTRY_COMPARE_WITH_TIMEOUT(suggestCount, 1, 3000);

    // 第一轮联想响应还在延迟中，立刻发起第二轮；旧候选不能进入第二轮。
    provider.search(QStringLiteral("第二次"), 5);
    QTRY_VERIFY_WITH_TIMEOUT(finishedCount >= 1, 3000);
    QTest::qWait(600);

    int detailRequests = 0;
    for (const auto &request : server.requests) {
        if (request.path.startsWith(QByteArrayLiteral("/voddetail/")))
            ++detailRequests;
    }
    QCOMPARE(detailRequests, 0);
    QCOMPARE(results.size(), 0);
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

void IntegrationTest::configSearchEmitsPartialBeforeFinal()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.handler = [](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path.startsWith("/search")) {
            // 搜索页没有直链，必须逐条请求详情；第 1 条快、第 2 条慢，
            // 用来验证慢详情不会把已经解析出的第 1 条扣到最后才显示。
            LocalHttpServer::respond(
                socket, 200,
                QByteArrayLiteral("{\"items\":[{\"id\":\"1\",\"title\":\"第一首\"},"
                                  "{\"id\":\"2\",\"title\":\"第二首\"}]}"),
                "application/json");
            return;
        }
        if (request.path.startsWith("/detail/1")) {
            LocalHttpServer::respond(
                socket, 200,
                QByteArrayLiteral("{\"url\":\"http://example.com/one.mp3\"}"),
                "application/json", -1, {}, 120);
            return;
        }
        if (request.path.startsWith("/detail/2")) {
            LocalHttpServer::respond(
                socket, 200,
                QByteArrayLiteral("{\"url\":\"http://example.com/two.mp3\"}"),
                "application/json", -1, {}, 700);
            return;
        }
        LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    SourceConfig config = makeSource(QUrl(QStringLiteral("http://127.0.0.1:%1/search")
                                              .arg(server.serverPort())));
    // QUrl::toString() 会把模板花括号编码成 %7Bkeyword%7D，renderTemplate 就替换不到；
    // 这里直接给未编码的模板字符串，和真实源配置的保存形式一致。
    config.urlTemplate = QStringLiteral("http://127.0.0.1:%1/search?q={keyword}")
                             .arg(server.serverPort());
    config.fields.insert(QStringLiteral("id"), QStringLiteral("id"));
    config.detailUrlTemplate = QStringLiteral("http://127.0.0.1:%1/detail/{id}")
                                   .arg(server.serverPort());
    config.detailFields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));

    HttpClient http;
    ConfigSearchProvider provider(config, &http);

    QVector<int> progressSizes;
    QVector<MediaItem> finalItems;
    bool finished = false;
    connect(&provider, &SearchProviderBase::itemsReady, this,
            [&progressSizes](const QString &, const QVector<MediaItem> &items, int) {
                progressSizes.append(items.size());
            });
    connect(&provider, &SearchProviderBase::finished, this,
            [&finished, &finalItems](const QString &, const QVector<MediaItem> &items, int,
                                    const QString &) {
                finalItems = items;
                finished = true;
            });

    provider.search(QStringLiteral("测试"), 5);

    // 第一条详情 120ms 完成，此时整源还没结束，必须已经能看到 1 条。
    QTRY_VERIFY_WITH_TIMEOUT(progressSizes.contains(1), 2500);
    QVERIFY(!finished);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 3500);
    QCOMPARE(finalItems.size(), 2);
    QCOMPARE(progressSizes.last(), 2);
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

/// 网页播放页解析是异步的：用户在中途按停止或切到另一个列表后，旧回调
/// 不能再用已经失效的行号把播放重新拉起来。这里用延迟响应稳定地制造
/// “解析未返回就切走”的时序，而不是依赖本机网络快慢。
void IntegrationTest::pageResolutionDoesNotReviveStoppedOrSwitchedPlayback()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QUrl mediaUrl = server.url(QStringLiteral("/media.m3u8"));
    const QString html = QStringLiteral("<script>const vid = '%1';</script>")
                             .arg(mediaUrl.toString());
    server.handler = [html](const LocalHttpServer::Request &request, QTcpSocket *socket) {
        if (request.path.startsWith("/play")) {
            LocalHttpServer::respond(socket, 200, html.toUtf8(), "text/html", -1, {}, 300);
            return;
        }
        LocalHttpServer::respond(socket, 404, {}, "text/plain");
    };

    auto videoItem = [&](const QString &title, const QString &query) {
        MediaItem item;
        item.type = MediaType::Video;
        item.title = title;
        item.url = server.url(QStringLiteral("/play?%1").arg(query));
        item.sourceOrigin = HeaderPolicy::originOnly(item.url);
        item.online = true;
        item.sourceId = QStringLiteral("link");
        return item;
    };

    PlaylistModel videoOne;
    videoOne.addItem(videoItem(QStringLiteral("page-one"), QStringLiteral("id=1")));
    PlaylistModel videoTwo;
    videoTwo.addItem(videoItem(QStringLiteral("page-two"), QStringLiteral("id=2")));
    PlaylistModel music;
    MediaItem song;
    song.type = MediaType::Music;
    song.title = QStringLiteral("song");
    song.url = server.url(QStringLiteral("/track.mp3"));
    song.sourceOrigin = HeaderPolicy::originOnly(song.url);
    song.online = true;
    music.addItem(song);

    HttpClient http;
    MediaPageResolver resolver(&http);
    PlaybackController player;
    player.setPageMediaResolver(&resolver);

    // 场景一：解析未返回时按下停止，回调不能把播放重新拉起来。
    player.setPlaylist(&videoOne);
    player.playRow(0);
    QCOMPARE(player.currentRow(), -1);
    QTest::qWait(40);
    player.stop();
    QTest::qWait(500);
    QCOMPARE(player.currentRow(), -1);
    QCOMPARE(player.activeUpstreamUrl(), QUrl());

    // 场景二：解析未返回时切到音乐列表，旧行号不能在新列表上开播。
    player.setPlaylist(&videoTwo);
    player.playRow(0);
    QCOMPARE(player.currentRow(), -1);
    QTest::qWait(40);
    player.setPlaylist(&music);
    QTest::qWait(500);
    QCOMPARE(player.currentRow(), -1);
    QCOMPARE(player.activeUpstreamUrl(), QUrl());

    // 作废旧回调整体不应影响后续正常点播。
    player.playRow(0);
    QCOMPARE(player.currentRow(), 0);
    player.stop();
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
