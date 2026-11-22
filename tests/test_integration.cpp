#include "download/DownloadManager.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
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
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
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
    void downloadFailsOnShortContentLength();
    void downloadStopsAtInjectedByteLimit();
    void downloadRejectsShortHlsSegmentRange();
    void downloadResumesFromPartialFileWithRange();
    void downloadCancelLeavesExistingFinalFile();
    void downloadManagerKeepsExistingFinalFile();
    void downloadManagerSameNameGetsDistinctFiles();
    void downloadHlsMergesAndRemuxes();
    void mediaRemuxConvertsTsToMp4();
    void downloadTaskAllowsVodWithoutEndList();
    void downloadTaskRequiresConsentWithoutVod();
    void downloadManagerForcesSnapshotOnlyAfterConsent();
    void streamProxyPreservesEncodedSegmentQuery();
    void streamProxyServesFirstSegmentOfLongPlaylist();
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

QTEST_GUILESS_MAIN(IntegrationTest)

#include "test_integration.moc"
