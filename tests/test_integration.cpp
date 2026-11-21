#include "download/DownloadTask.h"
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

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
    void redirectKeepsAuthorizationOnSameHost();
    void downloadReportsHttp404WithoutFile();
    void downloadRejectsHtmlMasqueradingAsMedia();
    void downloadFailsOnShortContentLength();
    void downloadStopsAtInjectedByteLimit();
    void downloadRejectsShortHlsSegmentRange();
    void streamProxyPreservesEncodedSegmentQuery();
    void searchTimeoutAbortsProvider();
    void maccmsAbortClosesInflightRequest();
    void externalAbortKillsProcessWithoutFinish();
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
    QCOMPARE(request.headers.value("x-source-session"), QByteArray("session-keep"));
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
