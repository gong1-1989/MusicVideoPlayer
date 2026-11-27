/// @file test_core_logic.cpp
/// @brief 核心纯逻辑测试：歌词解析、频谱数学、媒体身份/缓存、网址映射、
///        搜索源校验、播放列表模型与下载任务状态机。
#include "lyrics/LrcParser.h"
#include "audio/SpectrumMath.h"
#include "cache/MediaCache.h"
#include "cache/HlsCacheManifest.h"
#include "cache/HlsPreloadIndex.h"
#include "cache/HlsPreloadMigrator.h"
#include "cache/LyricsCache.h"
#include "core/PlaybackController.h"
#include "core/PlaylistModel.h"
#include "download/DownloadTask.h"
#include "media/MediaItem.h"
#include "net/ConfigMapper.h"
#include "net/HttpClient.h"
#include "net/MediaPageResolver.h"
#include "net/SourceConfig.h"
#include "net/SourceProbe.h"
#include "net/SourceRegistry.h"
#include "net/SearchCoordinator.h"
#include "net/SearchRanker.h"
#include "settings/AppShortcuts.h"
#include "stream/StreamProxy.h"
#include "stream/HlsPlaylist.h"
#include "stream/HlsSeekWindow.h"
#include "stream/StreamCacheStore.h"
#include "stream/StreamRange.h"
#include "net/NetLimits.h"
#include "stream/StreamTokenRegistry.h"
#include "util/Aes128Cbc.h"
#include "util/HeaderPolicy.h"
#include "util/HtmlText.h"
#include "util/MediaExtensions.h"
#include "util/NetworkAddressPolicy.h"
#include "util/PageMediaExtractor.h"
#include "util/PlaylistSniffer.h"
#include "util/SecretStore.h"
#include "util/TimeFormat.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

#include <cmath>
#include <functional>
#include <limits>

namespace {

QByteArray proxyTestBody(int size = 1000)
{
    QByteArray body;
    body.resize(size);
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
                                          "Content-Type: " + contentType + "\r\n"
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
    QByteArray contentType = QByteArrayLiteral("audio/mpeg");
};

/// 模拟 HLS 分片首片探测成功，但后续多连接 Range 分片返回 500 的 CDN。
/// 用于覆盖 failRangeEntry 通知已挂载播放器客户端时的同步断连重入。
class RangePartFailServer : public QTcpServer
{
public:
    explicit RangePartFailServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    const int headEnd = pending.indexOf("\r\n\r\n");
                    if (headEnd < 0) {
                        socket->setProperty("pending", pending);
                        return;
                    }
                    socket->setProperty("pending", QByteArray());

                    QByteArray range;
                    for (const QByteArray &line : pending.left(headEnd).split('\n')) {
                        if (line.toLower().startsWith("range:"))
                            range = line.mid(6).trimmed();
                    }
                    ++requestCount;

                    if (range == QByteArrayLiteral("bytes=0-16383")) {
                        QByteArray response =
                            "HTTP/1.1 206 Partial Content\r\n"
                            "Content-Type: video/mp2t\r\n"
                            "Accept-Ranges: bytes\r\n"
                            "Content-Range: bytes 0-16383/185227\r\n"
                            "Content-Length: 16384\r\n"
                            "Connection: close\r\n\r\n";
                        response += body.left(16384);
                        socket->write(response);
                    } else {
                        socket->write("HTTP/1.1 500 Internal Server Error\r\n"
                                      "Content-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                    }
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body = proxyTestBody(185227);
    int requestCount = 0;
};

/// 模拟整段 200 响应传到一半被 CDN 重置连接，但支持从断点发 Range 续传。
/// 这是“播放约两分钟后 Demuxing failed”的回归源。
class TruncatingRangeServer : public QTcpServer
{
public:
    explicit TruncatingRangeServer(QObject *parent = nullptr)
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
                    qint64 start = 0;
                    for (const QByteArray &line : pending.left(headEnd).split('\n')) {
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const QByteArray spec = line.mid(6).trimmed();
                        if (!spec.startsWith("bytes="))
                            continue;
                        const int dash = spec.indexOf('-', 6);
                        if (dash > 6)
                            start = spec.mid(6, dash - 6).toLongLong();
                    }
                    ranges.append(start);
                    socket->setProperty("responded", true);

                    QByteArray head;
                    QByteArray part;
                    if (requestCount == 1) {
                        // 首次故意只发前 300 字节，然后 RST。
                        if (partialInitial) {
                            head = "HTTP/1.1 206 Partial Content\r\n"
                                   "Content-Type: audio/mpeg\r\n"
                                   "Content-Range: bytes 0-"
                                   + QByteArray::number(body.size() - 1) + '/'
                                   + QByteArray::number(body.size())
                                   + "\r\nContent-Length: "
                                   + QByteArray::number(body.size())
                                   + "\r\nConnection: close\r\n\r\n";
                        } else {
                            head = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: audio/mpeg\r\n"
                                   "Content-Length: "
                                   + QByteArray::number(body.size())
                                   + "\r\nConnection: close\r\n\r\n";
                        }
                        part = body.left(300);
                    } else {
                        const QByteArray tail = body.mid(int(start));
                        head = "HTTP/1.1 206 Partial Content\r\n"
                               "Content-Type: audio/mpeg\r\n"
                               "Content-Range: bytes "
                               + QByteArray::number(start) + '-'
                               + QByteArray::number(body.size() - 1) + '/'
                               + QByteArray::number(body.size())
                               + "\r\nContent-Length: "
                               + QByteArray::number(tail.size())
                               + "\r\nConnection: close\r\n\r\n";
                        part = tail;
                    }
                    socket->write(head + part);
                    socket->flush();
                    const bool first = requestCount == 1;
                    QTimer::singleShot(first ? 60 : 0, socket, [socket, first] {
                        if (first)
                            socket->abort();
                        else
                            socket->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    int requestCount = 0;
    bool partialInitial = false;
    QList<qint64> ranges;
};

/// 模拟真正支持 206 的 HLS 分片源站。代理对 `bytes=0-` 的 .ts 请求会先
/// 用 16KB 探测总长，再并发拉剩余分段；这里逐条记录 Range，供测试确认
/// 多连接路径确实被走到、而不是退化在上游单路透传。
class RangeSupportingServer : public QTcpServer
{
public:
    explicit RangeSupportingServer(QObject *parent = nullptr)
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
                    qint64 start = 0;
                    qint64 end = body.size() - 1;
                    for (const QByteArray &line : pending.left(headEnd).split('\n')) {
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const QByteArray spec = line.mid(6).trimmed();
                        if (!spec.startsWith("bytes="))
                            continue;
                        const QByteArray value = spec.mid(6);
                        const int dash = value.indexOf('-');
                        if (dash <= 0)
                            continue;
                        start = value.left(dash).toLongLong();
                        const QByteArray tail = value.mid(dash + 1);
                        if (!tail.isEmpty())
                            end = tail.toLongLong();
                    }
                    ranges.append(qMakePair(start, end));

                    socket->setProperty("responded", true);
                    if (start < 0 || start >= body.size()) {
                        const QByteArray response = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                                    "Content-Range: bytes */"
                                                    + QByteArray::number(body.size())
                                                    + "\r\nContent-Length: 0\r\n"
                                                      "Connection: close\r\n\r\n";
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    end = qMin(end, qint64(body.size() - 1));
                    const QByteArray part = body.mid(int(start), int(end - start + 1));
                    QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                          "Content-Type: video/mp2t\r\n"
                                          "Accept-Ranges: bytes\r\n"
                                          "Content-Range: bytes "
                                          + QByteArray::number(start) + '-'
                                          + QByteArray::number(end) + '/'
                                          + QByteArray::number(body.size())
                                          + "\r\nContent-Length: "
                                          + QByteArray::number(part.size())
                                          + "\r\nConnection: close\r\n\r\n" + part;
                    socket->write(response);
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    int requestCount = 0;
    QList<QPair<qint64, qint64>> ranges;
};

/// 播放列表 + 单个普通分片；用来验证子分片 token 继承根列表的条目身份后，
/// 已缓存分片不再回源。
class PreloadPlaylistServer : public QTcpServer
{
public:
    explicit PreloadPlaylistServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        body = proxyTestBody(4096);
        playlist = QByteArrayLiteral("#EXTM3U\n"
                                     "#EXT-X-VERSION:3\n"
                                     "#EXT-X-TARGETDURATION:2\n"
                                     "#EXTINF:2,\n"
                                     "seg.ts\n"
                                     "#EXT-X-ENDLIST\n");
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
                    socket->setProperty("responded", true);
                    ++requestCount;
                    const QByteArray path =
                        pending.left(headEnd).split('\n').value(0).trimmed().split(' ').value(1);
                    QByteArray response;
                    if (path.endsWith(".m3u8")) {
                        response = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/vnd.apple.mpegurl\r\n"
                                   "Content-Length: " + QByteArray::number(playlist.size())
                                   + "\r\nConnection: close\r\n\r\n" + playlist;
                    } else {
                        response = "HTTP/1.1 206 Partial Content\r\n"
                                   "Content-Type: video/mp2t\r\n"
                                   "Content-Range: bytes 0-" + QByteArray::number(body.size() - 1)
                                   + "/" + QByteArray::number(body.size())
                                   + "\r\nContent-Length: " + QByteArray::number(body.size())
                                   + "\r\nConnection: close\r\n\r\n" + body;
                    }
                    socket->write(response);
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    QByteArray playlist;
    int requestCount = 0;
};

/// 单文件 BYTERANGE HLS：所有分片都指向同一个 main.mp4。
/// 用来验证播放器请求大文件的局部 Range 时，StreamProxy 不会缓存整段。
class SingleFileHlsServer : public QTcpServer
{
public:
    explicit SingleFileHlsServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        body = proxyTestBody(400'000);
        playlist = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:7\n"
            "#EXT-X-TARGETDURATION:2\n"
            "#EXTINF:2,\n"
            "#EXT-X-BYTERANGE:200000@0\n"
            "main.mp4\n"
            "#EXTINF:2,\n"
            "#EXT-X-BYTERANGE:200000@200000\n"
            "main.mp4\n"
            "#EXT-X-ENDLIST\n");

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

                    const QByteArray head = pending.left(headEnd);
                    const QByteArray path =
                        head.split('\n').value(0).trimmed().split(' ').value(1);
                    qint64 start = 0;
                    qint64 end = body.size() - 1;
                    for (const QByteArray &line : head.split('\n')) {
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const QByteArray spec = line.mid(6).trimmed();
                        if (!spec.startsWith("bytes="))
                            continue;
                        const QByteArray value = spec.mid(6);
                        const int dash = value.indexOf('-');
                        if (dash <= 0)
                            continue;
                        start = value.left(dash).toLongLong();
                        const QByteArray tail = value.mid(dash + 1);
                        if (!tail.isEmpty())
                            end = tail.toLongLong();
                    }
                    socket->setProperty("responded", true);

                    if (path == "/hls/index.m3u8") {
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: application/vnd.apple.mpegurl\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(playlist.size())
                                              + "\r\nConnection: close\r\n\r\n" + playlist;
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }
                    if (path != "/hls/main.mp4") {
                        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    end = qMin(end, qint64(body.size() - 1));
                    const QByteArray part = body.mid(int(start), int(end - start + 1));
                    QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                          "Content-Type: video/mp4\r\n"
                                          "Content-Range: bytes "
                                          + QByteArray::number(start) + '-'
                                          + QByteArray::number(end) + '/'
                                          + QByteArray::number(body.size())
                                          + "\r\nContent-Length: "
                                          + QByteArray::number(part.size())
                                          + "\r\nConnection: close\r\n\r\n" + part;
                    socket->write(response);
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    QByteArray playlist;
};

/// 缓存淘汰回归源：播放列表有 10 个分片，预取请求只回 206 头和 1KB 正文后挂住；
/// 普通媒体请求返回小文件，用来把缓存条目数推过 24 条上限。
class PrefetchEvictionServer : public QTcpServer
{
public:
    explicit PrefetchEvictionServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        playlist = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:5\n"
            "#EXTINF:5,\n"
            "slow0.ts\n"
            "#EXTINF:5,\n"
            "slow1.ts\n"
            "#EXTINF:5,\n"
            "slow2.ts\n"
            "#EXTINF:5,\n"
            "slow3.ts\n"
            "#EXTINF:5,\n"
            "slow4.ts\n"
            "#EXTINF:5,\n"
            "slow5.ts\n"
            "#EXTINF:5,\n"
            "slow6.ts\n"
            "#EXTINF:5,\n"
            "slow7.ts\n"
            "#EXTINF:5,\n"
            "slow8.ts\n"
            "#EXTINF:5,\n"
            "slow9.ts\n"
            "#EXT-X-ENDLIST\n");

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

                    const QByteArray requestLine =
                        pending.left(headEnd).split('\n').value(0).trimmed();
                    const QByteArray path = requestLine.split(' ').value(1);
                    socket->setProperty("responded", true);

                    if (path == "/hls/index.m3u8") {
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: application/vnd.apple.mpegurl\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(playlist.size())
                                              + "\r\nConnection: close\r\n\r\n" + playlist;
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    if (path.startsWith("/hls/slow")) {
                        const QString name = QString::fromUtf8(path.mid(5));
                        if (!slowRequests.contains(name))
                            slowRequests.append(name);
                        // 头声明 16KB，只发 1KB 后保持连接：rangeParts 里会有数据，
                        // 但该分片永远不会完成，模拟“预取进行中被缓存淘汰”。
                        QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                              "Content-Type: video/mp2t\r\n"
                                              "Content-Range: bytes 0-16383/200000\r\n"
                                              "Content-Length: 16384\r\n\r\n";
                        socket->write(response + QByteArray(1024, 's'));
                        return;
                    }

                    if (path.startsWith("/media/")) {
                        ++mediaRequests;
                        const QByteArray body(1000, 'm');
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: audio/mpeg\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(body.size())
                                              + "\r\nConnection: close\r\n\r\n" + body;
                        socket->write(response);
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                  "Connection: close\r\n\r\n");
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray playlist;
    QStringList slowRequests;
    int mediaRequests = 0;
};

/// 本地 HLS 下载源：主播放列表带 EXT-X-MAP 的 BYTERANGE，分片支持 206。
/// 用来验证 DownloadTask 的 init 段范围、分片长度校验和 .mp4→.ts 切换。
class HlsDownloadServer : public QTcpServer
{
public:
    explicit HlsDownloadServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        initBody = QByteArrayLiteral("INIT-SEGMENT-BYTES-0123456789");
        segment0 = proxyTestBody(100);
        segment1 = proxyTestBody(100);
        playlist = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:7\n"
            "#EXT-X-TARGETDURATION:1\n"
            "#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"10@0\"\n"
            "#EXTINF:1,\n"
            "seg0.ts\n"
            "#EXTINF:1,\n"
            "seg1.ts\n"
            "#EXT-X-ENDLIST\n");

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

                    const QByteArray head = pending.left(headEnd);
                    const QByteArray requestLine = head.split('\n').value(0).trimmed();
                    const QByteArray path = requestLine.split(' ').value(1);
                    qint64 start = 0;
                    qint64 end = -1;
                    bool hasRange = false;
                    for (const QByteArray &line : head.split('\n')) {
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const QByteArray value = line.mid(6).trimmed();
                        if (!value.startsWith("bytes="))
                            continue;
                        const int dash = value.indexOf('-');
                        if (dash <= 6)
                            continue;
                        start = value.mid(6, dash - 6).toLongLong();
                        const QByteArray tail = value.mid(dash + 1);
                        if (!tail.isEmpty())
                            end = tail.toLongLong();
                        hasRange = true;
                    }

                    QByteArray content;
                    QByteArray contentType;
                    if (path == "/index.m3u8") {
                        content = playlist;
                        contentType = "application/vnd.apple.mpegurl";
                    } else if (path == "/init.mp4") {
                        content = initBody;
                        contentType = "video/mp4";
                        initRangeStart = start;
                        initRangeEnd = end;
                        initRangeReceived = hasRange;
                    } else if (path == "/seg0.ts") {
                        content = segment0;
                        contentType = "video/mp2t";
                    } else if (path == "/seg1.ts") {
                        content = segment1;
                        contentType = "video/mp2t";
                    } else {
                        socket->setProperty("responded", true);
                        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    if (hasRange) {
                        end = (end < 0 || end >= content.size()) ? content.size() - 1 : end;
                        if (start < 0 || start > end) {
                            socket->setProperty("responded", true);
                            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\n"
                                          "Content-Range: bytes */"
                                          + QByteArray::number(content.size())
                                          + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                            return;
                        }
                        const QByteArray part = content.mid(int(start), int(end - start + 1));
                        QByteArray response = "HTTP/1.1 206 Partial Content\r\n"
                                              "Content-Type: " + contentType + "\r\n"
                                              "Content-Range: bytes "
                                              + QByteArray::number(start) + '-'
                                              + QByteArray::number(end) + '/'
                                              + QByteArray::number(content.size())
                                              + "\r\nContent-Length: "
                                              + QByteArray::number(part.size())
                                              + "\r\nConnection: close\r\n\r\n" + part;
                        socket->setProperty("responded", true);
                        socket->write(response);
                    } else {
                        QByteArray response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: " + contentType + "\r\n"
                                              "Content-Length: "
                                              + QByteArray::number(content.size())
                                              + "\r\nConnection: close\r\n\r\n" + content;
                        socket->setProperty("responded", true);
                        socket->write(response);
                    }
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray playlist;
    QByteArray initBody;
    QByteArray segment0;
    QByteArray segment1;
    bool initRangeReceived = false;
    qint64 initRangeStart = -1;
    qint64 initRangeEnd = -1;
};

/// 本地 AES-128 HLS 源：主列表只写 SESSION-KEY，媒体列表中途换 key。
/// 用来验证“从站点自动取 key + 默认 IV + 显式 IV + 解密后合并”。
class AesHlsDownloadServer : public QTcpServer
{
public:
    explicit AesHlsDownloadServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        key1 = QByteArray::fromHex("000102030405060708090A0B0C0D0E0F");
        key2 = QByteArray::fromHex("0F0E0D0C0B0A09080706050403020100");
        segment0 = QByteArray::fromHex(
            "A7AF14C58597562145AE04D5D25CF1A05A512B545589BB14B8875F9A4128B2F4");
        segment1 = QByteArray::fromHex(
            "5DADE9B6B556DF6104567947F89AA058F7E6D0CB9AB73C69D796A3BE2EB27B55");
        plain0 = QByteArrayLiteral("0123456789ABCDEF");
        plain1 = QByteArrayLiteral("abcdefghijklmnopq");

        master = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=1\n"
            "media.m3u8\n");
        media = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:1\n"
            "#EXT-X-MEDIA-SEQUENCE:7\n"
            "#EXTINF:1,\n"
            "seg0.ts\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"key2.bin\","
            "IV=0x101112131415161718191A1B1C1D1E1F\n"
            "#EXTINF:1,\n"
            "seg1.ts\n"
            "#EXT-X-ENDLIST\n");

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

                    const QByteArray head = pending.left(headEnd);
                    const QString path = QString::fromUtf8(
                        head.split('\n').value(0).trimmed().split(' ').value(1));
                    for (const QByteArray &line : head.split('\n')) {
                        if (line.toLower().startsWith("referer:"))
                            lastKeyReferer = line.mid(8).trimmed();
                    }
                    socket->setProperty("responded", true);

                    QByteArray body;
                    QByteArray contentType = QByteArrayLiteral("application/octet-stream");
                    if (path == QLatin1String("/master.m3u8")) {
                        body = master;
                        contentType = "application/vnd.apple.mpegurl";
                    } else if (path == QLatin1String("/media.m3u8")) {
                        body = media;
                        contentType = "application/vnd.apple.mpegurl";
                    } else if (path == QLatin1String("/key.bin")) {
                        ++key1Requests;
                        body = key1;
                    } else if (path == QLatin1String("/key2.bin")) {
                        ++key2Requests;
                        body = key2;
                    } else if (path == QLatin1String("/seg0.ts")) {
                        body = segment0;
                    } else if (path == QLatin1String("/seg1.ts")) {
                        body = segment1;
                    } else {
                        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                        QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                        return;
                    }

                    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType
                                          + "\r\nContent-Length: " + QByteArray::number(body.size())
                                          + "\r\nConnection: close\r\n\r\n" + body;
                    socket->write(response);
                    QTimer::singleShot(0, socket, [socket] { socket->disconnectFromHost(); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray master;
    QByteArray media;
    QByteArray key1;
    QByteArray key2;
    QByteArray segment0;
    QByteArray segment1;
    QByteArray plain0;
    QByteArray plain1;
    int key1Requests = 0;
    int key2Requests = 0;
    QByteArray lastKeyReferer;

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1/").arg(serverPort());
    }
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

/// 通用探查站点：按请求路径返回固定响应，用来验证 SourceProbe 的候选顺序、
/// 网页型 fallback 和三轮关键词校验，不依赖公网。
class ProbeSiteServer : public QTcpServer
{
public:
    using Handler = std::function<void(const QString &target, QByteArray &body,
                                       QByteArray &contentType, int &status)>;

    explicit ProbeSiteServer(Handler handler, QObject *parent = nullptr)
        : QTcpServer(parent), m_handler(std::move(handler))
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
                    socket->setProperty("responded", true);
                    const QByteArray requestLine = pending.left(pending.indexOf("\r\n"));
                    const QList<QByteArray> parts = requestLine.split(' ');
                    const QString target = parts.size() >= 2
                                               ? QString::fromUtf8(parts.at(1))
                                               : QString();
                    targets.append(target);
                    QByteArray body;
                    QByteArray contentType = QByteArrayLiteral("text/html; charset=utf-8");
                    int status = 404;
                    if (m_handler)
                        m_handler(target, body, contentType, status);
                    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " "
                                          + (status >= 200 && status < 300 ? "OK" : "Not Found")
                                          + "\r\nContent-Type: " + contentType + "\r\n"
                                          + m_extraHeaders
                                          + "Content-Length: " + QByteArray::number(body.size())
                                          + "\r\nConnection: close\r\n\r\n" + body;
                    QTimer::singleShot(m_delayMs, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState)
                            return;
                        socket->write(response);
                        socket->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1/").arg(serverPort());
    }

    /// 让响应延迟 ms 毫秒写出，用来覆盖“请求在途时取消/销毁 Prober”的
    /// 生命周期路径；默认 0，不改变既有探查用例的时序。
    void setResponseDelay(int ms) { m_delayMs = qMax(0, ms); }

    QStringList targets;

private:
    Handler m_handler;
    int m_delayMs = 0;

public:
    /// 由测试追加的响应头（如 Content-Range），必须自带 CRLF。
    QByteArray m_extraHeaders;
};

} // namespace

class CoreLogicTest : public QObject
{
    Q_OBJECT

private slots:
    void timeFormat();
    void spectrumMathMapsToneToBand();
    void lrcParseWithOffsetAndMultipleTimestamps();
    void lrcIndexAt();
    void mediaItemJsonRoundTrip();
    void audioStreamScopeLabelsAndRoundTrip();
    void preferredPlaybackUrlSelectsProgressive();
    void playbackCandidateUrlsKeepOriginalAsFallback();
    void mediaItemValidity();
    void mediaIdentityUsesStableWebUrl();
    void dedupeItemsKeepsFirstOccurrence();
    void searchRankerGroupsAndRanks();
    void searchRankerKeepsBestDuplicate();
    void playlistModelRefreshesSignedUrlWithoutLosingProgress();
    void playlistModelUpdatesPlaybackPositionSeparately();
    void playbackSeekStepClampsToMediaBounds();
    void sourceConfigJsonRoundTrip();
    void sourceRegistryKeepsLongTermSeedsOnly();
    void sourceConfigSecretsNotPlaintext();
    void sourceConfigRejectsIncompleteConfig();
    void sourceConfigNormalizesTimeout();
    void sourceConfigRejectsCookieInjection();
    void sourceConfigRejectsNonHttpDetailTemplate();
    void sourceProbeVideoPathsAreSearchable();
    void sourceProbeMusicPathsAreSearchable();
    void sourceProbeLiveVideoSearch();
    void sourceProbeLiveMusicSearch();
    void sourceProbeLiveSubsonicStreamTemplate();
    void sourceProbeLiveRadioBrowserSearch();
    void sourceProbeLiveWebFallback();
    void sourceProbeWebFallbackForMaccmsSite();
    void sourceProbeMusicDoesNotBecomeWebVideo();
    void sourceProbeThirdKeywordValidation();
    void sourceProbeNestedArrayItemsPath();
    void sourceProbeKeepsOuterListOverEpisodeArrays();
    void sourceProbeAutoCoversBothTypeCandidates();
    void sourceProbeCancelBeforeHomeReply();
    void sourceProbeDeletedBeforeHomeReply();
    void sourceTimeoutUsesConfiguredValue();
    void httpLogUrlRedactsSignature();
    void headerPolicyStripsSecretsAcrossHost();
    void headerPolicyFailsClosedWithoutOrigin();
    void headerPolicyMatchesCompactedNames();
    void networkAddressPolicyBlocksPrivateMediaTargets();
    void htmlTextPoliciesPreserveSiteDifferences();
    void pageMediaExtractorReadsPlayerConfig();
    void mediaExtensionsKeepCategorySets();
    void playlistSnifferRecognizesBody();
    void downloadRejectsHtmlDisguisedAsOctetStream();
    void downloadUsesContentRangeEndWhenTotalUnknown();
    void downloadResumesMidFileWithoutMagicFalsePositive();
    void downloadHlsVerifiesSegmentsAndMapRange();
    void aes128CbcDecryptsKnownVector();
    void downloadHlsDecryptsAes128Segments();
    void hlsCacheManifestKeepsSessionKeyAndDisablesEncryptedSingleFile();
    void hlsCacheManifestDetectsFragmentedMp4Segments();
    void configMapperReadsNestedPaths();
    void configMapperBoolRulesDoNotMixWithNumbers();
    void configMapperSplitsEpisodesAndResolvesUrls();
    void streamProxySlicesRangeFromCache();
    void streamProxyResumesTruncatedCache();
    void streamProxyResumesTruncatedPartialCache();
    void streamProxyWritesCompleteDiskCache();
    void streamProxyResumesDiskCacheFromPartialFile();
    void mediaCacheLayoutForMusicAndVideo();
    void hlsCompleteCacheRecognition();
    void hlsCacheManifestParsesMasterAndSingleFile();
    void lyricsCacheRoundTrip();
    void shortcutsDefaultConflictDetection();
    void streamProxyHandlesSuffixlessTextPlaylist();
    void streamProxyDoesNotReuseLocalPrefixEntry();
    void streamProxyEvictsPrefetchWithoutLeakingSlots();
    void seekPrefetchWindowFollowsPlaylistIndex();
    void streamProxyMultiRangeForHlsSegment();
    void hlsPreloadIndexPersistsAndFollowsRenamedTrack();
    void hlsPreloadMigratorBuildsOnlyExactIndex();
    void streamProxyServesPreloadedSegmentWithoutUpstream();
    void streamProxySkipsSingleFileHlsSegmentCache();
    void streamProxyFallsBackWhenHlsRangeIgnored();
    void streamProxyHandlesFailedRangePartWithoutCrash();
    void hlsPlaylistParsesAndIndexes();
    void hlsPlaylistRewritesUris();
    void hlsPlaylistSameScopeRejectsSiblingDirectory();
    void hlsPlaylistParsesBitrateVariants();
    void hlsPlaylistRejectsTooManySegments();
    void hlsPlaylistSharedMediaMetadata();
    void streamTokensReuseByUrlAndHeaders();
    void streamTokensExpireByIdleClock();
    void streamProxyRejectsPrivateTargetFromPublicOrigin();
    void streamTokensKeepFreshTokensBeyondCapacity();
    void streamCacheStoreCountsRangeParts();
    void streamRangeParsesHeadersAndSplitsParts();
    void streamRangeAppendsAndMergesConsecutive();
};

void CoreLogicTest::timeFormat()
{
    QCOMPARE(TimeFormat::formatMs(-1), QStringLiteral("--:--"));
    QCOMPARE(TimeFormat::formatMs(0), QStringLiteral("00:00"));
    QCOMPARE(TimeFormat::formatMs(61'000), QStringLiteral("01:01"));
    QCOMPARE(TimeFormat::formatMs(3'600'000), QStringLiteral("1:00:00"));
}

void CoreLogicTest::spectrumMathMapsToneToBand()
{
    constexpr int sampleRate = 48000;
    QVector<float> silence(SpectrumMath::kFftSize, 0.0f);
    const QVector<float> silentLevels = SpectrumMath::analyze(silence, sampleRate);
    QCOMPARE(silentLevels.size(), SpectrumMath::kBandCount);
    for (float level : silentLevels)
        QVERIFY(level < 0.01f);

    auto tone = [](double amplitude, double hz) {
        QVector<float> samples(SpectrumMath::kFftSize);
        for (int i = 0; i < samples.size(); ++i) {
            samples[i] = float(amplitude
                               * std::sin(2.0 * 3.14159265358979323846 * hz * i
                                          / double(sampleRate)));
        }
        return samples;
    };

    const QVector<float> loud = SpectrumMath::analyze(tone(0.5, 440.0), sampleRate);
    const QVector<float> quiet = SpectrumMath::analyze(tone(0.08, 440.0), sampleRate);
    QCOMPARE(loud.size(), SpectrumMath::kBandCount);

    int peakBand = 0;
    for (int i = 1; i < loud.size(); ++i) {
        if (loud.at(i) > loud.at(peakBand))
            peakBand = i;
    }
    const double expected =
        std::log(440.0 / 40.0) / std::log(16000.0 / 40.0) * SpectrumMath::kBandCount;
    QVERIFY2(qAbs(peakBand - int(expected)) <= 2,
             qPrintable(QStringLiteral("440Hz 落到频段 %1，预期约 %2")
                            .arg(peakBand)
                            .arg(int(expected))));
    QVERIFY(loud.at(peakBand) > 0.8f);
    QVERIFY(quiet.at(peakBand) > 0.1f);
    QVERIFY(quiet.at(peakBand) < loud.at(peakBand));

    // 解码/重采样异常时可能混入 NaN 或无穷；频谱层必须把它们当静音，
    // 不能把非有限值传进柱子高度。
    QVector<float> poisoned = tone(0.5, 440.0);
    poisoned[10] = std::numeric_limits<float>::quiet_NaN();
    poisoned[11] = std::numeric_limits<float>::infinity();
    const QVector<float> sanitized = SpectrumMath::analyze(poisoned, sampleRate);
    QCOMPARE(sanitized.size(), SpectrumMath::kBandCount);
    for (float level : sanitized)
        QVERIFY(std::isfinite(level));
    QVERIFY(sanitized.at(peakBand) > 0.8f);
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
    item.headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret"));
    item.headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret"));
    item.online = true;
    item.durationMs = 12345;
    item.posterUrl = QUrl(QStringLiteral("https://example.com/p.jpg"));
    item.year = QStringLiteral("2026");
    item.area = QStringLiteral("中国大陆");
    item.genre = QStringLiteral("测试");
    item.description = QStringLiteral("简介");

    const QByteArray payload =
        QJsonDocument(mediaItemToJson(item)).toJson(QJsonDocument::Compact);
    QVERIFY(!payload.contains("sid=secret"));
    QVERIFY(!payload.contains("Bearer secret"));

    const MediaItem restored = mediaItemFromJson(mediaItemToJson(item));

    QVERIFY(restored.type == MediaType::Video);
    QCOMPARE(restored.title, item.title);
    QCOMPARE(restored.artist, item.artist);
    QCOMPARE(restored.url, item.url);
    QCOMPARE(restored.alternateUrls, item.alternateUrls);
    QCOMPARE(restored.lyricUrl, item.lyricUrl);
    QCOMPARE(restored.webUrl, item.webUrl);
    QCOMPARE(restored.sourceId, item.sourceId);
    QCOMPARE(restored.headers.value(QStringLiteral("Referer")),
             QStringLiteral("https://example.com/"));
    QVERIFY(!restored.headers.contains(QStringLiteral("Cookie")));
    QVERIFY(!restored.headers.contains(QStringLiteral("Authorization")));
    QCOMPARE(restored.online, item.online);
    QCOMPARE(restored.durationMs, item.durationMs);
    QCOMPARE(restored.posterUrl, item.posterUrl);
    QCOMPARE(restored.year, item.year);
    QCOMPARE(restored.area, item.area);
    QCOMPARE(restored.genre, item.genre);
    QCOMPARE(restored.description, item.description);
}

void CoreLogicTest::audioStreamScopeLabelsAndRoundTrip()
{
    // 字段名是唯一可靠信号：iTunes 的 previewUrl 只能当试听，Radio Browser 的
    // url_resolved 是电台流；Audius 的 stream.url 是完整曲目，不应误标。
    QCOMPARE(inferStreamScope(QStringLiteral("previewUrl")), QStringLiteral("preview"));
    QCOMPARE(inferStreamScope(QStringLiteral("url_resolved")), QStringLiteral("radio"));
    QCOMPARE(inferStreamScope(QStringLiteral("stream.url")), QString());
    QCOMPARE(inferStreamScope(QString(), QStringLiteral("previewUrl")),
             QStringLiteral("preview"));

    MediaItem preview;
    preview.type = MediaType::Music;
    preview.title = QStringLiteral("试听曲目");
    preview.url = QUrl(QStringLiteral("https://example.com/preview.m4a"));
    preview.streamScope = QStringLiteral("preview");
    QCOMPARE(preview.streamScopeLabel(), QStringLiteral("试听"));
    QVERIFY(preview.metaLine().contains(QStringLiteral("试听")));

    MediaItem restored = mediaItemFromJson(mediaItemToJson(preview));
    QCOMPARE(restored.streamScope, QStringLiteral("preview"));

    MediaItem radio;
    radio.type = MediaType::Music;
    radio.url = QUrl(QStringLiteral("https://example.com/radio.mp3"));
    radio.streamScope = QStringLiteral("radio");
    QCOMPARE(radio.streamScopeLabel(), QStringLiteral("电台"));
    QCOMPARE(mediaItemFromJson(mediaItemToJson(radio)).streamScope,
             QStringLiteral("radio"));

    MediaItem full;
    full.type = MediaType::Music;
    full.url = QUrl(QStringLiteral("https://example.com/full.mp3"));
    QVERIFY(full.streamScopeLabel().isEmpty());
    QVERIFY(full.metaLine().isEmpty());
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
    // 实际播放 mp4 时不能按 HLS 进入“定位中”，即使原始 URL 是 m3u8。
    QVERIFY(!playbackIsHls(hlsPrimary));

    MediaItem progressivePrimary;
    progressivePrimary.type = MediaType::Video;
    progressivePrimary.url = QUrl(QStringLiteral("https://example.com/video.flv"));
    progressivePrimary.alternateUrls = {QStringLiteral("https://example.com/video.m3u8")};
    QCOMPARE(preferredPlaybackUrl(progressivePrimary), progressivePrimary.url);
    QVERIFY(!playbackIsHls(progressivePrimary));

    MediaItem hlsOnly = hlsPrimary;
    hlsOnly.alternateUrls = {QStringLiteral("https://example.com/video.m3u8"),
                             QStringLiteral("https://cdn.example.com/not-a-video")};
    QCOMPARE(preferredPlaybackUrl(hlsOnly), hlsOnly.url);
    QVERIFY(playbackIsHls(hlsOnly));

    MediaItem music;
    music.type = MediaType::Music;
    music.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    music.alternateUrls = {QStringLiteral("https://example.com/song.mp4")};
    QCOMPARE(preferredPlaybackUrl(music), music.url);
    QVERIFY(!playbackIsHls(music));
}

void CoreLogicTest::playbackCandidateUrlsKeepOriginalAsFallback()
{
    MediaItem item;
    item.type = MediaType::Video;
    item.url = QUrl(QStringLiteral("https://example.com/video.m3u8"));
    item.alternateUrls = {QStringLiteral("https://example.com/video.mp4"),
                          QStringLiteral("https://example.com/video.mp4"),
                          QStringLiteral("https://example.com/video.flv")};

    const QVector<QUrl> candidates = playbackCandidateUrls(item);
    QCOMPARE(candidates.size(), 3);
    // 首选按 preferredPlaybackUrl 走 mp4，但原始 m3u8 必须保留为最后回退，
    // 不能“选完 preferred 就把 alternateUrls/主地址丢掉”。
    QCOMPARE(candidates.at(0), QUrl(QStringLiteral("https://example.com/video.mp4")));
    QCOMPARE(candidates.at(1), QUrl(QStringLiteral("https://example.com/video.flv")));
    QCOMPARE(candidates.at(2), item.url);
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

    // query 里真正区分内容的 id 不能一起丢掉，否则 ?id=1 / ?id=2 会被
    // 当成同一条，播放列表和缓存都会串到第一条。
    MediaItem idOne = noWeb;
    idOne.url = QUrl(QStringLiteral("https://cdn.example.com/play?id=1"));
    MediaItem idTwo = idOne;
    idTwo.url = QUrl(QStringLiteral("https://cdn.example.com/play?id=2"));
    QVERIFY(mediaIdentity(idOne) != mediaIdentity(idTwo));
}

void CoreLogicTest::dedupeItemsKeepsFirstOccurrence()
{
    MediaItem first;
    first.online = true;
    first.title = QStringLiteral("第一集");
    first.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    first.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=old"));

    MediaItem duplicate = first;
    duplicate.url = QUrl(QStringLiteral("https://cdn.example.com/b.mp4?token=new"));
    duplicate.artist = QStringLiteral("另一个源");

    MediaItem nextEpisode = first;
    nextEpisode.title = QStringLiteral("第二集");

    MediaItem localA;
    localA.url = QUrl::fromLocalFile(QStringLiteral("/tmp/a.mp3"));
    MediaItem localDuplicate = localA;
    localDuplicate.title = QStringLiteral("同名文件");

    const QVector<MediaItem> unique = dedupeMediaItems(
        {first, duplicate, nextEpisode, localA, localDuplicate});
    QCOMPARE(unique.size(), 3);
    // 保留第一次出现的 URL，分页里展示的仍是原始结果顺序
    QCOMPARE(unique.at(0).url, first.url);
    QCOMPARE(unique.at(1).title, QStringLiteral("第二集"));
    QCOMPARE(unique.at(2).url, localA.url);
}

void CoreLogicTest::searchRankerGroupsAndRanks()
{
    MediaItem exactMusic;
    exactMusic.type = MediaType::Music;
    exactMusic.online = true;
    exactMusic.title = QStringLiteral("稻香");
    exactMusic.artist = QStringLiteral("周杰伦");
    exactMusic.url = QUrl(QStringLiteral("https://example.com/song.mp3"));

    MediaItem partialMusic = exactMusic;
    partialMusic.title = QStringLiteral("稻香现场版");
    partialMusic.url = QUrl(QStringLiteral("https://example.com/live.mp3"));

    MediaItem exactVideo;
    exactVideo.type = MediaType::Video;
    exactVideo.online = true;
    exactVideo.title = QStringLiteral("稻香");
    exactVideo.url = QUrl(QStringLiteral("https://example.com/video.mp4"));

    // 输入顺序故意把视频和弱匹配放前面：排序后音乐组在前，组内标题完全匹配在前。
    const QVector<MediaItem> ranked =
        SearchRanker::rankAndDedupe({exactVideo, partialMusic, exactMusic},
                                    QStringLiteral("稻香"));
    QCOMPARE(ranked.size(), 3);
    QCOMPARE(ranked.at(0).url, exactMusic.url);
    QCOMPARE(ranked.at(1).url, partialMusic.url);
    QCOMPARE(ranked.at(2).url, exactVideo.url);
    QVERIFY(ranked.at(0).type == MediaType::Music);
    QVERIFY(ranked.at(2).type == MediaType::Video);
}

void CoreLogicTest::searchRankerKeepsBestDuplicate()
{
    MediaItem first;
    first.online = true;
    first.title = QStringLiteral("稻香");
    first.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    first.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp3"));

    MediaItem better = first;
    better.artist = QStringLiteral("周杰伦");
    better.quality = QStringLiteral("无损");
    better.url = QUrl(QStringLiteral("https://cdn.example.com/b.mp3"));

    const QVector<MediaItem> ranked =
        SearchRanker::rankAndDedupe({first, better}, QStringLiteral("周杰伦 稻香"));
    QCOMPARE(ranked.size(), 1);
    // 同 identity 不能继续“先到先得”：歌手命中、品质更好的第二条应被保留。
    QCOMPARE(ranked.first().artist, QStringLiteral("周杰伦"));
    QCOMPARE(ranked.first().url, better.url);
}

void CoreLogicTest::playlistModelRefreshesSignedUrlWithoutLosingProgress()
{
    PlaylistModel model;
    MediaItem stored;
    stored.type = MediaType::Video;
    stored.title = QStringLiteral("第一集");
    stored.online = true;
    stored.webUrl = QUrl(QStringLiteral("https://example.com/detail/1"));
    stored.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=old"));
    stored.quality = QStringLiteral("720P");
    stored.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/old"));
    stored.posterUrl = QUrl(QStringLiteral("https://example.com/old.jpg"));
    stored.positionMs = 42 * 1000;
    QVERIFY(model.addItem(stored));

    MediaItem fresh = stored;
    fresh.url = QUrl(QStringLiteral("https://cdn.example.com/a.mp4?token=new"));
    fresh.quality = QStringLiteral("1080P");
    fresh.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/new"));
    fresh.posterUrl = QUrl(QStringLiteral("https://example.com/new.jpg"));
    fresh.positionMs = 0; // 搜索结果不会带用户进度
    fresh.previewOnly = true;

    QCOMPARE(model.indexOf(fresh), 0);
    QVERIFY(model.refreshItem(0, fresh));
    const MediaItem refreshed = model.itemAt(0);
    QCOMPARE(refreshed.url, fresh.url);
    QCOMPARE(refreshed.quality, QStringLiteral("1080P"));
    QCOMPARE(refreshed.headers.value(QStringLiteral("Referer")),
             QStringLiteral("https://example.com/new"));
    QCOMPARE(refreshed.posterUrl, fresh.posterUrl);
    // 永久条目即使被试听刷新，也不能退回临时状态；视频进度必须保留。
    QVERIFY(!refreshed.previewOnly);
    QCOMPARE(refreshed.positionMs, qint64(42 * 1000));

    // 再次刷新相同内容不应产生无意义的数据变更。
    QVERIFY(!model.refreshItem(0, fresh));
}

void CoreLogicTest::playlistModelUpdatesPlaybackPositionSeparately()
{
    PlaylistModel model;

    MediaItem video;
    video.type = MediaType::Video;
    video.title = QStringLiteral("第二集");
    video.online = true;
    video.url = QUrl(QStringLiteral("https://cdn.example.com/b.mp4"));
    QVERIFY(model.addItem(video));

    // 进度只能通过专用入口写入：停止、切歌、退出时都能落盘。
    QVERIFY(model.updatePlaybackPosition(0, 61 * 1000, 4 * 60 * 1000));
    QCOMPARE(model.itemAt(0).positionMs, qint64(61 * 1000));
    QCOMPARE(model.itemAt(0).durationMs, qint64(4 * 60 * 1000));
    QVERIFY(!model.updatePlaybackPosition(0, 61 * 1000, 4 * 60 * 1000));

    // 搜索结果刷新仍然不能碰用户看到一半的进度。
    MediaItem refreshed = model.itemAt(0);
    refreshed.url = QUrl(QStringLiteral("https://cdn.example.com/b.mp4?token=new"));
    refreshed.positionMs = 0;
    QVERIFY(model.refreshItem(0, refreshed));
    QCOMPARE(model.itemAt(0).positionMs, qint64(61 * 1000));

    MediaItem music;
    music.type = MediaType::Music;
    music.title = QStringLiteral("歌");
    music.url = QUrl(QStringLiteral("https://cdn.example.com/song.mp3"));
    QVERIFY(model.addItem(music));
    QVERIFY(!model.updatePlaybackPosition(1, 5000, 5000));
}

void CoreLogicTest::playbackSeekStepClampsToMediaBounds()
{
    // 正常步进、开头与结尾夹取、未知时长保持原位。
    QCOMPARE(PlaybackController::steppedSeekTarget(30000, 120000, 10000), 40000);
    QCOMPARE(PlaybackController::steppedSeekTarget(30000, 120000, -10000), 20000);
    QCOMPARE(PlaybackController::steppedSeekTarget(5000, 120000, -10000), 0);
    QCOMPARE(PlaybackController::steppedSeekTarget(115000, 120000, 10000), 120000);
    QCOMPARE(PlaybackController::steppedSeekTarget(30000, 0, 10000), 30000);
    // 基准位置本身越界时也要先夹回媒体范围，不能让 UI 把负位置传下去。
    QCOMPARE(PlaybackController::steppedSeekTarget(-5000, 120000, 0), 0);
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
    config.headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret-token"));
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
    const QString serialized =
        QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact));
    QVERIFY(!serialized.contains(QStringLiteral("sid=1")));
    QVERIFY(!serialized.contains(QStringLiteral("secret-token")));
    QVERIFY(json.value(QStringLiteral("cookie")).toString().startsWith(
        QStringLiteral("mvp-dpapi:v1:")));
    QVERIFY(!json.value(QStringLiteral("headers")).toObject().contains(
        QStringLiteral("Authorization")));
    QVERIFY(json.value(QStringLiteral("headersProtected")).toObject().contains(
        QStringLiteral("Authorization")));
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

void CoreLogicTest::sourceRegistryKeepsLongTermSeedsOnly()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    qputenv("MVP_DATA_DIR", temp.path().toUtf8());

    SourceRegistry registry;
    QVERIFY2(registry.load(), qPrintable(registry.lastError()));

    // 只保留三个长期可用源，全部作为受保护的内置源并默认启用。
    const QStringList keptIds = {QStringLiteral("netease"), QStringLiteral("web_dmxq"),
                                 QStringLiteral("audius")};
    for (const QString &id : keptIds) {
        const SourceConfig config = registry.sourceById(id);
        QVERIFY2(!config.id.isEmpty(), qPrintable(id));
        QVERIFY2(config.enabled, qPrintable(id));
        QVERIFY2(config.builtin, qPrintable(id));
        QVERIFY2(config.isValid(), qPrintable(config.validationError()));
    }
    QCOMPARE(registry.sources().size(), keptIds.size());

    // 旧示例/试验源不再作为种子，也不能通过 upsert 重新加回来。
    for (const QString &retired : {QStringLiteral("itunes"), QStringLiteral("radio_browser"),
                                   QStringLiteral("ccmixter"),
                                   QStringLiteral("web_studioslob"),
                                   QStringLiteral("movie_example")}) {
        QVERIFY2(registry.sourceById(retired).id.isEmpty(), qPrintable(retired));
    }

    if (oldDataDir.isEmpty())
        qunsetenv("MVP_DATA_DIR");
    else
        qputenv("MVP_DATA_DIR", oldDataDir);
}

void CoreLogicTest::sourceConfigSecretsNotPlaintext()
{
    if (!SecretStore::isAvailable())
        QSKIP("当前平台没有凭据加密能力，源凭据按设计不落盘");

    QJsonObject legacy;
    legacy.insert(QStringLiteral("id"), QStringLiteral("legacy"));
    legacy.insert(QStringLiteral("name"), QStringLiteral("旧源"));
    legacy.insert(QStringLiteral("url"), QStringLiteral("https://api.example.com/search"));
    legacy.insert(QStringLiteral("itemsPath"), QStringLiteral("data"));
    legacy.insert(QStringLiteral("fields"),
                  QJsonObject{{QStringLiteral("title"), QStringLiteral("name")},
                              {QStringLiteral("streamUrl"), QStringLiteral("url")}});
    legacy.insert(QStringLiteral("headers"),
                  QJsonObject{{QStringLiteral("Authorization"),
                               QStringLiteral("Bearer legacy-token")}});
    legacy.insert(QStringLiteral("cookie"), QStringLiteral("sid=legacy"));

    bool ok = false;
    const SourceConfig config = sourceConfigFromJson(legacy, &ok);
    QVERIFY(ok);
    QCOMPARE(config.cookie, QStringLiteral("sid=legacy"));
    QCOMPARE(config.headers.value(QStringLiteral("Authorization")),
             QStringLiteral("Bearer legacy-token"));

    const QJsonObject stored = sourceConfigToJson(config);
    const QString serialized =
        QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact));
    QVERIFY(!serialized.contains(QStringLiteral("sid=legacy")));
    QVERIFY(!serialized.contains(QStringLiteral("legacy-token")));
    QVERIFY(stored.value(QStringLiteral("cookie")).toString().startsWith(
        QStringLiteral("mvp-dpapi:v1:")));
    QVERIFY(stored.value(QStringLiteral("headersProtected"))
                .toObject()
                .contains(QStringLiteral("Authorization")));
    QVERIFY(!stored.value(QStringLiteral("headers"))
                 .toObject()
                 .contains(QStringLiteral("Authorization")));

    bool restoredOk = false;
    const SourceConfig restored = sourceConfigFromJson(stored, &restoredOk);
    QVERIFY(restoredOk);
    QCOMPARE(restored.cookie, QStringLiteral("sid=legacy"));
    QCOMPARE(restored.headers.value(QStringLiteral("Authorization")),
             QStringLiteral("Bearer legacy-token"));
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
    QCOMPARE(config.timeoutMs, 1000);
    // 负值仍表示自动预算
    QJsonObject negative = object;
    negative.insert(QStringLiteral("timeoutMs"), -5);
    const SourceConfig autoConfig = sourceConfigFromJson(negative, &ok);
    QVERIFY(ok);
    QCOMPARE(autoConfig.timeoutMs, 0);
    // 缺失 timeoutMs 也按自动预算，不能偷偷回落到 8 秒
    QJsonObject absent = object;
    absent.remove(QStringLiteral("timeoutMs"));
    const SourceConfig absentConfig = sourceConfigFromJson(absent, &ok);
    QVERIFY(ok);
    QCOMPARE(absentConfig.timeoutMs, 0);
}

void CoreLogicTest::sourceConfigRejectsCookieInjection()
{
    SourceConfig config;
    config.id = QStringLiteral("cookie-injection");
    config.name = QStringLiteral("Cookie 注入");
    config.kind = QStringLiteral("external");
    config.command = QStringLiteral("parser.exe");
    config.cookie = QStringLiteral("sid=1\r\nX-Injected: yes");
    QVERIFY(!config.isValid());
    QVERIFY(config.validationError().contains(QStringLiteral("换行")));

    // 旧配置/手工导入里的非法 Cookie 读入时直接清掉，不能显示成“已配置”
    // 但请求层实际丢弃，造成“登录态已填却不生效”的错觉。
    QJsonObject object;
    object.insert(QStringLiteral("id"), config.id);
    object.insert(QStringLiteral("name"), config.name);
    object.insert(QStringLiteral("kind"), config.kind);
    object.insert(QStringLiteral("command"), config.command);
    object.insert(QStringLiteral("cookie"), config.cookie);
    bool ok = false;
    const SourceConfig restored = sourceConfigFromJson(object, &ok);
    QVERIFY(ok);
    QVERIFY(restored.cookie.isEmpty());
}

void CoreLogicTest::sourceConfigRejectsNonHttpDetailTemplate()
{
    // 详情模板也由 QNetworkRequest 发起；file:/// 会让客户端去读本地文件，
    // isValid() 和 validationError() 必须给出同样的拒绝结果。
    SourceConfig config;
    config.id = QStringLiteral("detail-scheme");
    config.name = QStringLiteral("详情协议校验");
    config.kind = QStringLiteral("config");
    config.method = QStringLiteral("GET");
    config.urlTemplate = QStringLiteral("https://api.example.com/search");
    config.detailUrlTemplate = QStringLiteral("file:///etc/passwd");
    config.itemsPath = QStringLiteral("data");
    config.fields.insert(QStringLiteral("title"), QStringLiteral("title"));
    config.detailFields.insert(QStringLiteral("streamUrl"), QStringLiteral("url"));
    QVERIFY(config.needsDetailFetch());
    QVERIFY(!config.isValid());
    QVERIFY(config.validationError().contains(QStringLiteral("详情地址")));

    config.detailUrlTemplate = QStringLiteral("https://api.example.com/detail/{id}");
    QVERIFY(config.isValid());
    QVERIFY(config.validationError().isEmpty());
}

void CoreLogicTest::sourceProbeLiveVideoSearch()
{
    // 真实网络验证默认跳过，避免 CI/离线环境误报；用
    // MVP_LIVE_PROBE=1 mvp_tests sourceProbeLiveVideoSearch 手动执行。
    if (!qEnvironmentVariableIsSet("MVP_LIVE_PROBE"))
        QSKIP("设置 MVP_LIVE_PROBE=1 才会访问真实站点");

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());

    prober.start(QUrl(QStringLiteral("https://api.apibdzy.com")), MediaType::Video, {}, true);
    QVERIFY2(finished.wait(40000), "真实站点探测在 40 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("探测失败：") + result.error));
    QVERIFY(result.mediaType == MediaType::Video);
    QVERIFY2(result.config.urlTemplate.contains(QStringLiteral("wd=")),
             qPrintable(QStringLiteral("保存的地址没有关键词参数：") + result.config.urlTemplate));
    QVERIFY(result.sampleCount > 0);
    QVERIFY(!result.sampleTitles.isEmpty());
}

void CoreLogicTest::sourceProbeVideoPathsAreSearchable()
{
    const QStringList paths = SourceProbe::defaultPaths(MediaType::Video);
    // 真实节点验证表明 MacCMS 有 ac=detail/videolist/list、/inc/api.php、
    // /inc/api_mac10.php、/index.php/ajax/data、带/不带尾斜杠等多种形态。
    QVERIFY(paths.size() >= 10);
    for (const QString &path : paths) {
        QVERIFY2(path.contains(QStringLiteral("{keyword}")),
                 qPrintable(QStringLiteral("探测路径必须带关键词占位符：") + path));
        QVERIFY2(path.contains(QStringLiteral("wd=")),
                 qPrintable(QStringLiteral("探测路径必须使用 wd 关键词参数：") + path));
    }
    QVERIFY(paths.contains(QStringLiteral("/api.php/provide/vod/?ac=detail&wd={keyword}")));
    QVERIFY(paths.contains(QStringLiteral("/api.php/provide/vod/?ac=videolist&wd={keyword}")));
    QVERIFY(paths.contains(QStringLiteral("/inc/api.php?ac=detail&wd={keyword}")));
    QVERIFY(paths.contains(QStringLiteral("/inc/api_mac10.php?ac=detail&wd={keyword}")));
    QVERIFY(paths.contains(QStringLiteral("/index.php/ajax/data?mid=1&wd={keyword}&limit={limit}")));
    // 不能把不带关键词的列表页当成搜索源，否则试搜会“有结果”但搜索词完全无效。
    QVERIFY(!paths.contains(QStringLiteral("/api.php/provide/vod/?ac=list")));
}

void CoreLogicTest::sourceProbeMusicPathsAreSearchable()
{
    const QStringList paths = SourceProbe::defaultPaths(MediaType::Music);
    // 真实音乐节点验证表明：iTunes、Audius、ccMixter、Radio Browser 可匿名搜索；
    // Subsonic/Navidrome 能搜索但只返回 id；Funkwhale/Jellyfin/Ampache 需要凭据。
    QVERIFY(paths.size() >= 16);
    for (const QString &path : paths) {
        QVERIFY2(path.contains(QStringLiteral("{keyword}")),
                 qPrintable(QStringLiteral("音乐探测路径必须带关键词占位符：") + path));
    }
    QVERIFY(paths.contains(QStringLiteral("/rest/search3?query={keyword}&songCount={limit}&f=json")));
    QVERIFY(paths.contains(QStringLiteral("/api/v1/tracks/?q={keyword}&page_size={limit}")));
    QVERIFY(paths.contains(QStringLiteral("/json/stations/search?name={keyword}&limit={limit}")));
    QVERIFY(paths.contains(QStringLiteral("/json/stations/byname/{keyword}?limit={limit}")));
    QVERIFY(paths.contains(QStringLiteral("/api/query?f=json&search={keyword}&limit={limit}")));
    QVERIFY(paths.contains(QStringLiteral("/Items?searchTerm={keyword}&IncludeItemTypes=Audio&Limit={limit}")));
    QVERIFY(paths.contains(QStringLiteral("/wp-json/wp/v2/search?search={keyword}&per_page={limit}")));
    QVERIFY(paths.contains(QStringLiteral("/search?term={keyword}&media=music&limit={limit}")));
    // 不能把影视的 wd 参数混进音乐默认路径，否则音乐试搜会永远拿不到结果。
    for (const QString &path : paths)
        QVERIFY2(!path.contains(QStringLiteral("wd=")),
                 qPrintable(QStringLiteral("音乐路径不应使用影视 wd 参数：") + path));
}

void CoreLogicTest::sourceProbeLiveWebFallback()
{
    // 真实 MacCMS 网页站验证默认跳过；用
    // MVP_LIVE_PROBE=1 mvp_tests sourceProbeLiveWebFallback 手动执行。
    if (!qEnvironmentVariableIsSet("MVP_LIVE_PROBE"))
        QSKIP("设置 MVP_LIVE_PROBE=1 才会访问真实网页站");

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(QStringLiteral("https://dmxq.info/")), MediaType::Video, {}, true);
    QVERIFY2(finished.wait(45000), "真实网页站探测在 45 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("网页型 fallback 失败：") + result.error));
    QCOMPARE(result.siteKind, SourceProbe::SiteKind::MaccmsWeb);
    QCOMPARE(result.config.provider, QStringLiteral("maccms_web"));
    QVERIFY(result.sampleCount > 0);
}

void CoreLogicTest::sourceProbeLiveMusicSearch()
{
    // 真实网络验证默认跳过；用
    // MVP_LIVE_PROBE=1 mvp_tests sourceProbeLiveMusicSearch 手动执行。
    if (!qEnvironmentVariableIsSet("MVP_LIVE_PROBE"))
        QSKIP("设置 MVP_LIVE_PROBE=1 才会访问真实音乐站点");

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());

    prober.start(QUrl(QStringLiteral("https://itunes.apple.com")), MediaType::Music, {}, false);
    QVERIFY2(finished.wait(40000), "iTunes 音乐探测在 40 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("音乐探测失败：") + result.error));
    QVERIFY(result.mediaType == MediaType::Music);
    QVERIFY2(result.config.urlTemplate.contains(QStringLiteral("term=")),
             qPrintable(QStringLiteral("保存的地址没有 term 关键词参数：") + result.config.urlTemplate));
    QVERIFY(result.sampleCount > 0);
    QVERIFY(!result.sampleTitles.isEmpty());
    QCOMPARE(result.config.fields.value(QStringLiteral("title")), QStringLiteral("trackName"));
    QCOMPARE(result.config.fields.value(QStringLiteral("streamUrl")), QStringLiteral("previewUrl"));
}

void CoreLogicTest::sourceProbeLiveSubsonicStreamTemplate()
{
    // 真实网络验证默认跳过；用
    // MVP_LIVE_PROBE=1 mvp_tests sourceProbeLiveSubsonicStreamTemplate 手动执行。
    // 这个测试用的是 Navidrome 公开 demo 凭据，只放在测试里，
    // 不会写进 SourceProbe 的内置默认配置。
    if (!qEnvironmentVariableIsSet("MVP_LIVE_PROBE"))
        QSKIP("设置 MVP_LIVE_PROBE=1 才会访问真实 Subsonic/Navidrome demo");

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());

    const QString endpoint = QStringLiteral(
        "https://demo.navidrome.org/rest/search3?u=demo&p=demo&v=1.16.1&c=MVP&f=json"
        "&query={keyword}&songCount={limit}");
    prober.start(QUrl(QStringLiteral("https://demo.navidrome.org")), MediaType::Music,
                 {endpoint}, false);
    QVERIFY2(finished.wait(40000), "Navidrome 探测在 40 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("Navidrome 探测失败：") + result.error));
    QVERIFY(result.mediaType == MediaType::Music);
    QCOMPARE(result.config.fields.value(QStringLiteral("title")), QStringLiteral("title"));
    const QString stream = result.config.fields.value(QStringLiteral("streamUrl"));
    QVERIFY2(stream.startsWith(QLatin1Char('@')),
             qPrintable(QStringLiteral("Subsonic 没有生成 @ 模板：") + stream));
    QVERIFY2(stream.contains(QStringLiteral("/rest/stream?id={id}")),
             qPrintable(QStringLiteral("Subsonic 模板不是 /rest/stream：") + stream));
    QVERIFY2(stream.contains(QStringLiteral("u=demo")) && stream.contains(QStringLiteral("p=demo")),
             qPrintable(QStringLiteral("Subsonic 模板没有带上认证参数：") + stream));
    QVERIFY(result.sampleCount > 0);
}

void CoreLogicTest::sourceProbeLiveRadioBrowserSearch()
{
    // 真实网络验证默认跳过；用
    // MVP_LIVE_PROBE=1 mvp_tests sourceProbeLiveRadioBrowserSearch 手动执行。
    // 这个测试专门覆盖“根数组 + 无 .mp3 后缀的 url_resolved”这条真实分支。
    if (!qEnvironmentVariableIsSet("MVP_LIVE_PROBE"))
        QSKIP("设置 MVP_LIVE_PROBE=1 才会访问真实 Radio Browser");

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());

    prober.start(QUrl(QStringLiteral("https://de1.api.radio-browser.info")),
                 MediaType::Music, {}, false);
    QVERIFY2(finished.wait(40000), "Radio Browser 探测在 40 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("Radio Browser 探测失败：") + result.error));
    QVERIFY(result.mediaType == MediaType::Music);
    QVERIFY2(result.config.urlTemplate.contains(QStringLiteral("/json/stations/search")),
             qPrintable(QStringLiteral("保存的地址不是 Radio Browser 搜索：")
                        + result.config.urlTemplate));
    QCOMPARE(result.config.fields.value(QStringLiteral("title")), QStringLiteral("name"));
    QCOMPARE(result.config.fields.value(QStringLiteral("streamUrl")),
             QStringLiteral("url_resolved"));
    QVERIFY(result.sampleCount > 0);
}

void CoreLogicTest::sourceProbeWebFallbackForMaccmsSite()
{
    // 只有网址、没有 JSON 采集接口的公开 MacCMS 站：首页强特征命中后，
    // 10 条 JSON 候选都失败，应转成“网页型影视源”，而不是把用户推去手工填字段。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        if (target == QLatin1String("/")) {
            status = 200;
            contentType = QByteArrayLiteral("text/html; charset=utf-8");
            body = QStringLiteral("<html><head><title>MacCMS 测试站</title></head>"
                                  "<body>maccms vodsearch voddetail</body></html>")
                       .toUtf8();
            return;
        }
        if (target.contains(QStringLiteral("ajax/suggest"))) {
            status = 200;
            contentType = QByteArrayLiteral("application/json");
            body = QByteArrayLiteral("{\"list\":[{\"id\":1,\"name\":\"庆余年\"}]}");
            return;
        }
        status = 404;
        contentType = QByteArrayLiteral("text/html");
        body = QByteArrayLiteral("not found");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(server.baseUrl()), MediaType::Video, {}, false);
    QVERIFY2(finished.wait(15000), "网页型 fallback 在 15 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("网页型影视源探测失败：") + result.error));
    QCOMPARE(result.siteKind, SourceProbe::SiteKind::MaccmsWeb);
    QCOMPARE(result.mediaType, MediaType::Video);
    QCOMPARE(result.config.kind, QStringLiteral("builtin"));
    QCOMPARE(result.config.provider, QStringLiteral("maccms_web"));
    QVERIFY(result.config.builtin == false);
    QVERIFY(result.config.urlTemplate.endsWith(QLatin1Char('/')));
    QVERIFY2(result.sampleTitles.contains(QStringLiteral("庆余年")),
             qPrintable(QStringLiteral("网页型试搜样例标题不对：")
                        + result.sampleTitles.join(QStringLiteral(","))));
}

void CoreLogicTest::sourceProbeMusicDoesNotBecomeWebVideo()
{
    // 同一个 MacCMS 首页，用户明确选“音乐”时不能生成网页型影视源：
    // 音乐没有 maccms_web 抓取器，误配只会制造一个永远搜不到东西的源。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        if (target == QLatin1String("/")) {
            status = 200;
            contentType = QByteArrayLiteral("text/html; charset=utf-8");
            body = QStringLiteral("<html><head><title>MacCMS 测试站</title></head>"
                                  "<body>maccms vodsearch</body></html>")
                       .toUtf8();
            return;
        }
        status = 404;
        contentType = QByteArrayLiteral("text/html");
        body = QByteArrayLiteral("not found");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(server.baseUrl()), MediaType::Music, {}, false);
    QVERIFY2(finished.wait(15000), "音乐误判测试在 15 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(!result.ok, "音乐类型不应把 MacCMS 首页配成网页型影视源");
    QVERIFY(result.siteKind != SourceProbe::SiteKind::MaccmsWeb);
    QVERIFY(result.config.provider != QStringLiteral("maccms_web"));
}

void CoreLogicTest::sourceProbeThirdKeywordValidation()
{
    // 第二轮空结果不能直接放行：继续用第三关键词确认，避免 catalog-only
    // 或只认单个词的接口被当成可搜索源。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        const QString decoded = QUrl::fromPercentEncoding(target.toUtf8());
        if (target == QLatin1String("/")) {
            status = 200;
            contentType = QByteArrayLiteral("text/html; charset=utf-8");
            body = QByteArrayLiteral("<html><head><title>JSON 测试站</title></head><body></body></html>");
            return;
        }
        if (!target.contains(QStringLiteral("provide/vod"))) {
            status = 404;
            contentType = QByteArrayLiteral("text/html");
            body = QByteArrayLiteral("not found");
            return;
        }
        QString title;
        if (decoded.contains(QStringLiteral("wd=电影")))
            title = QStringLiteral("首轮电影");
        else if (decoded.contains(QStringLiteral("wd=庆余年")))
            title = QStringLiteral("第三轮庆余年");
        if (title.isEmpty()) {
            // 第二轮故意返回空列表，验证会继续用第三关键词确认，
            // 而不是把“请求成功但 0 条”当成通过。
            status = 200;
            contentType = QByteArrayLiteral("application/json");
            body = QByteArrayLiteral("{\"list\":[]}");
            return;
        }
        status = 200;
        contentType = QByteArrayLiteral("application/json");
        body = QByteArrayLiteral("{\"list\":[{\"vod_name\":\"")
               + title.toUtf8()
               + QByteArrayLiteral("\",\"vod_pic\":\"https://example.com/p.jpg\","
                                   "\"vod_year\":\"2024\","
                                   "\"vod_play_url\":\"第1集$https://example.com/a.m3u8\"}]}");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(server.baseUrl()), MediaType::Video, {}, false);
    QVERIFY2(finished.wait(15000), "三轮关键词校验在 15 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("三轮关键词探测失败：") + result.error));
    QCOMPARE(result.siteKind, SourceProbe::SiteKind::JsonConfig);
    QCOMPARE(result.validationPasses, 3);
    QVERIFY(result.validationNote.contains(QStringLiteral("三轮")));
    QVERIFY(result.sampleCount > 0);
}

void CoreLogicTest::sourceProbeNestedArrayItemsPath()
{
    // 根是 `[{"list":[...]}]` 的接口：外层数组只有 1 条包装对象，
    // 真正的 2 条结果在 `[0].list` 里。旧扫描遇到数组就返回，
    // 会把外层 1 条当结果数组，字段全空后探测失败。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        const QString decoded = QUrl::fromPercentEncoding(target.toUtf8());
        if (target.contains(QStringLiteral("provide/vod"))) {
            QString title = QStringLiteral("嵌套数组片");
            if (decoded.contains(QStringLiteral("wd=电影")))
                title = QStringLiteral("嵌套数组电影");
            else if (decoded.contains(QStringLiteral("wd=庆余年")))
                title = QStringLiteral("嵌套数组庆余年");
            status = 200;
            contentType = QByteArrayLiteral("application/json");
            body = QByteArrayLiteral("[{\"list\":[")
                   + QByteArrayLiteral("{\"vod_name\":\"") + title.toUtf8()
                   + QByteArrayLiteral("\",\"vod_play_url\":\"第1集$https://example.com/a.m3u8\"},")
                   + QByteArrayLiteral("{\"vod_name\":\"") + title.toUtf8()
                   + QByteArrayLiteral(" 第二集\",\"vod_play_url\":\"第2集$https://example.com/b.m3u8\"}")
                   + QByteArrayLiteral("]}]");
            return;
        }
        status = 404;
        contentType = QByteArrayLiteral("text/html");
        body = QByteArrayLiteral("not found");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(server.baseUrl()), MediaType::Video, {}, false);
    QVERIFY2(finished.wait(15000), "嵌套数组探查在 15 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("嵌套数组探测失败：") + result.error));
    QCOMPARE(result.siteKind, SourceProbe::SiteKind::JsonConfig);
    QCOMPARE(result.config.itemsPath, QStringLiteral("[0].list"));
    QCOMPARE(result.sampleCount, 2);
    QVERIFY(!result.sampleTitles.isEmpty());
}

void CoreLogicTest::sourceProbeKeepsOuterListOverEpisodeArrays()
{
    // 结果项自身可能带一个更大的 episodes/tracks 数组。旧扫描对所有数组
    // 无条件下钻，会选中 data.list[0].episodes（50 条），把 10 条结果误读成
    // 第 1 条结果的播放片段。这里锁住“只有包装层数组才下钻”。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        const QString decoded = QUrl::fromPercentEncoding(target.toUtf8());
        if (target.contains(QStringLiteral("provide/vod"))) {
            QString title = QStringLiteral("外层列表片");
            if (decoded.contains(QStringLiteral("wd=电影")))
                title = QStringLiteral("外层列表电影");
            else if (decoded.contains(QStringLiteral("wd=庆余年")))
                title = QStringLiteral("外层列表庆余年");
            QByteArray items;
            for (int i = 0; i < 10; ++i) {
                if (i > 0)
                    items += ',';
                items += "{\"title\":\"" + title.toUtf8() + QByteArray::number(i)
                         + "\",\"vod_play_url\":\"第1集$https://example.com/"
                         + QByteArray::number(i) + ".m3u8\",\"episodes\":[";
                for (int j = 0; j < 50; ++j) {
                    if (j > 0)
                        items += ',';
                    items += "{\"name\":\"片段" + QByteArray::number(j)
                             + "\",\"url\":\"https://example.com/ep"
                             + QByteArray::number(j) + ".mp4\"}";
                }
                items += "]}";
            }
            status = 200;
            contentType = QByteArrayLiteral("application/json");
            body = "{\"code\":0,\"data\":{\"list\":[" + items + "]}}";
            return;
        }
        status = 404;
        contentType = QByteArrayLiteral("text/html");
        body = QByteArrayLiteral("not found");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    prober.start(QUrl(server.baseUrl()), MediaType::Video, {}, false);
    QVERIFY2(finished.wait(15000), "外层列表探查在 15 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(result.ok, qPrintable(QStringLiteral("外层列表探测失败：") + result.error));
    QCOMPARE(result.config.itemsPath, QStringLiteral("data.list"));
    QCOMPARE(result.sampleCount, 10);
}

void CoreLogicTest::sourceProbeAutoCoversBothTypeCandidates()
{
    // 影视 10 条 + 音乐 16 条 = 26 条。自动识别上限如果还是 20，
    // 后半段候选永远轮不到；这里用本地 404 站点确认两种类型都真的被请求过。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        if (target == QLatin1String("/")) {
            status = 200;
            contentType = QByteArrayLiteral("text/html; charset=utf-8");
            body = QByteArrayLiteral("<html><head><title>普通站点</title></head><body></body></html>");
            return;
        }
        status = 404;
        contentType = QByteArrayLiteral("text/html");
        body = QByteArrayLiteral("not found");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    qRegisterMetaType<SourceProbe::Result>("SourceProbe::Result");
    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(server.baseUrl()), MediaType::Video, {}, true);
    QVERIFY2(finished.wait(20000), "自动识别在 20 秒内没有返回");

    const SourceProbe::Result result =
        qvariant_cast<SourceProbe::Result>(finished.at(0).at(0));
    QVERIFY2(!result.ok, "全 404 站点不应探测成功");
    QVERIFY2(server.targets.size() >= 27,
             qPrintable(QStringLiteral("自动识别只请求了 %1 次，没有覆盖 26 条候选")
                        .arg(server.targets.size())));
    bool sawVideo = false;
    bool sawMusic = false;
    for (const QString &target : server.targets) {
        if (target.contains(QStringLiteral("provide/vod")))
            sawVideo = true;
        if (target.contains(QStringLiteral("/rest/search3"))
            || target.contains(QStringLiteral("/json/stations/search")))
            sawMusic = true;
    }
    QVERIFY2(sawVideo, "自动识别没有请求影视候选");
    QVERIFY2(sawMusic, "自动识别没有请求音乐候选");
}

void CoreLogicTest::sourceProbeCancelBeforeHomeReply()
{
    // 首页请求在途时取消探测：迟到响应必须被代次判断丢弃，不能再发
    // finished，也不能继续访问已经取消的 Prober 状态。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        Q_UNUSED(target);
        status = 200;
        contentType = QByteArrayLiteral("text/html; charset=utf-8");
        body = QByteArrayLiteral("<html><head><title>慢站点</title></head><body></body></html>");
    });
    server.setResponseDelay(250);
    QVERIFY(server.listen(QHostAddress::LocalHost));

    HttpClient http;
    SourceProbe::Prober prober(&http);
    QSignalSpy finished(&prober, &SourceProbe::Prober::finished);
    QVERIFY(finished.isValid());
    prober.start(QUrl(server.baseUrl()), MediaType::Video, {}, true);
    QVERIFY2(QTest::qWaitFor([&] { return !server.targets.isEmpty(); }, 1000),
             "首页请求还没有发到本地站点");
    prober.cancel();
    QTest::qWait(500);
    QCOMPARE(finished.count(), 0);
}

void CoreLogicTest::sourceProbeDeletedBeforeHomeReply()
{
    // 更极端的生命周期路径：对话框关闭后 Prober 先被析构，首页响应随后
    // 到达。QPointer 护栏必须保证回调只释放 reply，不触碰已释放的 Prober。
    ProbeSiteServer server([](const QString &target, QByteArray &body,
                              QByteArray &contentType, int &status) {
        Q_UNUSED(target);
        status = 200;
        contentType = QByteArrayLiteral("text/html; charset=utf-8");
        body = QByteArrayLiteral("<html><head><title>关闭后的迟到响应</title></head><body></body></html>");
    });
    server.setResponseDelay(250);
    QVERIFY(server.listen(QHostAddress::LocalHost));

    HttpClient http;
    SourceProbe::Prober *prober = new SourceProbe::Prober(&http);
    prober->start(QUrl(server.baseUrl()), MediaType::Video, {}, true);
    QVERIFY2(QTest::qWaitFor([&] { return !server.targets.isEmpty(); }, 1000),
             "首页请求还没有发到本地站点");
    delete prober;
    QTest::qWait(500);
}

void CoreLogicTest::sourceTimeoutUsesConfiguredValue()
{
    SourceConfig music;
    music.kind = QStringLiteral("config");
    music.timeoutMs = 12000;
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(music, 20), 12000);

    music.timeoutMs = 0;
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(music, 20), 20000);

    SourceConfig web;
    web.kind = QStringLiteral("builtin");
    web.provider = QStringLiteral("maccms_web");
    web.timeoutMs = 0;
    // 网页影视源没填超时仍按条数自动放宽
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(web, 20), 20000);
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(web, 50), 32000);

    // 一旦用户明确填写，网页源也按配置值，不再被自动预算覆盖
    web.timeoutMs = 15000;
    QCOMPARE(SearchCoordinator::sourceTimeoutMs(web, 50), 15000);
}

void CoreLogicTest::httpLogUrlRedactsSignature()
{
    const QString text = HttpClient::redactUrl(
        QUrl(QStringLiteral("https://cdn.example.com/a.m3u8?id=42&sign=abc123&token=xyz&key=kk&vuutv=vv&sig=s1&hdnts=h1&hash=hh&md5=mm")));
    const QUrlQuery query{QUrl(text)};

    QCOMPARE(query.queryItemValue(QStringLiteral("id")), QStringLiteral("42"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sign")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("token")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("key")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("vuutv")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("sig")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("hdnts")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("hash")), QStringLiteral("***"));
    QCOMPARE(query.queryItemValue(QStringLiteral("md5")), QStringLiteral("***"));
    QVERIFY(!text.contains(QStringLiteral("abc123")));
    QVERIFY(!text.contains(QStringLiteral("xyz")));

    const QString pathText = HttpClient::redactUrl(QUrl(QStringLiteral(
        "https://cdn.example.com/sign/0123456789abcdef0123456789abcdef/video.mp4")));
    QVERIFY(!pathText.contains(QStringLiteral("0123456789abcdef0123456789abcdef")));
    QVERIFY(pathText.contains(QStringLiteral("video.mp4")));

    // Subsonic 兼容接口常把账号放在 u/p，token/salt 放在 t/s；
    // 这些单字母参数也必须出现在日志脱敏范围里。
    const QString subsonicText = HttpClient::redactUrl(QUrl(QStringLiteral(
        "https://music.example.com/rest/search3?u=demo&p=secret&t=tok123&s=salt456&query=love&songCount=5")));
    const QUrlQuery subsonicQuery{QUrl(subsonicText)};
    QCOMPARE(subsonicQuery.queryItemValue(QStringLiteral("u")), QStringLiteral("***"));
    QCOMPARE(subsonicQuery.queryItemValue(QStringLiteral("p")), QStringLiteral("***"));
    QCOMPARE(subsonicQuery.queryItemValue(QStringLiteral("t")), QStringLiteral("***"));
    QCOMPARE(subsonicQuery.queryItemValue(QStringLiteral("s")), QStringLiteral("***"));
    QCOMPARE(subsonicQuery.queryItemValue(QStringLiteral("query")), QStringLiteral("love"));
    QCOMPARE(subsonicQuery.queryItemValue(QStringLiteral("songCount")), QStringLiteral("5"));
    QVERIFY(!subsonicText.contains(QStringLiteral("secret")));
    QVERIFY(!subsonicText.contains(QStringLiteral("tok123")));
    QVERIFY(!subsonicText.contains(QStringLiteral("salt456")));
}

void CoreLogicTest::playlistSnifferRecognizesBody()
{
    QVERIFY(PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/a/index.m3u8")), QString()));
    // 部分源会在 m3u8 地址末尾多给一个斜杠，仍然应识别为播放列表。
    QVERIFY(PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/a/index.m3u8/")), QString()));
    QVERIFY(PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/signed/abc")),
        QStringLiteral("application/vnd.apple.mpegurl")));
    QVERIFY(!PlaylistSniffer::metadataIndicatesPlaylist(
        QUrl(QStringLiteral("https://cdn.example.com/signed/abc")),
        QStringLiteral("text/plain")));
    // query 里带 .m3u8 的 MP4 不是 HLS；只看 path 后缀。
    const QUrl queryOnlyM3u8(
        QStringLiteral("https://cdn.example.com/video.mp4?next=.m3u8&token=x"));
    QVERIFY(!PlaylistSniffer::metadataIndicatesPlaylist(queryOnlyM3u8, QString()));
    QVERIFY(!isHlsUrl(queryOnlyM3u8));

    QVERIFY(PlaylistSniffer::bodyIsPlaylist(QByteArrayLiteral("#EXTM3U\n#EXTINF:1,\na.ts\n")));
    QVERIFY(PlaylistSniffer::bodyIsPlaylist(
        QByteArrayLiteral("\xEF\xBB\xBF  \r\n#EXTM3U\n#EXTINF:1,\na.ts\n")));
    QVERIFY(!PlaylistSniffer::bodyIsPlaylist(QByteArrayLiteral("<html>not a playlist</html>")));
    QVERIFY(!PlaylistSniffer::bodyIsPlaylist(
        QByteArrayLiteral("#EXTINF:1,\na.ts\n#EXTM3U\n")));
}

void CoreLogicTest::headerPolicyStripsSecretsAcrossHost()
{
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), QStringLiteral("https://source.example/"));
    headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret"));
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret"));
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("MVP"));

    const QHash<QString, QString> sameHost =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://source.example/a.m3u8")),
                                         headers);
    QVERIFY(sameHost.contains(QStringLiteral("Cookie")));
    QVERIFY(sameHost.contains(QStringLiteral("Authorization")));

    const QHash<QString, QString> crossHost =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://cdn.example/seg.ts")),
                                         headers);
    QVERIFY(!crossHost.contains(QStringLiteral("Cookie")));
    QVERIFY(!crossHost.contains(QStringLiteral("Authorization")));
    QCOMPARE(crossHost.value(QStringLiteral("Referer")),
             QStringLiteral("https://source.example/"));

    const QHash<QString, QString> downgrade =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("http://source.example/a")),
                                         headers);
    QVERIFY(!downgrade.contains(QStringLiteral("Cookie")));
}

void CoreLogicTest::headerPolicyFailsClosedWithoutOrigin()
{
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=secret"));
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret"));
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("MVP"));

    // 只有 Cookie、没有 Referer 时，绝对 CDN 地址不能拿到源站凭据。
    const QHash<QString, QString> noOrigin =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://cdn.example/seg.ts")),
                                         headers);
    QVERIFY(!noOrigin.contains(QStringLiteral("Cookie")));
    QVERIFY(!noOrigin.contains(QStringLiteral("Authorization")));
    QCOMPARE(noOrigin.value(QStringLiteral("User-Agent")), QStringLiteral("MVP"));

    // 显式源站才允许发敏感头；跨 host 仍剥离。
    const QUrl source(QStringLiteral("https://source.example/"));
    const QHash<QString, QString> sameHost =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://source.example/a")),
                                         headers, source);
    QVERIFY(sameHost.contains(QStringLiteral("Cookie")));

    const QHash<QString, QString> crossHost =
        HeaderPolicy::sanitizedForTarget(QUrl(QStringLiteral("https://cdn.example/a")),
                                         headers, source);
    QVERIFY(!crossHost.contains(QStringLiteral("Cookie")));
    QVERIFY(!crossHost.contains(QStringLiteral("Authorization")));

    const QHash<QString, QString> withHeaders =
        HeaderPolicy::withSourceHeaders(headers, source);
    QCOMPARE(withHeaders.value(QStringLiteral("Referer")),
             QStringLiteral("https://source.example/"));
    QCOMPARE(withHeaders.value(QStringLiteral("Origin")),
             QStringLiteral("https://source.example"));
}

void CoreLogicTest::headerPolicyMatchesCompactedNames()
{
    // 实际源配置里出现过分隔符不同的大小写变体；它们本质仍是凭据头，
    // 跨 host 时必须被裁剪，不能因为写法不同而漏发。
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("X-APIKEY")));
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("apiKey")));
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("X_API_KEY")));
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("access-key")));
    // 下划线/驼峰变体压掉分隔符后仍应命中；这些头跨 host 时同样可能带凭据。
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("X_AUTH")));
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("X_AUTH_KEY")));
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("X_ACCESS_KEY")));
    QVERIFY(HeaderPolicy::isSensitiveHeader(QStringLiteral("authKey")));
    QVERIFY(!HeaderPolicy::isSensitiveHeader(QStringLiteral("User-Agent")));
    QVERIFY(!HeaderPolicy::isSensitiveHeader(QStringLiteral("Accept")));

    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("X-APIKEY"), QStringLiteral("secret-1"));
    headers.insert(QStringLiteral("apiKey"), QStringLiteral("secret-2"));
    headers.insert(QStringLiteral("X_API_KEY"), QStringLiteral("secret-3"));
    headers.insert(QStringLiteral("X_AUTH_KEY"), QStringLiteral("secret-4"));
    headers.insert(QStringLiteral("X_ACCESS_KEY"), QStringLiteral("secret-5"));
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("MVP"));
    const QHash<QString, QString> safe =
        HeaderPolicy::withoutSensitiveHeaders(headers);
    QVERIFY(!safe.contains(QStringLiteral("X-APIKEY")));
    QVERIFY(!safe.contains(QStringLiteral("apiKey")));
    QVERIFY(!safe.contains(QStringLiteral("X_API_KEY")));
    QVERIFY(!safe.contains(QStringLiteral("X_AUTH_KEY")));
    QVERIFY(!safe.contains(QStringLiteral("X_ACCESS_KEY")));
    QCOMPARE(safe.value(QStringLiteral("User-Agent")), QStringLiteral("MVP"));
}

void CoreLogicTest::networkAddressPolicyBlocksPrivateMediaTargets()
{
    // 字面地址识别不能依赖 DNS，否则公网页面返回的内网地址会先被请求。
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://127.0.0.1:8080/a.mp4"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://10.1.2.3/a.mp4"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://172.20.0.1/a.mp4"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://192.168.1.10/a.mp4"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://169.254.169.254/latest/meta-data"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://[::1]/a.mp4"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://localhost:9000/a.mp4"))));
    QVERIFY(NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://metadata.google.internal/computeMetadata"))));
    QVERIFY(!NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("https://example.com/a.mp4"))));
    QVERIFY(!NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://8.8.8.8/a.mp4"))));
    // 后缀匹配必须精确，`localhost.evil.example` 不能被当成本机。
    QVERIFY(!NetworkAddressPolicy::isLocalOrPrivateHost(
        QUrl(QStringLiteral("http://localhost.evil.example/a.mp4"))));

    const QUrl publicSource(QStringLiteral("https://example.com/"));
    const QUrl localSource(QStringLiteral("http://127.0.0.1:8080/"));
    QVERIFY(!NetworkAddressPolicy::allowsMediaTarget(
        QUrl(QStringLiteral("http://127.0.0.1/a.mp4")), publicSource));
    QVERIFY(NetworkAddressPolicy::allowsMediaTarget(
        QUrl(QStringLiteral("http://127.0.0.1/a.mp4")), localSource));
    QVERIFY(NetworkAddressPolicy::allowsMediaTarget(
        QUrl(QStringLiteral("https://cdn.example/a.mp4")), publicSource));
    QVERIFY(!NetworkAddressPolicy::allowsMediaTarget(
        QUrl(QStringLiteral("ftp://example.com/a.mp4")), publicSource));
    QVERIFY(!NetworkAddressPolicy::allowsMediaTarget(
        QUrl(QStringLiteral("file:///C:/a.mp4")), publicSource));

    // 统一 URL 判断的边界：必须有 host，拒绝协议正确但 host 为空的异常地址。
    QVERIFY(NetworkAddressPolicy::isHttpMediaUrl(
        QUrl(QStringLiteral("https://cdn.example/a.mp4"))));
    QVERIFY(!NetworkAddressPolicy::isHttpMediaUrl(QUrl(QStringLiteral("http:///a.mp4"))));
    QVERIFY(!NetworkAddressPolicy::isHttpMediaUrl(QUrl(QStringLiteral("ftp://example.com/a.mp4"))));
    QVERIFY(!NetworkAddressPolicy::isHttpMediaUrl(QUrl()));
}

void CoreLogicTest::htmlTextPoliciesPreserveSiteDifferences()
{
    // Maccms 网页源：双层转义、nbsp、数字实体都要还原。
    const HtmlTextPolicy maccms{2, true, true, true};
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&amp;#39;"), maccms),
             QStringLiteral("'"));
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&#x4E07;"), maccms),
             QStringLiteral("万"));
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&#xD800;"), maccms),
             QStringLiteral("&#xD800;"));

    // 搜索链接属性：只解一轮，不把 nbsp 当成空格。
    const HtmlTextPolicy attribute{1, false, true, true};
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&#x2F;"), attribute),
             QStringLiteral("/"));
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&nbsp;"), attribute),
             QStringLiteral("&nbsp;"));

    // 配置映射：保留 nbsp，但不还原数字实体和 &apos;，避免改变接口正文。
    const HtmlTextPolicy mapper{1, true, false, false};
    QCOMPARE(HtmlText::stripTags(QStringLiteral("<b>A&nbsp;B</b>"), mapper),
             QStringLiteral("A B"));
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&#x4E07;"), mapper),
             QStringLiteral("&#x4E07;"));
    QCOMPARE(HtmlText::decodeEntities(QStringLiteral("&apos;"), mapper),
             QStringLiteral("&apos;"));
}

void CoreLogicTest::pageMediaExtractorReadsPlayerConfig()
{
    // 红牛资源的真实播放页结构：DPlayer 把直链放在 const vid 里。
    const QUrl page(QStringLiteral("https://hn.bfvvs.com/play/oQeZw2ap"));
    const QString playerHtml = QStringLiteral(
        "<script>const vid = 'https://hn.bfvvs.com/play/oQeZw2ap/index.m3u8';</script>");
    const QUrl media = PageMediaExtractor::extractMediaUrl(playerHtml, page);
    QCOMPARE(media.toString(),
             QStringLiteral("https://hn.bfvvs.com/play/oQeZw2ap/index.m3u8"));

    // 传统 MacCMS 播放器配置里是 JSON，斜杠和 & 可能被转义。
    const QString escaped = QStringLiteral(
        "{\"url\":\"https:\\/\\/cdn.example.com\\/hls\\/index.m3u8?token=1\\u0026e=2\"}");
    QCOMPARE(PageMediaExtractor::extractMediaUrl(escaped,
                                                 QUrl(QStringLiteral("https://site.example/")))
                 .toString(),
             QStringLiteral("https://cdn.example.com/hls/index.m3u8?token=1&e=2"));

    // 相对路径按播放页地址补全；非 HTTP 协议必须拒绝。
    QCOMPARE(PageMediaExtractor::extractMediaUrl(
                 QStringLiteral("<source src=\"../video/a.mp4\">"),
                 QUrl(QStringLiteral("https://cdn.example.com/play/1/index.html")))
                 .toString(),
             QStringLiteral("https://cdn.example.com/play/video/a.mp4"));
    QVERIFY(PageMediaExtractor::extractMediaUrl(
                QStringLiteral("var url = 'javascript:alert(1)'"), page)
                .isEmpty());

    QVERIFY(MediaPageResolver::isPlaybackPage(page));
    QVERIFY(MediaPageResolver::isPlaybackPage(
        QUrl(QStringLiteral("https://site.example/vodplay/123-1-1.html"))));
    QVERIFY(!MediaPageResolver::isPlaybackPage(
        QUrl(QStringLiteral("https://site.example/hls/index.m3u8"))));

    MediaItem video;
    video.type = MediaType::Video;
    video.url = page;
    QVERIFY(MediaPageResolver::needsResolution(video));
    video.url = QUrl(QStringLiteral("https://site.example/hls/index.m3u8"));
    QVERIFY(!MediaPageResolver::needsResolution(video));
}

void CoreLogicTest::mediaExtensionsKeepCategorySets()
{
    QCOMPARE(MediaExtensions::suffixFromUrl(
                 QUrl(QStringLiteral("https://cdn.example/a.MP3?next=.mp4"))),
             QStringLiteral("mp3"));
    QVERIFY(MediaExtensions::isCacheSafeSuffix(QStringLiteral("oga")));
    QVERIFY(!MediaExtensions::isCacheSafeSuffix(QStringLiteral("m4s")));
    QVERIFY(MediaExtensions::isMediaContentSuffix(QStringLiteral("m4s")));
    QVERIFY(MediaExtensions::isDirectLinkSuffix(QStringLiteral("mpd")));
    QVERIFY(!MediaExtensions::isCacheSafeSuffix(QStringLiteral("mpd")));
    QVERIFY(MediaExtensions::isAudioSuffix(QStringLiteral("wma")));
    QVERIFY(MediaExtensions::isLosslessAudioSuffix(QStringLiteral("ape")));
    QVERIFY(!MediaExtensions::isLosslessAudioSuffix(QStringLiteral("mp3")));
}

void CoreLogicTest::downloadHlsVerifiesSegmentsAndMapRange()
{
    HlsDownloadServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("movie.mp4"));

    HttpClient http;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/index.m3u8")
                       .arg(server.serverPort()));
    DownloadTask *task = DownloadTask::start(&http, url, target);
    QVERIFY(task != nullptr);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);

    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 8000);
    QCOMPARE(failed.count(), 0);
    QVERIFY(succeeded.count() == 1);

    // 目标原本是 .mp4，识别为 fMP4（EXT-X-MAP）后仍应保持 .mp4，
    // 不能把 init+m4s 的容器错误命名成 .ts。
    const QString output = dir.filePath(QStringLiteral("movie.mp4"));
    QVERIFY2(QFile::exists(output), qPrintable(output));
    QFile file(output);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.size(), qint64(server.initBody.mid(0, 10).size()
                                 + server.segment0.size() + server.segment1.size()));
    QCOMPARE(file.readAll(), server.initBody.mid(0, 10) + server.segment0 + server.segment1);

    // EXT-X-MAP 的 BYTERANGE="10@0" 必须变成 bytes=0-9，而不是整段 init。
    QVERIFY(server.initRangeReceived);
    QCOMPARE(server.initRangeStart, qint64(0));
    QCOMPARE(server.initRangeEnd, qint64(9));
}

void CoreLogicTest::aes128CbcDecryptsKnownVector()
{
    const QByteArray ciphertext = QByteArray::fromHex(
        "39D963D814C82E03B417192899964797DB6FC66358A7D270F6B5909E0725FD68E4"
        "C72C4C4114FDEC1B4C607CAE2AD368");
    const QByteArray key = QByteArray::fromHex("000102030405060708090A0B0C0D0E0F");
    const QByteArray iv = QByteArray::fromHex("101112131415161718191A1B1C1D1E1F");

    QByteArray plain;
    QString error;
    QVERIFY2(Aes128Cbc::decrypt(ciphertext, key, iv, &plain, &error), qPrintable(error));
    QCOMPARE(plain, QByteArrayLiteral("AES-128 HLS test segment payload."));

    QVERIFY(!Aes128Cbc::decrypt(ciphertext, key.left(15), iv, &plain, &error));
    QVERIFY(!Aes128Cbc::decrypt(ciphertext.left(47), key, iv, &plain, &error));
}

void CoreLogicTest::downloadHlsDecryptsAes128Segments()
{
    AesHlsDownloadServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("movie.mp4"));
    const QString base = server.baseUrl();

    HttpClient http;
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), base);
    const QUrl url(base + QStringLiteral("master.m3u8"));
    DownloadTask *task = DownloadTask::start(&http, url, target, headers);
    QVERIFY(task != nullptr);
    task->setOriginUrl(QUrl(base));
    // 模拟播放缓存：上限是 qint64 最大值。这里曾因 remaining + 16 溢出，
    // 把加密分片缓冲上限算成 0，导致第一个分片就被误判超限。
    task->setMaxBytes(std::numeric_limits<qint64>::max());

    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 8000);
    QCOMPARE(failed.count(), 0);
    QVERIFY(succeeded.count() == 1);

    const QString output = succeeded.at(0).at(0).toString();
    QFile file(output);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(output));
    QCOMPARE(file.readAll(), server.plain0 + server.plain1);

    // 两个不同 KEY URI 各请求一次；默认 IV 与显式 IV 都必须解出正确明文。
    QCOMPARE(server.key1Requests, 1);
    QCOMPARE(server.key2Requests, 1);
    QCOMPARE(server.lastKeyReferer, base.toUtf8());
}

void CoreLogicTest::hlsCacheManifestKeepsSessionKeyAndDisablesEncryptedSingleFile()
{
    HlsCacheManifest::Master master;
    QString error;
    const QByteArray masterBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-SESSION-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000\n"
        "media.m3u8\n");
    QVERIFY2(HlsCacheManifest::parseMaster(masterBody, QUrl(QStringLiteral("http://example.com/")),
                                            &master, &error),
             qPrintable(error));
    QVERIFY(master.encrypted);
    QCOMPARE(master.sessionKeyLines.size(), 1);
    QVERIFY(master.sessionKeyLines.first().contains(QLatin1String("METHOD=AES-128")));

    HlsCacheManifest::MediaTrack track;
    const QByteArray mediaBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXT-X-BYTERANGE:10@0\n"
        "#EXTINF:1,\n"
        "all.mp4\n"
        "#EXT-X-BYTERANGE:10@10\n"
        "#EXTINF:1,\n"
        "all.mp4\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY2(HlsCacheManifest::parseMediaTrack(mediaBody,
                                               QUrl(QStringLiteral("http://example.com/media.m3u8")),
                                               &track, &error),
             qPrintable(error));
    QVERIFY(track.encrypted);
    QVERIFY(!track.singleFile);
    QVERIFY(track.singleFileUrl.isEmpty());

    // AES-128 行结构异常（缺 URI）时仍要保守标记加密，避免缓存层误判为明文单文件。
    HlsCacheManifest::MediaTrack malformedKey;
    const QByteArray malformedBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128\n"
        "#EXTINF:1,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY2(HlsCacheManifest::parseMediaTrack(malformedBody,
                                               QUrl(QStringLiteral("http://example.com/media.m3u8")),
                                               &malformedKey, &error),
             qPrintable(error));
    QVERIFY(malformedKey.encrypted);
    QVERIFY(!malformedKey.singleFile);
}

void CoreLogicTest::hlsCacheManifestDetectsFragmentedMp4Segments()
{
    const QUrl playlistUrl(QStringLiteral("http://example.com/media.m3u8"));
    QString error;

    // EXT-X-MAP 是 fMP4 的明确标志；此时分片索引必须按 MP4 类型返回，
    // 不能沿用 TS 的 video/mp2t。
    HlsCacheManifest::MediaTrack mapped;
    const QByteArray mapBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:1,\n"
        "seg1.mp4\n"
        "#EXTINF:1,\n"
        "seg2.mp4\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY2(HlsCacheManifest::parseMediaTrack(mapBody, playlistUrl, &mapped, &error),
             qPrintable(error));
    QVERIFY(mapped.fragmentedMp4);
    QCOMPARE(mapped.segmentCount, 2);
    QVERIFY(!mapped.singleFile);

    // 没有 MAP 时，m4s/m4f 后缀同样说明是 fMP4 分片。
    HlsCacheManifest::MediaTrack suffixed;
    const QByteArray suffixBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXTINF:1,\n"
        "seg1.m4s\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY2(HlsCacheManifest::parseMediaTrack(suffixBody, playlistUrl, &suffixed, &error),
             qPrintable(error));
    QVERIFY(suffixed.fragmentedMp4);

    // 普通 TS 列表不能被误判。
    HlsCacheManifest::MediaTrack ts;
    const QByteArray tsBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXTINF:1,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY2(HlsCacheManifest::parseMediaTrack(tsBody, playlistUrl, &ts, &error),
             qPrintable(error));
    QVERIFY(!ts.fragmentedMp4);
}

void CoreLogicTest::downloadRejectsHtmlDisguisedAsOctetStream()
{
    // 不少站点把登录页/错误页包成 application/octet-stream；只按 MIME 放行
    // 会把网页存成 .mp3。这里验证首块魔数校验仍会拒绝。
    ProbeSiteServer server([](const QString &, QByteArray &body, QByteArray &contentType,
                              int &status) {
        status = 200;
        contentType = QByteArrayLiteral("application/octet-stream");
        body = QByteArrayLiteral("<!DOCTYPE html><html><body>请登录</body></html>");
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("song.mp3"));
    HttpClient http;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/song.mp3")
                       .arg(server.serverPort()));
    DownloadTask *task = DownloadTask::start(&http, url, target);
    QVERIFY(task != nullptr);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);

    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 8000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!QFile::exists(target));
}

void CoreLogicTest::downloadUsesContentRangeEndWhenTotalUnknown()
{
    // 206 的 Content-Range 只给 `0-9/*`、总长未知时，end+1 就是最终长度；
    // 实际只返回 5 字节必须判失败，不能仅因为 Content-Length 是 5 就提交半截文件。
    ProbeSiteServer server([](const QString &, QByteArray &body, QByteArray &contentType,
                              int &status) {
        status = 206;
        contentType = QByteArrayLiteral("application/octet-stream");
        body = QByteArray("ID3\x04\x00", 5);
    });
    server.m_extraHeaders = QByteArrayLiteral("Content-Range: bytes 0-9/*\r\n");
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("clip.mp3"));
    HttpClient http;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/clip.mp3")
                       .arg(server.serverPort()));
    DownloadTask *task = DownloadTask::start(&http, url, target);
    QVERIFY(task != nullptr);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);

    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 8000);
    QCOMPARE(succeeded.count(), 0);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!QFile::exists(target));
}

void CoreLogicTest::downloadResumesMidFileWithoutMagicFalsePositive()
{
    // octet-stream + 无媒体后缀的续传：首块是文件中段，不是文件头。
    // 不能拿它做魔数校验，否则合法续传会被误判成网页并删掉半成品。
    ProbeSiteServer server([](const QString &, QByteArray &body, QByteArray &contentType,
                              int &status) {
        status = 206;
        contentType = QByteArrayLiteral("application/octet-stream");
        body = QByteArrayLiteral("abcde");
    });
    server.m_extraHeaders = QByteArrayLiteral("Content-Range: bytes 5-9/10\r\n");
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("clip.bin"));
    QFile partial(target + QStringLiteral(".mvpdownload"));
    QVERIFY(partial.open(QIODevice::WriteOnly));
    QCOMPARE(partial.write(QByteArrayLiteral("PREFX")), qint64(5));
    partial.close();

    HttpClient http;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/clip")
                       .arg(server.serverPort()));
    DownloadTask *task = DownloadTask::start(&http, url, target);
    QVERIFY(task != nullptr);
    task->setResumeState(5, 0);
    QSignalSpy succeeded(task, &DownloadTask::succeeded);
    QSignalSpy failed(task, &DownloadTask::failed);

    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 8000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(succeeded.count(), 1);

    QFile file(target);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("PREFXabcde"));
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

    // all/any 是可编辑规则，不能无限递归；超过 64 层按不匹配处理。
    QJsonObject deep{{QStringLiteral("path"), QStringLiteral("is_streamable")},
                     {QStringLiteral("op"), QStringLiteral("eq")},
                     {QStringLiteral("value"), true}};
    for (int i = 0; i < 70; ++i)
        deep = QJsonObject{{QStringLiteral("all"), QJsonArray{deep}}};
    QVERIFY(!ConfigMapper::ruleMatches(item, deep));
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

    // 远程配置 JSON 不能把播放器指向本机文件、UNC 共享或其他协议。
    QVERIFY(ConfigMapper::normalizeUrl(QStringLiteral("file:///C:/secret.mp4")).isEmpty());
    QVERIFY(ConfigMapper::normalizeUrl(QStringLiteral("\\\\server\\share\\secret.mp4")).isEmpty());
    QVERIFY(ConfigMapper::normalizeUrl(QStringLiteral("smb://server/share/a.ts")).isEmpty());
    QVERIFY(ConfigMapper::normalizeUrl(QStringLiteral("data:text/plain,hello")).isEmpty());

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

void CoreLogicTest::streamProxyResumesTruncatedCache()
{
    TruncatingRangeServer upstream;
    upstream.body = proxyTestBody(1000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/audio/truncated.mp3").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));
    socket.write("GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                 "Host: 127.0.0.1\r\nConnection: close\r\n\r\n");
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 200"), head.constData());
    QCOMPARE(body, upstream.body);

    QTRY_COMPARE_WITH_TIMEOUT(upstream.requestCount, 2, 6000);
    QVERIFY2(upstream.ranges.size() >= 2, qPrintable(QString::number(upstream.ranges.size())));
    QVERIFY(upstream.ranges.at(1) > 0);
    QVERIFY(upstream.ranges.at(1) <= upstream.body.size());
}

void CoreLogicTest::streamProxyResumesTruncatedPartialCache()
{
    TruncatingRangeServer upstream;
    upstream.partialInitial = true;
    upstream.body = proxyTestBody(1000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/audio/partial-truncated.mp3").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));
    // 播放器对音频常用 `Range: bytes=0-`，网易云等 CDN 会回 206。
    // 这条必须和 200 全量路径一样进入缓存并在断流后续传。
    socket.write("GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                 "Host: 127.0.0.1\r\nRange: bytes=0-\r\nConnection: close\r\n\r\n");
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QCOMPARE(body, upstream.body);

    QTRY_COMPARE_WITH_TIMEOUT(upstream.requestCount, 2, 6000);
    QVERIFY(upstream.ranges.size() >= 2);
    QVERIFY(upstream.ranges.at(1) > 0);
}

void CoreLogicTest::streamProxyWritesCompleteDiskCache()
{
    // 数据目录用临时路径，避免测试把缓存写进用户真实的 Data/media-cache。
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    const bool hadDataDir = qEnvironmentVariableIsSet("MVP_DATA_DIR");
    struct EnvGuard
    {
        QByteArray oldValue;
        bool had = false;
        ~EnvGuard()
        {
            if (had)
                qputenv("MVP_DATA_DIR", oldValue);
            else
                qunsetenv("MVP_DATA_DIR");
        }
    } guard{oldDataDir, hadDataDir};
    qputenv("MVP_DATA_DIR", dataDir.path().toUtf8());

    TruncatingRangeServer upstream;
    upstream.partialInitial = true;
    upstream.body = proxyTestBody(1000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/audio/disk-cache.mp3").arg(upstream.serverPort());
    MediaItem item;
    item.type = MediaType::Music;
    item.title = QStringLiteral("磁盘缓存测试");
    item.url = upstreamUrl;
    item.online = true;
    QVERIFY(MediaCache::isCacheable(item));

    const QString cachePath = MediaCache::filePathForItem(item);
    QVERIFY(!cachePath.isEmpty());
    QVERIFY(cachePath.contains(QStringLiteral("media-cache")));
    // 音乐正文直接放缓存根目录，一个条目一个文件，且不带后缀。
    QCOMPARE(QFileInfo(cachePath).absolutePath(),
             QDir::cleanPath(MediaCache::rootDir()));
    QVERIFY(QFileInfo(cachePath).suffix().isEmpty());
    QVERIFY(QFileInfo(cachePath).fileName() != QStringLiteral("media.mp3"));

    auto fetchAll = [](const QUrl &local, QByteArray *head, QByteArray *body) {
        QTcpSocket socket;
        socket.connectToHost(local.host(), quint16(local.port()));
        if (!socket.waitForConnected(3000))
            return false;
        socket.write("GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\nConnection: close\r\n\r\n");
        socket.flush();
        return readProxyResponse(&socket, head, body);
    };

    {
        StreamProxy proxy;
        QVERIFY(proxy.start());
        const QUrl local =
            proxy.proxyUrlFor(QUrl(upstreamUrl), {}, cachePath, MediaCache::cacheIdentity(item));
        QVERIFY(local.isValid());

        QByteArray head;
        QByteArray body;
        QVERIFY(fetchAll(local, &head, &body));
        QVERIFY2(head.startsWith("HTTP/1.1 200"), head.constData());
        QCOMPARE(body, upstream.body);

        // 上游先只给 300 字节再断流，代理应落盘并从 300 续传补齐。
        QTRY_COMPARE_WITH_TIMEOUT(upstream.requestCount, 2, 6000);
        QVERIFY(upstream.ranges.size() >= 2);
        QCOMPARE(upstream.ranges.at(1), qint64(300));
        QTRY_COMPARE_WITH_TIMEOUT(MediaCache::fileSize(cachePath), qint64(1000), 3000);

        MediaCache::State state;
        QVERIFY(MediaCache::readState(cachePath, &state));
        QVERIFY(state.complete);
        QCOMPARE(state.totalBytes, qint64(1000));
        QCOMPARE(MediaCache::completeFileForItem(item), cachePath);
    }

    // 新进程/新代理再次请求同一 URL：完整磁盘缓存直接供流，不再打上游。
    const int requestsAfterDownload = upstream.requestCount;
    {
        StreamProxy proxy;
        QVERIFY(proxy.start());
        const QUrl local =
            proxy.proxyUrlFor(QUrl(upstreamUrl), {}, cachePath, MediaCache::cacheIdentity(item));
        QVERIFY(local.isValid());

        QByteArray head;
        QByteArray body;
        QVERIFY(fetchAll(local, &head, &body));
        QVERIFY2(head.startsWith("HTTP/1.1 200"), head.constData());
        QCOMPARE(body, upstream.body);
        QCOMPARE(upstream.requestCount, requestsAfterDownload);
    }
}

void CoreLogicTest::streamProxyResumesDiskCacheFromPartialFile()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    const bool hadDataDir = qEnvironmentVariableIsSet("MVP_DATA_DIR");
    struct EnvGuard
    {
        QByteArray oldValue;
        bool had = false;
        ~EnvGuard()
        {
            if (had)
                qputenv("MVP_DATA_DIR", oldValue);
            else
                qunsetenv("MVP_DATA_DIR");
        }
    } guard{oldDataDir, hadDataDir};
    qputenv("MVP_DATA_DIR", dataDir.path().toUtf8());

    RangeSupportingServer upstream;
    upstream.body = proxyTestBody(1000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/video/resume-cache.mp4").arg(upstream.serverPort());
    MediaItem item;
    item.type = MediaType::Video;
    item.title = QStringLiteral("断点续传测试");
    item.url = upstreamUrl;
    item.online = true;
    QVERIFY(MediaCache::isCacheable(item));

    const QString cachePath = MediaCache::filePathForItem(item);
    QVERIFY(QDir().mkpath(QFileInfo(cachePath).absolutePath()));
    {
        QFile file(cachePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(upstream.body.left(300)) == 300);
    }
    QVERIFY(MediaCache::writeState(cachePath, MediaCache::cacheIdentity(item), QUrl(upstreamUrl),
                                   "video/mp4", 1000, false));

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl local =
        proxy.proxyUrlFor(QUrl(upstreamUrl), {}, cachePath, MediaCache::cacheIdentity(item));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));
    socket.write("GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                 "Host: 127.0.0.1\r\nRange: bytes=0-\r\nConnection: close\r\n\r\n");
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QCOMPARE(body, upstream.body);

    // 已有 300 字节时必须从 300 请求上游，绝不能重新从 0 下载覆盖前缀。
    QCOMPARE(upstream.requestCount, 1);
    QVERIFY(!upstream.ranges.isEmpty());
    QCOMPARE(upstream.ranges.first().first, qint64(300));
    QTRY_COMPARE_WITH_TIMEOUT(MediaCache::fileSize(cachePath), qint64(1000), 3000);

    MediaCache::State state;
    QVERIFY(MediaCache::readState(cachePath, &state));
    QVERIFY(state.complete);
    QCOMPARE(state.totalBytes, qint64(1000));
}

void CoreLogicTest::mediaCacheLayoutForMusicAndVideo()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    const bool hadDataDir = qEnvironmentVariableIsSet("MVP_DATA_DIR");
    struct EnvGuard
    {
        QByteArray oldValue;
        bool had = false;
        ~EnvGuard()
        {
            if (had)
                qputenv("MVP_DATA_DIR", oldValue);
            else
                qunsetenv("MVP_DATA_DIR");
        }
    } guard{oldDataDir, hadDataDir};
    qputenv("MVP_DATA_DIR", dataDir.path().toUtf8());

    MediaItem music;
    music.type = MediaType::Music;
    music.title = QStringLiteral("Lover");
    music.artist = QStringLiteral("Taylor Swift");
    music.url = QStringLiteral("https://example.com/song.mp3");
    music.online = true;
    const QString musicPath = MediaCache::filePathForItem(music);
    // 音乐：缓存根目录下一条目一个正文文件，正文不带后缀；后缀只进同目录 JSON。
    QCOMPARE(QDir::cleanPath(QFileInfo(musicPath).absolutePath()),
             QDir::cleanPath(MediaCache::rootDir()));
    QVERIFY(QFileInfo(musicPath).suffix().isEmpty());
    QVERIFY(musicPath.endsWith(QStringLiteral("Lover - Taylor Swift")));
    QCOMPARE(MediaCache::metaPathForFile(musicPath), musicPath + QStringLiteral(".json"));
    {
        QVERIFY(QDir().mkpath(MediaCache::rootDir()));
        QFile first(musicPath);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QVERIFY(first.write("music") > 0);
    }
    QVERIFY(MediaCache::writeState(musicPath, MediaCache::cacheIdentity(music), QUrl(music.url),
                                   "audio/mpeg", 5, false));

    // 同名不同地址必须自动让位到 "(2)"，不能覆盖另一个条目的缓存。
    MediaItem musicSameName;
    musicSameName.type = MediaType::Music;
    musicSameName.title = music.title;
    musicSameName.artist = music.artist;
    musicSameName.url = QStringLiteral("https://example.com/song-2.mp3");
    musicSameName.online = true;
    const QString secondPath = MediaCache::filePathForItem(musicSameName);
    QVERIFY2(secondPath.endsWith(QStringLiteral("Lover - Taylor Swift (2)")),
             qPrintable(secondPath));
    QVERIFY(MediaCache::cacheIdentity(music) != MediaCache::cacheIdentity(musicSameName));

    // 同一外链路径、不同歌曲 id 必须得到不同缓存身份；这正是旧实现
    // “所有网易云歌曲共用一个缓存目录”的根因。
    MediaItem songA = music;
    songA.url = QStringLiteral("https://music.163.com/song/media/outer/url?id=1.mp3");
    MediaItem songB = music;
    songB.url = QStringLiteral("https://music.163.com/song/media/outer/url?id=2.mp3");
    QVERIFY(MediaCache::cacheIdentity(songA) != MediaCache::cacheIdentity(songB));

    // 删除重试的真实场景：正文被短暂占用，第一轮删正文失败后 JSON 已被删掉。
    // 下一次按“无 JSON 的残留正文”仍要定位到同一个文件，不能跳到 (3)。
    {
        QFile second(secondPath);
        QVERIFY(second.open(QIODevice::WriteOnly));
        QVERIFY(second.write("second") > 0);
    }
    QVERIFY(MediaCache::writeState(secondPath,
                                   MediaCache::cacheIdentity(musicSameName),
                                   QUrl(musicSameName.url), "audio/mpeg", 6, false));
    QVERIFY(QFile::remove(MediaCache::metaPathForFile(secondPath)));
    QVERIFY(MediaCache::removeForItem(musicSameName));
    QVERIFY(!QFileInfo::exists(secondPath));

    QVERIFY(MediaCache::removeForItem(music));
    QVERIFY(!QFileInfo::exists(musicPath));
    QVERIFY(!QFileInfo::exists(MediaCache::metaPathForFile(musicPath)));

    // 升级兼容：第一轮的 32 位哈希目录要能自动搬到新布局，而不是变成孤儿。
    MediaItem legacy;
    legacy.type = MediaType::Music;
    legacy.title = QStringLiteral("旧缓存");
    legacy.url = QStringLiteral("https://example.com/legacy.mp3");
    legacy.online = true;
    const QString legacyDir =
        MediaCache::rootDir() + QLatin1Char('/')
        + QCryptographicHash::hash(mediaIdentity(legacy).toUtf8(), QCryptographicHash::Sha256)
              .toHex()
              .left(32);
    QVERIFY(QDir().mkpath(legacyDir));
    QFile legacyData(legacyDir + QStringLiteral("/media.mp3"));
    QVERIFY(legacyData.open(QIODevice::WriteOnly));
    QVERIFY(legacyData.write(QByteArray("old-cache-body")) > 0);
    legacyData.close();
    QJsonObject oldMeta;
    oldMeta.insert(QStringLiteral("version"), 1);
    oldMeta.insert(QStringLiteral("url"), legacy.url.toString());
    oldMeta.insert(QStringLiteral("contentType"), QStringLiteral("audio/mpeg"));
    oldMeta.insert(QStringLiteral("total"), 14);
    oldMeta.insert(QStringLiteral("complete"), true);
    {
        QFile meta(legacyDir + QStringLiteral("/meta.json"));
        QVERIFY(meta.open(QIODevice::WriteOnly));
        QVERIFY(meta.write(QJsonDocument(oldMeta).toJson(QJsonDocument::Compact)) > 0);
    }
    const QString adopted = MediaCache::completeFileForItem(legacy);
    QVERIFY(!adopted.isEmpty());
    QVERIFY(!QDir(legacyDir).exists());
    QFile adoptedFile(adopted);
    QVERIFY(adoptedFile.open(QIODevice::ReadOnly));
    QCOMPARE(adoptedFile.readAll(), QByteArray("old-cache-body"));
    MediaCache::State adoptedState;
    QVERIFY(MediaCache::readState(adopted, &adoptedState));
    QCOMPARE(adoptedState.extension, QStringLiteral(".mp3"));

    // 影视：一条目一个文件夹，正文固定 media、不带后缀，并过滤非法字符。
    MediaItem video;
    video.type = MediaType::Video;
    video.title = QStringLiteral("某影视: 第一集?");
    video.url = QStringLiteral("https://example.com/video.mp4");
    video.online = true;
    const QString videoDir = MediaCache::directoryForItem(video);
    const QString folderName = QFileInfo(videoDir).fileName();
    QVERIFY2(folderName.startsWith(QStringLiteral("某影视_ 第一集_")), qPrintable(folderName));
    QVERIFY(!folderName.contains(QStringLiteral("__")));
    QVERIFY(!folderName.contains(QLatin1Char(':')));
    QVERIFY(!folderName.contains(QLatin1Char('?')));
    QVERIFY(QDir().mkpath(videoDir));
    const QString videoPath = videoDir + QStringLiteral("/media");
    QVERIFY(QFileInfo(videoPath).suffix().isEmpty());

    QFile payload(videoPath);
    QVERIFY(payload.open(QIODevice::WriteOnly));
    QVERIFY(payload.write("fake-video") > 0);
    payload.close();
    QVERIFY(MediaCache::writeState(videoPath, MediaCache::cacheIdentity(video), QUrl(video.url),
                                   "video/mp4", 10, true));
    MediaCache::State state;
    QVERIFY(MediaCache::readState(videoPath, &state));
    QCOMPARE(state.extension, QStringLiteral(".mp4"));
    QCOMPARE(MediaCache::completeFileForItem(video), videoPath);

    QVERIFY(MediaCache::removeForItem(video));
    QVERIFY(!QDir(videoDir).exists());

    // 目录不存在时删除也算成功，删除接口可以安全地重复调用。
    QVERIFY(MediaCache::removeForItem(video));

    // 影视目录里的 JSON 先被删掉、正文仍因句柄占用留下时，重试也必须删掉
    // 整个条目目录，而不是跳到下一个同名候选。
    QVERIFY(QDir().mkpath(videoDir));
    QFile orphanVideo(videoPath);
    QVERIFY(orphanVideo.open(QIODevice::WriteOnly));
    QVERIFY(orphanVideo.write("orphan") > 0);
    orphanVideo.close();
    QVERIFY(MediaCache::removeForItem(video));
    QVERIFY(!QDir(videoDir).exists());
}

void CoreLogicTest::hlsCompleteCacheRecognition()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    const bool hadDataDir = qEnvironmentVariableIsSet("MVP_DATA_DIR");
    struct EnvGuard
    {
        QByteArray oldValue;
        bool had = false;
        ~EnvGuard()
        {
            if (had)
                qputenv("MVP_DATA_DIR", oldValue);
            else
                qunsetenv("MVP_DATA_DIR");
        }
    } guard{oldDataDir, hadDataDir};
    qputenv("MVP_DATA_DIR", dataDir.path().toUtf8());

    MediaItem hls;
    hls.type = MediaType::Video;
    hls.title = QStringLiteral("HLS 完整缓存");
    hls.url = QUrl(QStringLiteral("https://example.com/show/index.m3u8?token=abc"));
    hls.online = true;

    // HLS 不是可按 Range 读取的直链，但合并转封装后的 MP4 必须能被当成
    // 完整缓存命中；否则每次重播都会重新拉全部分片。
    QVERIFY(!MediaCache::isCacheable(hls));
    const QString bodyPath = MediaCache::filePathForItem(hls);
    QVERIFY(QDir().mkpath(QFileInfo(bodyPath).absolutePath()));
    QFile body(bodyPath);
    QVERIFY(body.open(QIODevice::WriteOnly));
    QVERIFY(body.write("fake-mp4-data") > 0);
    body.close();
    QCOMPARE(QFileInfo(bodyPath).suffix(), QString());

    QVERIFY(MediaCache::writeState(bodyPath, MediaCache::cacheIdentity(hls), hls.url,
                                   "video/mp4", QFileInfo(bodyPath).size(), true,
                                   QStringLiteral("mp4")));
    MediaCache::State state;
    QVERIFY(MediaCache::readState(bodyPath, &state));
    QCOMPARE(state.extension, QStringLiteral(".mp4"));
    QCOMPARE(MediaCache::completeFileForItem(hls), bodyPath);

    QVERIFY(MediaCache::removeForItem(hls));
    QVERIFY(!QFileInfo::exists(bodyPath));
}

void CoreLogicTest::hlsCacheManifestParsesMasterAndSingleFile()
{
    const QUrl masterUrl(QStringLiteral("https://cdn.example.com/fmp4/master.m3u8"));
    const QByteArray masterBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud1\",NAME=\"English\",DEFAULT=YES,"
        "URI=\"a1/prog_index.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=82753463,RESOLUTION=1920x1080,CODECS=\"hvc1\" "
        "AUDIO=\"aud1\"\n"
        "v4/prog_index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=585499767,RESOLUTION=3840x2160,CODECS=\"hvc1\" "
        "AUDIO=\"aud1\"\n"
        "v9/prog_index.m3u8\n");
    HlsCacheManifest::Master master;
    QString error;
    QVERIFY2(HlsCacheManifest::parseMaster(masterBody, masterUrl, &master, &error),
             qPrintable(error));
    QCOMPARE(master.variants.size(), 2);
    QCOMPARE(master.variants.at(0).bandwidth, 82753463);
    QCOMPARE(master.variants.at(0).resolution, QStringLiteral("1920x1080"));
    QCOMPARE(master.variants.at(0).audioGroup, QStringLiteral("aud1"));
    QCOMPARE(master.variants.at(0).url, QUrl(QStringLiteral(
                 "https://cdn.example.com/fmp4/v4/prog_index.m3u8")));
    QCOMPARE(master.audioRenditions.size(), 1);
    QCOMPARE(master.audioRenditions.at(0).groupId, QStringLiteral("aud1"));
    QVERIFY(master.audioRenditions.at(0).isDefault);
    QCOMPARE(master.audioRenditions.at(0).url, QUrl(QStringLiteral(
                 "https://cdn.example.com/fmp4/a1/prog_index.m3u8")));

    const QUrl videoUrl(QStringLiteral(
        "https://cdn.example.com/fmp4/v4/prog_index.m3u8?token=abc"));
    const QByteArray singleFileBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MAP:URI=\"main.mp4\",BYTERANGE=\"718@0\"\n"
        "#EXTINF:4.0,\n"
        "#EXT-X-BYTERANGE:830431@718\n"
        "main.mp4\n"
        "#EXTINF:4.0,\n"
        "#EXT-X-BYTERANGE:830431@831149\n"
        "main.mp4\n"
        "#EXTINF:4.0,\n"
        "#EXT-X-BYTERANGE:830431@1661580\n"
        "main.mp4\n"
        "#EXT-X-ENDLIST\n");
    HlsCacheManifest::MediaTrack track;
    QVERIFY2(HlsCacheManifest::parseMediaTrack(singleFileBody, videoUrl, &track, &error),
             qPrintable(error));
    QVERIFY(track.valid);
    QVERIFY(track.hasEndList);
    QCOMPARE(track.segmentCount, 3);
    QVERIFY(track.singleFile);
    QCOMPARE(track.singleFileUrl, QUrl(QStringLiteral(
                 "https://cdn.example.com/fmp4/v4/main.mp4")));

    // 普通分片即使 URL 相同也不能走“整文件 Range”快路径；
    // 只有每片都声明 BYTERANGE 才是单文件 HLS。
    const QByteArray segmentedBody = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1,\n"
        "seg0.ts\n"
        "#EXTINF:1,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n");
    HlsCacheManifest::MediaTrack segmented;
    QVERIFY(HlsCacheManifest::parseMediaTrack(segmentedBody, videoUrl, &segmented, &error));
    QVERIFY(!segmented.singleFile);
    QCOMPARE(segmented.segmentCount, 2);

    QVERIFY(HlsCacheManifest::sameUrlIgnoringQuery(
        QUrl(QStringLiteral("https://cdn.example.com/fmp4/v4/prog.m3u8?token=one")),
        QUrl(QStringLiteral("https://cdn.example.com/fmp4/v4/prog.m3u8?token=two"))));
    QVERIFY(!HlsCacheManifest::sameUrlIgnoringQuery(
        QUrl(QStringLiteral("https://cdn.example.com/fmp4/v4/prog.m3u8")),
        QUrl(QStringLiteral("https://cdn.example.com/fmp4/v9/prog.m3u8"))));
    // id/seq 是能区分资源的稳定参数，不能因为“忽略 query”被当成同一文件。
    QVERIFY(!HlsCacheManifest::sameUrlIgnoringQuery(
        QUrl(QStringLiteral("https://cdn.example.com/fmp4/prog.m3u8?id=1&token=one")),
        QUrl(QStringLiteral("https://cdn.example.com/fmp4/prog.m3u8?id=2&token=two"))));

    // 有 STREAM-INF 但没有任何可用变体地址时，必须返回 false 并带错误，
    // 不能返回 true 让调用方拿到空 variants 继续执行。
    HlsCacheManifest::Master emptyMaster;
    QString emptyError;
    QVERIFY(!HlsCacheManifest::parseMaster(
        QByteArrayLiteral("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000\n"),
        masterUrl, &emptyMaster, &emptyError));
    QVERIFY(emptyMaster.variants.isEmpty());
    QVERIFY(!emptyError.isEmpty());
}

void CoreLogicTest::lyricsCacheRoundTrip()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QByteArray oldDataDir = qgetenv("MVP_DATA_DIR");
    const bool hadDataDir = qEnvironmentVariableIsSet("MVP_DATA_DIR");
    struct EnvGuard
    {
        QByteArray oldValue;
        bool had = false;
        ~EnvGuard()
        {
            if (had)
                qputenv("MVP_DATA_DIR", oldValue);
            else
                qunsetenv("MVP_DATA_DIR");
        }
    } guard{oldDataDir, hadDataDir};
    qputenv("MVP_DATA_DIR", dataDir.path().toUtf8());

    MediaItem music;
    music.type = MediaType::Music;
    music.title = QStringLiteral("缓存歌词");
    music.artist = QStringLiteral("测试");
    music.url = QUrl(QStringLiteral("https://music.example.com/song.mp3?id=42"));
    music.lyricUrl = QUrl(QStringLiteral("https://music.example.com/api/lyric?id=42"));
    music.online = true;

    const QByteArray payload =
        QByteArrayLiteral("{\"lrc\":{\"lyric\":\"[00:01.00]第一句\\n[00:02.00]第二句\"}}");
    QVERIFY(LyricsCache::isEligible(music));
    QVERIFY(LyricsCache::store(music, payload));

    QByteArray loaded;
    QVERIFY(LyricsCache::load(music, &loaded));
    QCOMPARE(loaded, payload);

    const QDir root(LyricsCache::rootDir());
    QVERIFY(root.exists());
    const QStringList files = root.entryList(QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(files.size(), 2);
    bool foundBodyWithoutSuffix = false;
    bool foundMeta = false;
    for (const QString &name : files) {
        if (name.endsWith(QStringLiteral(".json")))
            foundMeta = true;
        else if (QFileInfo(name).suffix().isEmpty())
            foundBodyWithoutSuffix = true;
    }
    QVERIFY(foundBodyWithoutSuffix);
    QVERIFY(foundMeta);

    QVERIFY(LyricsCache::removeForItem(music));
    QVERIFY(root.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());

    // 只有正文、没有旁车 JSON 的孤儿文件也必须能被删除条目回收，
    // 否则它会永久占用基础名，把后续歌词缓存推到 "(2)" 且无法命中。
    const QString orphanPath = root.filePath(MediaCache::entryBaseName(music));
    QFile orphan(orphanPath);
    QVERIFY(orphan.open(QIODevice::WriteOnly));
    orphan.write(payload);
    orphan.close();
    QVERIFY(QFileInfo::exists(orphanPath));
    QVERIFY(LyricsCache::removeForItem(music));
    QVERIFY(!QFileInfo::exists(orphanPath));
    QVERIFY(root.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

void CoreLogicTest::shortcutsDefaultConflictDetection()
{
    const QHash<QString, QString> defaults = AppShortcuts::defaultBindings();
    QCOMPARE(defaults.size(), AppShortcuts::actions().size());
    QVERIFY(AppShortcuts::conflicts(defaults).isEmpty());

    // 同一快捷键只能有一个动作；播放/暂停与全屏/退出全屏在动作表里
    // 本身已是 toggle，不能再拆成两个动作去共用或占用两把按键。
    QHash<QString, QString> duplicate = defaults;
    duplicate.insert(QStringLiteral("stop"),
                     defaults.value(QStringLiteral("playPause")));
    const QVector<AppShortcuts::Conflict> conflicts =
        AppShortcuts::conflicts(duplicate);
    QCOMPARE(conflicts.size(), 1);
    QCOMPARE(conflicts.first().actionIds.size(), 2);
    QVERIFY(!AppShortcuts::conflictMessage(conflicts).isEmpty());

    QCOMPARE(AppShortcuts::normalizeSequence(QStringLiteral("ctrl+s")),
             QStringLiteral("Ctrl+S"));
    QVERIFY(AppShortcuts::normalizeSequence(QStringLiteral("  ")).isEmpty());
}

void CoreLogicTest::streamProxyHandlesSuffixlessTextPlaylist()
{
    // 签名 HLS 常见形态：地址没有 .m3u8，Content-Type 只是 text/plain。
    // 代理必须在 bytes=0- 时仍拉全文并改写分片，不能把原始 m3u8 当媒体缓存。
    RangeIgnoringServer upstream;
    upstream.contentType = QByteArrayLiteral("text/plain; charset=utf-8");
    upstream.body = QByteArrayLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXTINF:1,\n"
        "seg0.ts\n"
        "#EXTINF:1,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n");
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/signed/playlist?token=abc")
            .arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));

    QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Range: bytes=0-\r\n"
                         "Connection: close\r\n\r\n";
    socket.write(request);
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 200"), head.constData());
    QVERIFY(head.contains("Content-Type: application/vnd.apple.mpegurl"));
    QVERIFY(body.contains("#EXTM3U"));
    QVERIFY(body.contains("/stream/"));
    QCOMPARE(upstream.requestCount, 1);
}

void CoreLogicTest::streamProxyDoesNotReuseLocalPrefixEntry()
{
    // 预置 300 字节半截缓存：先发有界 Range 只读前缀，随后整段请求必须
    // 从 300 继续向上游续传。旧实现把前缀条目留在正常缓存表里，第二次
    // 请求会拿到 206 头后停在前缀末尾，连接一直等不到后续字节。
    const QByteArray full = proxyTestBody(1000);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cachePath = dir.filePath(QStringLiteral("partial-cache"));
    {
        QFile file(cachePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(full.left(300)) == 300);
    }

    RangeSupportingServer upstream;
    upstream.body = full;
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/media/partial.mp4").arg(upstream.serverPort());
    QVERIFY(MediaCache::writeState(cachePath, QStringLiteral("partial-identity"),
                                   upstreamUrl, "video/mp2t", full.size(), false,
                                   QStringLiteral(".ts")));
    const QUrl local = proxy.proxyUrlFor(upstreamUrl, {}, cachePath,
                                         QStringLiteral("partial-identity"));
    QVERIFY(local.isValid());

    auto fetch = [&local](const QByteArray &range, QByteArray *head, QByteArray *body) {
        QTcpSocket socket;
        socket.connectToHost(local.host(), quint16(local.port()));
        if (!socket.waitForConnected(3000))
            return false;
        QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Range: " + range + "\r\n"
                             "Connection: close\r\n\r\n";
        socket.write(request);
        socket.flush();
        return readProxyResponse(&socket, head, body, 8000);
    };

    QByteArray head;
    QByteArray body;
    QVERIFY(fetch("bytes=0-200", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QCOMPARE(body, full.left(201));
    QCOMPARE(upstream.requestCount, 0);

    QVERIFY(fetch("bytes=0-", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QCOMPARE(body, full);
    QCOMPARE(upstream.requestCount, 1);
    QCOMPARE(upstream.ranges.first(), qMakePair(qint64(300), qint64(999)));
}

void CoreLogicTest::streamProxyEvictsPrefetchWithoutLeakingSlots()
{
    PrefetchEvictionServer upstream;
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString playlistUrl =
        QStringLiteral("http://127.0.0.1:%1/hls/index.m3u8").arg(upstream.serverPort());
    const QUrl localPlaylist = proxy.proxyUrlFor(QUrl(playlistUrl));
    QVERIFY(localPlaylist.isValid());

    auto fetch = [](const QUrl &local, QByteArray *head, QByteArray *body) {
        QTcpSocket socket;
        socket.connectToHost(local.host(), quint16(local.port()));
        if (!socket.waitForConnected(3000))
            return false;
        QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Connection: close\r\n\r\n";
        socket.write(request);
        socket.flush();
        return readProxyResponse(&socket, head, body);
    };

    QByteArray head;
    QByteArray body;
    QVERIFY(fetch(localPlaylist, &head, &body));
    QVERIFY(body.contains("/stream/"));

    // 深拖动预取会创建 4 条多连接 Range 条目；源站只回 1KB 后挂住，
    // 这些条目会长期占用缓存，但字节必须计入 cacheStats()。
    proxy.prefetchForSeek(QUrl(playlistUrl), {}, 0);
    QTRY_VERIFY_WITH_TIMEOUT(proxy.cacheStats().rangePrefetches >= 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(proxy.cacheStats().bytes >= 1024, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(upstream.slowRequests.size() >= 1, 3000);

    // 再用 260 个普通媒体请求把缓存推过 256 条上限，淘汰最老的预取条目。
    // 若预取槽位没有归还，队列里的 slow4.ts 永远不会被发起。
    for (int i = 0; i < 260; ++i) {
        const QString mediaUrl =
            QStringLiteral("http://127.0.0.1:%1/media/%2.mp3")
                .arg(upstream.serverPort())
                .arg(i);
        const QUrl local = proxy.proxyUrlFor(QUrl(mediaUrl));
        QVERIFY(local.isValid());
        QVERIFY(fetch(local, &head, &body));
    }

    QTRY_VERIFY_WITH_TIMEOUT(upstream.slowRequests.contains(QStringLiteral("slow4.ts")),
                             5000);
    const StreamProxy::CacheStats stats = proxy.cacheStats();
    QVERIFY2(stats.entries <= 256, qPrintable(QString::number(stats.entries)));
    QVERIFY2(stats.activePrefetches <= 4, qPrintable(QString::number(stats.activePrefetches)));

    // 最后一片应仍在缓存：再次读取不能打上游。
    const QUrl newest = proxy.proxyUrlFor(
        QUrl(QStringLiteral("http://127.0.0.1:%1/media/259.mp3").arg(upstream.serverPort())));
    const int beforeNewest = upstream.mediaRequests;
    QVERIFY(fetch(newest, &head, &body));
    QCOMPARE(upstream.mediaRequests, beforeNewest);

    // 最早一片应已被 LRU 淘汰：再次读取必须重新请求上游。
    const QUrl oldest = proxy.proxyUrlFor(
        QUrl(QStringLiteral("http://127.0.0.1:%1/media/0.mp3").arg(upstream.serverPort())));
    const int beforeOldest = upstream.mediaRequests;
    QVERIFY(fetch(oldest, &head, &body));
    QCOMPARE(upstream.mediaRequests, beforeOldest + 1);
}

void CoreLogicTest::seekPrefetchWindowFollowsPlaylistIndex()
{
    // 索引 152 是播放列表里的目标片；FFmpeg 会顺序打开 152..161。
    // 这个用例锁住“从目标索引开始、向后 9 片”的窗口，防止再按文件名
    // 做 target+2 / target-9 导致整段预取错位。
    const auto normal = HlsSeekWindow::forTarget(152, 600, 9);
    QVERIFY(normal.isValid());
    QCOMPARE(normal.first, 152);
    QCOMPARE(normal.last, 161);

    const auto atStart = HlsSeekWindow::forTarget(0, 600, 9);
    QVERIFY(atStart.isValid());
    QCOMPARE(atStart.first, 0);
    QCOMPARE(atStart.last, 9);

    // 靠近末尾时只能裁到最后一个分片，不能越界。
    const auto atEnd = HlsSeekWindow::forTarget(598, 600, 9);
    QVERIFY(atEnd.isValid());
    QCOMPARE(atEnd.first, 598);
    QCOMPARE(atEnd.last, 599);

    QVERIFY(!HlsSeekWindow::forTarget(-1, 600, 9).isValid());
    QVERIFY(!HlsSeekWindow::forTarget(600, 600, 9).isValid());
    QVERIFY(!HlsSeekWindow::forTarget(152, 0, 9).isValid());
}

void CoreLogicTest::streamProxyMultiRangeForHlsSegment()
{
    RangeSupportingServer upstream;
    upstream.body = proxyTestBody(200'000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/hls/0000001.ts").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));

    const QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\n"
                               "Range: bytes=0-\r\n"
                               "Connection: close\r\n\r\n";
    socket.write(request);
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body, 8000));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QVERIFY(head.contains("Content-Range: bytes 0-199999/200000"));
    QCOMPARE(body, upstream.body);

    // 16KB 探测 + 剩余按 4 路拆分，必须是多连接 Range，而不是单路全量透传。
    QTRY_VERIFY_WITH_TIMEOUT(upstream.requestCount >= 4, 5000);
    QVERIFY(upstream.ranges.contains(qMakePair(qint64(0), qint64(16383))));
    QVERIFY(upstream.ranges.contains(qMakePair(qint64(16384), qint64(66383))));
}

void CoreLogicTest::hlsPreloadIndexPersistsAndFollowsRenamedTrack()
{
    HlsPreloadIndex::instance().openJournal(QString());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString journal = dir.path() + QStringLiteral("/index.preload.jsonl");
    const QString partial = dir.path() + QStringLiteral("/track.video.mvpdownload");
    const QString finalPath = dir.path() + QStringLiteral("/track.video");
    QByteArray bytes(120, 'x');
    QFile file(partial);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();

    HlsPreloadIndex::instance().openJournal(journal);
    HlsPreloadEntry entry;
    entry.url = QUrl(QStringLiteral("https://cdn.example.com/hls/seg0.ts?token=a&seq=1"));
    entry.range = QByteArrayLiteral("bytes=0-");
    entry.filePath = partial;
    entry.offset = 10;
    entry.size = 20;
    entry.contentType = QByteArrayLiteral("video/mp2t");
    HlsPreloadIndex::instance().record(QStringLiteral("item-a"), entry);
    QCOMPARE(HlsPreloadIndex::instance().count(QStringLiteral("item-a")), 1);

    HlsPreloadSlice slice;
    QVERIFY(HlsPreloadIndex::instance().find(QStringLiteral("item-a"), entry.url, QByteArray(), &slice));
    QCOMPARE(slice.filePath, partial);
    QCOMPARE(slice.offset, qint64(10));
    QCOMPARE(slice.size, qint64(20));
    QVERIFY(!HlsPreloadIndex::instance().find(QStringLiteral("item-b"), entry.url, QByteArray(), &slice));

    // 同一分片换了 token/sign 也必须命中；换掉 seq 这类稳定参数不能命中。
    const QUrl rotatedUrl(
        QStringLiteral("https://cdn.example.com/hls/seg0.ts?token=b&sign=2&seq=1"));
    const QUrl otherSegment(
        QStringLiteral("https://cdn.example.com/hls/seg0.ts?token=b&seq=2"));
    QVERIFY(HlsPreloadIndex::instance().find(QStringLiteral("item-a"), rotatedUrl,
                                             QByteArrayLiteral("bytes=0-"), &slice));
    QVERIFY(!HlsPreloadIndex::instance().find(QStringLiteral("item-a"), otherSegment,
                                              QByteArrayLiteral("bytes=0-"), &slice));
    // 从 0 开始的小范围探测也由整片本地数据切出，不因终点不是分片末尾而回源。
    QVERIFY(HlsPreloadIndex::instance().find(QStringLiteral("item-a"), entry.url,
                                             QByteArrayLiteral("bytes=0-9"), &slice));
    QVERIFY(!HlsPreloadIndex::instance().find(QStringLiteral("item-a"), entry.url,
                                              QByteArrayLiteral("bytes=5-9"), &slice));

    // 新进程重新打开日志必须还能命中；下载完成后临时文件改名也不能丢索引。
    HlsPreloadIndex::instance().openJournal(journal);
    QVERIFY(HlsPreloadIndex::instance().find(QStringLiteral("item-a"), entry.url,
                                             QByteArrayLiteral("bytes=0-"), &slice));
    QCOMPARE(slice.filePath, partial);
    QVERIFY(QFile::rename(partial, finalPath));
    QVERIFY(HlsPreloadIndex::instance().find(QStringLiteral("item-a"), entry.url,
                                             QByteArray(), &slice));
    QCOMPARE(slice.filePath, finalPath);

    // 另一条轨道仍要保留；续传点清零只丢弃视频轨道。
    const QString audioPartial = dir.path() + QStringLiteral("/track.audio.mvpdownload");
    QFile audioFile(audioPartial);
    QVERIFY(audioFile.open(QIODevice::WriteOnly));
    QVERIFY(audioFile.write(bytes) == qint64(bytes.size()));
    audioFile.close();
    HlsPreloadEntry audioEntry;
    audioEntry.url = QUrl(QStringLiteral("https://cdn.example.com/hls/audio0.ts"));
    audioEntry.range = QByteArrayLiteral("bytes=0-");
    audioEntry.filePath = audioPartial;
    audioEntry.offset = 0;
    audioEntry.size = 10;
    HlsPreloadIndex::instance().record(QStringLiteral("item-a"), audioEntry);
    QCOMPARE(HlsPreloadIndex::instance().count(QStringLiteral("item-a")), 2);
    QCOMPARE(HlsPreloadIndex::instance().countTrack(QStringLiteral("item-a"), finalPath), 1);
    QCOMPARE(HlsPreloadIndex::instance().countTrack(QStringLiteral("item-a"), audioPartial), 1);

    HlsPreloadIndex::instance().clearTrack(finalPath);
    QCOMPARE(HlsPreloadIndex::instance().count(QStringLiteral("item-a")), 1);
    QVERIFY(!HlsPreloadIndex::instance().find(QStringLiteral("item-a"), entry.url, QByteArray(),
                                              &slice));
    QVERIFY(HlsPreloadIndex::instance().find(QStringLiteral("item-a"), audioEntry.url, QByteArray(),
                                             &slice));
    HlsPreloadIndex::instance().openJournal(journal);
    QCOMPARE(HlsPreloadIndex::instance().count(QStringLiteral("item-a")), 1);
    HlsPreloadIndex::instance().openJournal(QString());
}

void CoreLogicTest::hlsPreloadMigratorBuildsOnlyExactIndex()
{
    RangeSupportingServer upstream;
    upstream.body = proxyTestBody(1000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString trackPath = dir.path() + QStringLiteral("/legacy.video.mvpdownload");
    QFile file(trackPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArray(3000, 'z')) == 3000);
    file.close();

    HttpClient http;
    const QString base = QStringLiteral("http://127.0.0.1:%1/hls/seg%2.ts")
                             .arg(upstream.serverPort());
    QList<QUrl> urls;
    for (int i = 0; i < 3; ++i)
        urls.append(QUrl(base.arg(QString::number(i))));

    bool done = false;
    bool ok = false;
    QString error;
    QList<HlsPreloadEntry> entries;
    HlsPreloadMigrator migrator(
        &http, urls, 3, 3000, trackPath, QByteArrayLiteral("video/mp2t"),
        QHash<QString, QString>(), QUrl(),
        [&](bool success, const QList<HlsPreloadEntry> &result, const QString &message) {
            done = true;
            ok = success;
            entries = result;
            error = message;
        });
    migrator.start();
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(1).offset, qint64(1000));
    QCOMPARE(entries.at(2).size, qint64(1000));

    // 如果探测累计与续传记录不一致，必须整体失败，不能生成错位索引。
    bool mismatchDone = false;
    bool mismatchOk = true;
    HlsPreloadMigrator mismatch(
        &http, urls, 3, 3001, trackPath, QByteArrayLiteral("video/mp2t"),
        QHash<QString, QString>(), QUrl(),
        [&](bool success, const QList<HlsPreloadEntry> &, const QString &) {
            mismatchDone = true;
            mismatchOk = success;
        });
    mismatch.start();
    QTRY_VERIFY_WITH_TIMEOUT(mismatchDone, 5000);
    QVERIFY(!mismatchOk);
}

void CoreLogicTest::streamProxyServesPreloadedSegmentWithoutUpstream()
{
    PreloadPlaylistServer upstream;
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString trackPath = dir.path() + QStringLiteral("/track.video");
    QFile file(trackPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(upstream.body), qint64(upstream.body.size()));
    file.close();

    const QString identity = QStringLiteral("preload-item");
    const QUrl segmentUrl(QStringLiteral("http://127.0.0.1:%1/hls/seg.ts")
                              .arg(upstream.serverPort()));
    HlsPreloadEntry entry;
    entry.url = segmentUrl;
    entry.range = QByteArrayLiteral("bytes=0-");
    entry.filePath = trackPath;
    entry.offset = 0;
    entry.size = upstream.body.size();
    entry.contentType = QByteArrayLiteral("video/mp2t");
    HlsPreloadIndex::instance().openJournal(dir.path() + QStringLiteral("/index.preload.jsonl"));
    HlsPreloadIndex::instance().record(identity, entry);

    StreamProxy proxy;
    QVERIFY(proxy.start());
    const QUrl playlistUrl(QStringLiteral("http://127.0.0.1:%1/hls/index.m3u8")
                               .arg(upstream.serverPort()));
    const QUrl localPlaylist = proxy.proxyUrlFor(playlistUrl, {}, QString(), identity);
    QVERIFY(localPlaylist.isValid());

    auto fetch = [](const QUrl &local, const QByteArray &range, QByteArray *head,
                    QByteArray *body) {
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

    QByteArray playlistHead;
    QByteArray playlistBody;
    QVERIFY(fetch(localPlaylist, {}, &playlistHead, &playlistBody));
    QVERIFY2(playlistHead.startsWith("HTTP/1.1 200"), playlistHead.constData());
    // 改写后的播放列表里，子分片必须指向同一个本地代理。
    const auto rewritten = HlsPlaylist::parse(playlistBody, localPlaylist);
    QVERIFY(!rewritten.isNull());
    QCOMPARE(rewritten->segments.size(), 1);
    QCOMPARE(rewritten->segments.first().host(), localPlaylist.host());

    QByteArray segmentHead;
    QByteArray segmentBody;
    // 播放器有时把整段写成 `bytes=0-<长度-1>`，索引也要能命中。
    const QByteArray boundedRange =
        QByteArrayLiteral("bytes=0-") + QByteArray::number(upstream.body.size() - 1);
    QVERIFY(fetch(rewritten->segments.first(), boundedRange, &segmentHead, &segmentBody));
    QVERIFY2(segmentHead.startsWith("HTTP/1.1 206"), segmentHead.constData());
    QCOMPARE(segmentBody, upstream.body);
    // 根播放列表只回源一次；分片必须由本地索引命中，不能产生第二次上游请求。
    QCOMPARE(upstream.requestCount, 1);
    HlsPreloadIndex::instance().openJournal(QString());
}

void CoreLogicTest::streamProxySkipsSingleFileHlsSegmentCache()
{
    SingleFileHlsServer upstream;
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QUrl playlistUrl(
        QStringLiteral("http://127.0.0.1:%1/hls/index.m3u8").arg(upstream.serverPort()));
    const QUrl mediaUrl(
        QStringLiteral("http://127.0.0.1:%1/hls/main.mp4").arg(upstream.serverPort()));
    const QUrl localPlaylist = proxy.proxyUrlFor(playlistUrl);
    const QUrl localMedia = proxy.proxyUrlFor(mediaUrl);
    QVERIFY(localPlaylist.isValid());
    QVERIFY(localMedia.isValid());

    auto fetch = [](const QUrl &local, const QByteArray &range, QByteArray *head,
                    QByteArray *body) {
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
    QVERIFY(fetch(localPlaylist, {}, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 200"), head.constData());
    QVERIFY(body.contains("/stream/"));

    // 播放器对 main.mp4 请求 `bytes=718-`：上游会告诉总长 400KB，
    // 但这是单文件 HLS 的第 2 个 BYTERANGE 起点，绝不能把整段建进缓存。
    QVERIFY(fetch(localMedia, "bytes=718-", &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 206"), head.constData());
    QVERIFY(body.size() > 300'000);

    const StreamProxy::CacheStats stats = proxy.cacheStats();
    QCOMPARE(stats.entries, 0);
    QCOMPARE(stats.bytes, 0);
}

void CoreLogicTest::streamProxyFallsBackWhenHlsRangeIgnored()
{
    RangeIgnoringServer upstream;
    upstream.body = proxyTestBody();
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QString upstreamUrl =
        QStringLiteral("http://127.0.0.1:%1/hls/0000002.ts").arg(upstream.serverPort());
    const QUrl local = proxy.proxyUrlFor(QUrl(upstreamUrl));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));

    const QByteArray request = "GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\n"
                               "Range: bytes=0-\r\n"
                               "Connection: close\r\n\r\n";
    socket.write(request);
    socket.flush();

    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body, 8000));
    QCOMPARE(body, upstream.body);
    // 首片探测收到 200 后必须回退单路，不能把客户端挂在 multiRange 上等超时。
    QTRY_COMPARE_WITH_TIMEOUT(upstream.requestCount, 2, 5000);
    QCOMPARE(upstream.lastRange, QStringLiteral("bytes=0-"));
}

void CoreLogicTest::streamProxyHandlesFailedRangePartWithoutCrash()
{
    RangePartFailServer upstream;
    QVERIFY(upstream.listen(QHostAddress::LocalHost));

    StreamProxy proxy;
    QVERIFY(proxy.start());

    const QUrl upstreamUrl(
        QStringLiteral("http://127.0.0.1:%1/hls/0000002.ts").arg(upstream.serverPort()));
    const QUrl local = proxy.proxyUrlFor(upstreamUrl);
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));
    socket.write("GET " + local.path().toUtf8() + " HTTP/1.1\r\n"
                 "Host: 127.0.0.1\r\n"
                 "Range: bytes=0-\r\n"
                 "Connection: close\r\n\r\n");
    socket.flush();

    // 首片探测成功后，代理会把播放器挂到 clients，再并发请求剩余 Range。
    // 后续分片返回 500 时 failRangeEntry 必须在同步断连重入下仍安全。
    QTRY_VERIFY_WITH_TIMEOUT(upstream.requestCount >= 3, 5000);
    QTest::qWait(1500);

    // 崩溃会在上面的异步回调里直接 abort；走到这里说明没有触发 QList 越界。
    QVERIFY(proxy.cacheStats().entries <= 1);
    proxy.shutdown();
}

void CoreLogicTest::hlsPlaylistParsesAndIndexes()
{
    const QByteArray body =
        "#EXTM3U\n"
        "#EXT-X-BASE:https://cdn.test/a/\n"
        "#EXTINF:4,\n"
        "seg1.ts\n"
        "#EXTINF:2.5,\n"
        "../b/seg2.ts?x=1\n";
    const QSharedPointer<HlsPlaylist::Info> info =
        HlsPlaylist::parse(body, QUrl(QStringLiteral("https://origin.test/path/list.m3u8")));
    QVERIFY(!info.isNull());
    QCOMPARE(info->segments.size(), 2);
    QCOMPARE(info->segments.at(0),
             QUrl(QStringLiteral("https://cdn.test/a/seg1.ts")));
    QCOMPARE(info->segments.at(1),
             QUrl(QStringLiteral("https://cdn.test/b/seg2.ts?x=1")));
    QCOMPARE(info->starts.at(0), qint64(0));
    QCOMPARE(info->starts.at(1), qint64(4000));
    QCOMPARE(info->totalMs, qint64(6500));
    QVERIFY(info->prefetchable);

    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, -1), 0);
    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, 3999), 0);
    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, 4000), 1);
    QCOMPARE(HlsPlaylist::segmentIndexAt(*info, 9999), 1);

    QVERIFY(HlsPlaylist::sameScope(
        QUrl(QStringLiteral("https://cdn.test/a/list.m3u8")),
        QUrl(QStringLiteral("https://cdn.test/a/720/index.m3u8"))));
    QVERIFY(!HlsPlaylist::sameScope(
        QUrl(QStringLiteral("https://cdn.test/a/list.m3u8")),
        QUrl(QStringLiteral("https://other.test/a/list.m3u8"))));

    const QByteArray byteRange =
        "#EXTM3U\n"
        "#EXTINF:4,\n"
        "#EXT-X-BYTERANGE:100@0\n"
        "seg.ts\n";
    const QSharedPointer<HlsPlaylist::Info> ranged =
        HlsPlaylist::parse(byteRange, QUrl(QStringLiteral("https://cdn.test/a/list.m3u8")));
    QVERIFY(!ranged.isNull());
    QVERIFY(!ranged->prefetchable);
}

void CoreLogicTest::hlsPlaylistRewritesUris()
{
    const QUrl playlistUrl(QStringLiteral("https://cdn.test/a/list.m3u8"));
    const QByteArray body =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXTINF:4,\n"
        "seg1.ts\n";

    const QByteArray rewritten = HlsPlaylist::rewrite(
        body, playlistUrl, [](const QUrl &absolute) {
            Q_UNUSED(absolute)
            return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
        });

    const QString text = QString::fromUtf8(rewritten);
    QVERIFY2(text.contains(QStringLiteral("URI=\"http://127.0.0.1:1234/stream/token/key.bin\"")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("http://127.0.0.1:1234/stream/token/seg1.ts")),
             qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("seg1.ts\nseg1.ts")));

    // UTF-8 BOM 不能把首行 #EXTM3U 变成“分片 URI”，否则整份播放列表
    // 会被判成非法并返回空结果。
    QString bomError;
    const QByteArray bomPlaylist =
        "\xEF\xBB\xBF#EXTM3U\n#EXTINF:4,\nseg1.ts\n";
    const QByteArray bomRewritten = HlsPlaylist::rewrite(
        bomPlaylist, playlistUrl, [](const QUrl &) {
            return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
        }, &bomError);
    QVERIFY2(bomError.isEmpty(), qPrintable(bomError));
    QVERIFY(!bomRewritten.isEmpty());
    const QString bomText = QString::fromUtf8(bomRewritten);
    QVERIFY2(bomText.startsWith(QStringLiteral("#EXTM3U")), qPrintable(bomText));
    QVERIFY2(bomText.contains(QStringLiteral("http://127.0.0.1:1234/stream/token/seg1.ts")),
             qPrintable(bomText));

    // 非 HTTP(S) 分片必须整份拒绝，不能把 file:// 原样交给播放器。
    QString error;
    const QByteArray localSegment =
        "#EXTM3U\n#EXTINF:4,\nfile:///C:/secret.ts\n";
    QVERIFY(HlsPlaylist::rewrite(localSegment, playlistUrl, [](const QUrl &) {
        return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
    }, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // 未加引号的 URI= 同样不能绕过检查。
    error.clear();
    const QByteArray unquotedLocalKey =
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=file:///C:/secret.key\n#EXTINF:4,\nseg1.ts\n";
    QVERIFY(HlsPlaylist::rewrite(unquotedLocalKey, playlistUrl, [](const QUrl &) {
        return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
    }, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // #EXT-X-BASE 指向本地目录时也必须拒绝，不能只更新内部 base。
    error.clear();
    const QByteArray localBase =
        "#EXTM3U\n#EXT-X-BASE:file:///C:/media/\n#EXTINF:4,\nseg1.ts\n";
    QVERIFY(HlsPlaylist::rewrite(localBase, playlistUrl, [](const QUrl &) {
        return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
    }, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // 合法的未加引号 HTTP URI 仍要改写，且保留不加引号的形态。
    error.clear();
    const QByteArray unquotedKey =
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=key.bin\n#EXTINF:4,\nseg1.ts\n";
    const QByteArray unquotedRewritten = HlsPlaylist::rewrite(
        unquotedKey, playlistUrl, [](const QUrl &) {
            return QUrl(QStringLiteral("http://127.0.0.1:1234/stream/token/"));
        }, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(QString::fromUtf8(unquotedRewritten)
                .contains(QStringLiteral("URI=http://127.0.0.1:1234/stream/token/key.bin")));
    QVERIFY(!QString::fromUtf8(unquotedRewritten)
                 .contains(QStringLiteral("URI=\"http://127.0.0.1:1234/stream/token/key.bin\"")));
}

void CoreLogicTest::hlsPlaylistParsesBitrateVariants()
{
    const QUrl masterUrl(QStringLiteral("https://cdn.test/videos/master.m3u8"));
    const QByteArray master =
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"main\",URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,"
        "CODECS=\"avc1.4d401e,mp4a.40.2\",AUDIO=\"aud\"\n"
        "low/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x720,"
        "CODECS=\"avc1.4d401f,mp4a.40.2\",AUDIO=\"aud\"\n"
        "mid/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,"
        "CODECS=\"avc1.640028,mp4a.40.2\",AUDIO=\"aud\"\n"
        "high/index.m3u8\n"
        "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=100000,URI=\"iframe.m3u8\"\n";

    const QList<HlsPlaylist::Variant> variants = HlsPlaylist::variants(master, masterUrl);
    QCOMPARE(variants.size(), 3);
    QCOMPARE(variants.at(0).index, 0);
    QCOMPARE(variants.at(0).bandwidth, 800000);
    QCOMPARE(variants.at(0).resolution, QStringLiteral("640x360"));
    QCOMPARE(variants.at(0).audioGroup, QStringLiteral("aud"));
    QCOMPARE(variants.at(0).uri,
             QUrl(QStringLiteral("https://cdn.test/videos/low/index.m3u8")));
    QCOMPARE(variants.at(2).index, 2);
    QCOMPARE(variants.at(2).bandwidth, 5000000);
    QCOMPARE(variants.at(2).resolution, QStringLiteral("1920x1080"));
    QCOMPARE(variants.at(2).uri,
             QUrl(QStringLiteral("https://cdn.test/videos/high/index.m3u8")));

    // 普通媒体列表没有码率变体，码率栏应拿到空列表而不是把分片当变体。
    const QByteArray media =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\nseg1.ts\n#EXT-X-ENDLIST\n";
    QVERIFY(HlsPlaylist::variants(media, masterUrl).isEmpty());
}

void CoreLogicTest::hlsPlaylistSameScopeRejectsSiblingDirectory()
{
    const QUrl master(QStringLiteral("https://cdn.test/videos/2024/master.m3u8"));
    const QUrl sameDir(QStringLiteral("https://cdn.test/videos/2024/index.m3u8"));
    const QUrl sibling(QStringLiteral("https://cdn.test/videos/2024/other/index.m3u8"));
    const QUrl directChild(QStringLiteral("https://cdn.test/videos/2024/720p/index.m3u8"));
    const QUrl chunklistChild(QStringLiteral("https://cdn.test/videos/2024/chunklist/index.m3u8"));
    const QUrl numberedChild(QStringLiteral("https://cdn.test/videos/2024/v1/index.m3u8"));
    const QUrl otherOrigin(QStringLiteral("https://cdn.test:8443/videos/2024/index.m3u8"));

    QVERIFY(HlsPlaylist::sameScope(master, sameDir));
    // /videos/2024/ 与 /videos/2024/other/ 不能再共用预取列表。
    QVERIFY(!HlsPlaylist::sameScope(master, sibling));
    QVERIFY(HlsPlaylist::sameScope(master, directChild));
    // 常见但不在原有限白名单里的真实变体目录也要能回退预取。
    QVERIFY(HlsPlaylist::sameScope(master, chunklistChild));
    QVERIFY(HlsPlaylist::sameScope(master, numberedChild));
    QVERIFY(!HlsPlaylist::sameScope(master, otherOrigin));
}

void CoreLogicTest::hlsPlaylistRejectsTooManySegments()
{
    const QUrl playlistUrl(QStringLiteral("https://cdn.test/a/list.m3u8"));
    QByteArray body = "#EXTM3U\n";
    for (int i = 0; i <= NetLimits::kPlaylistSegments; ++i) {
        body += "#EXTINF:1,\n";
        body += "seg" + QByteArray::number(i) + ".ts\n";
    }
    QVERIFY(HlsPlaylist::parse(body, playlistUrl).isNull());

    body = "#EXTM3U\n";
    for (int i = 0; i < NetLimits::kPlaylistSegments; ++i) {
        body += "#EXTINF:1,\n";
        body += "seg" + QByteArray::number(i) + ".ts\n";
    }
    const QSharedPointer<HlsPlaylist::Info> info = HlsPlaylist::parse(body, playlistUrl);
    QVERIFY(!info.isNull());
    QCOMPARE(info->segments.size(), NetLimits::kPlaylistSegments);
    QCOMPARE(info->totalMs, qint64(NetLimits::kPlaylistSegments) * 1000);
}

void CoreLogicTest::hlsPlaylistSharedMediaMetadata()
{
    const QUrl playlistUrl(QStringLiteral("https://cdn.test/a/list.m3u8"));
    const QByteArray body =
        "#EXTM3U\n"
        "#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"720@0\"\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXT-X-BYTERANGE:1000@720\n"
        "#EXTINF:4,\n"
        "seg1.m4s\n"
        "#EXT-X-BYTERANGE:1000\n"
        "#EXTINF:4,\n"
        "seg2.m4s\n"
        "#EXT-X-ENDLIST\n";

    HlsPlaylist::MediaParseOptions options;
    options.strict = true;
    options.keyParser = [](const QString &line, const QUrl &base) {
        HlsPlaylist::Key key;
        if (!line.contains(QLatin1String("AES-128")))
            return key;
        key.enabled = true;
        key.method = QStringLiteral("AES-128");
        key.url = base.resolved(QUrl(QStringLiteral("key.bin")));
        return key;
    };

    QString error;
    const auto media = HlsPlaylist::parseMedia(body, playlistUrl, options, &error);
    QVERIFY2(!media.isNull(), qPrintable(error));
    QCOMPARE(media->segments.size(), 3);
    QVERIFY(media->hasEndList);
    QVERIFY(media->fragmentedMp4);
    QCOMPARE(media->totalMs, qint64(8000));
    QCOMPARE(media->mediaSequence, qint64(9));

    const auto &map = media->segments.at(0);
    QVERIFY(map.initSegment);
    QCOMPARE(map.range, QByteArray("bytes=0-719"));
    QVERIFY(!map.key.enabled);

    const auto &first = media->segments.at(1);
    QVERIFY(!first.initSegment);
    QCOMPARE(first.url, QUrl(QStringLiteral("https://cdn.test/a/seg1.m4s")));
    QCOMPARE(first.range, QByteArray("bytes=720-1719"));
    QVERIFY(first.key.enabled);
    QCOMPARE(first.key.url, QUrl(QStringLiteral("https://cdn.test/a/key.bin")));
    // 默认 IV 必须跟随 MEDIA-SEQUENCE：首段为 7。
    QCOMPARE(first.key.iv, QByteArray(15, '\0') + QByteArray::fromHex("07"));

    const auto &second = media->segments.at(2);
    // 省略 @offset 的 BYTERANGE 从上段末尾继续。
    QCOMPARE(second.range, QByteArray("bytes=1720-2719"));
    QCOMPARE(second.key.iv, QByteArray(15, '\0') + QByteArray::fromHex("08"));
}


void CoreLogicTest::streamTokensReuseByUrlAndHeaders()
{
    StreamTokenRegistry tokens;
    const QUrl upstream(QStringLiteral("https://cdn.test/a/seg1.ts"));
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), QStringLiteral("https://source.test/"));
    headers.insert(QStringLiteral("X-Source-Session"), QStringLiteral("sid=1"));

    const QUrl first = tokens.registerFor(upstream, headers, 4567);
    const QUrl second = tokens.registerFor(upstream, headers, 4567);
    QVERIFY(first.isValid());
    QCOMPARE(first, second);
    QCOMPARE(tokens.size(), 1);

    QHash<QString, QString> other = headers;
    other.insert(QStringLiteral("X-Source-Session"), QStringLiteral("sid=2"));
    const QUrl changed = tokens.registerFor(upstream, other, 4567);
    QVERIFY(changed != first);
    QCOMPARE(tokens.size(), 2);

    const QString path = first.path();
    const QString prefix = QStringLiteral("/stream/");
    QVERIFY(path.startsWith(prefix));
    const QString token = path.mid(prefix.size()).section(QLatin1Char('/'), 0, 0);
    QVERIFY(!token.isEmpty());

    StreamTokenRegistry::Upstream restored;
    QVERIFY(tokens.lookup(token, &restored));
    QCOMPARE(restored.url, upstream);
    // Referer 指向 source.test，分片在 cdn.test：跨站会话头必须被剥离。
    QCOMPARE(restored.headers.value(QStringLiteral("X-Source-Session")), QString());
    QVERIFY(!tokens.lookup(QStringLiteral("unknown"), &restored));

    // 同 host 时保留源站会话头，否则需要 Cookie 鉴权的 HLS 分片会 401/403。
    QHash<QString, QString> sameHost = headers;
    sameHost.insert(QStringLiteral("Referer"), QStringLiteral("https://cdn.test/"));
    const QUrl sameHostUrl = tokens.registerFor(upstream, sameHost, 4567);
    const QString sameHostToken =
        sameHostUrl.path().mid(prefix.size()).section(QLatin1Char('/'), 0, 0);
    QVERIFY(tokens.lookup(sameHostToken, &restored));
    QCOMPARE(restored.headers.value(QStringLiteral("X-Source-Session")),
             QStringLiteral("sid=1"));

    tokens.clear();
    QCOMPARE(tokens.size(), 0);
}

void CoreLogicTest::streamProxyRejectsPrivateTargetFromPublicOrigin()
{
    StreamProxy proxy;
    QVERIFY(proxy.start());

    // 模拟公网播放列表返回绝对的本机分片地址。令牌登记时保留可信源站，
    // 代理真正回源前必须拒绝，不能等播放器先发起请求。
    const QUrl local = proxy.proxyUrlFor(
        QUrl(QStringLiteral("http://127.0.0.1:9/seg.ts")), {},
        QString(), QString(), QUrl(QStringLiteral("https://source.example/")));
    QVERIFY(local.isValid());

    QTcpSocket socket;
    socket.connectToHost(local.host(), quint16(local.port()));
    QVERIFY(socket.waitForConnected(3000));
    const QByteArray request = "GET " + local.path().toUtf8() + "seg.ts HTTP/1.1\r\n"
                               "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";
    socket.write(request);
    socket.flush();
    QByteArray head;
    QByteArray body;
    QVERIFY(readProxyResponse(&socket, &head, &body));
    QVERIFY2(head.startsWith("HTTP/1.1 403"), head.constData());
}

void CoreLogicTest::streamTokensExpireByIdleClock()
{
    StreamTokenRegistry tokens;
    const QUrl upstream(QStringLiteral("https://cdn.test/b/seg2.ts"));
    tokens.registerFor(upstream, {}, 4567, {}, {},
                       QUrl(QStringLiteral("https://source.test/")));
    QCOMPARE(tokens.size(), 1);
    QCOMPARE(tokens.mappingSize(), 1);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    tokens.pruneAt(now + 3600 * 1000);
    QCOMPARE(tokens.size(), 1);
    QCOMPARE(tokens.mappingSize(), 1);

    tokens.pruneAt(now + 2 * 3600 * 1000 + 1000);
    QCOMPARE(tokens.size(), 0);
    // 反向映射键带 sourceOrigin，过期清理必须一并删除，不能只删 token 表。
    QCOMPARE(tokens.mappingSize(), 0);
}

void CoreLogicTest::streamTokensKeepFreshTokensBeyondCapacity()
{
    StreamTokenRegistry tokens;
    QUrl first;
    QUrl last;
    // 2 秒一片的长 VOD 正片会有几千个分片；这里故意超过容量上限，
    // 模拟“一次重写登记完整张播放列表”，开头分片的 token 不能被淘汰。
    for (int i = 0; i < 20000; ++i) {
        const QUrl upstream =
            QUrl(QStringLiteral("https://cdn.test/long/seg%1.ts").arg(i));
        const QUrl proxied = tokens.registerFor(upstream, {}, 4567);
        if (i == 0)
            first = proxied;
        last = proxied;
    }
    QVERIFY(first.isValid());
    QVERIFY(last.isValid());

    auto tokenOf = [](const QUrl &url) {
        return url.path().mid(QStringLiteral("/stream/").size())
            .section(QLatin1Char('/'), 0, 0);
    };
    StreamTokenRegistry::Upstream restored;
    QVERIFY(tokens.lookup(tokenOf(first), &restored));
    QCOMPARE(restored.url, QUrl(QStringLiteral("https://cdn.test/long/seg0.ts")));
    QVERIFY(tokens.lookup(tokenOf(last), &restored));
    QCOMPARE(tokens.size(), 20000);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    tokens.pruneAt(now + 3 * 3600 * 1000);
    QCOMPARE(tokens.size(), 0);
}

void CoreLogicTest::streamCacheStoreCountsRangeParts()
{
    StreamCacheStore store;
    auto entry = QSharedPointer<StreamCacheEntry>::create();
    entry->data = QByteArray(10, 'a');
    entry->rangeParts.insert(0, QByteArray(5, 'b'));
    entry->rangeParts.insert(5, QByteArray(3, 'c'));
    entry->rangePartEnds.insert(0, 4);
    entry->rangePartEnds.insert(5, 7);
    QCOMPARE(StreamCacheStore::bytes(*entry), qint64(18));

    // pending 占位/在途响应按 total 预留配额，统计值取“已缓存”和“已预留”的较大者。
    entry->reservedBytes = qint64(64);
    QCOMPARE(StreamCacheStore::bytes(*entry), qint64(64));
    entry->reservedBytes = qint64(9);
    QCOMPARE(StreamCacheStore::bytes(*entry), qint64(18));

    store.insert(QStringLiteral("k"), entry);
    QCOMPARE(store.size(), 1);
    QVERIFY(store.contains(QStringLiteral("k")));
    QCOMPARE(store.value(QStringLiteral("k")), entry);

    const auto taken = store.take(QStringLiteral("k"));
    QCOMPARE(taken, entry);
    QVERIFY(store.isEmpty());

    store.insert(QStringLiteral("k2"), entry);
    store.clear();
    QVERIFY(store.isEmpty());
}

void CoreLogicTest::streamRangeParsesHeadersAndSplitsParts()
{
    qint64 start = -1;
    qint64 end = -1;
    qint64 total = -1;
    QVERIFY(StreamRange::parseContentRange(QByteArrayLiteral("bytes 100-199/500"), &start, &end,
                                           &total));
    QCOMPARE(start, qint64(100));
    QCOMPARE(end, qint64(199));
    QCOMPARE(total, qint64(500));
    QVERIFY(!StreamRange::parseContentRange(QByteArrayLiteral("bytes 100-200/200"), &start,
                                            &end, &total));
    QVERIFY(!StreamRange::parseContentRange(QByteArrayLiteral("bytes */500"), &start, &end,
                                            &total));

    QVERIFY(StreamRange::parseRange(QStringLiteral("bytes=0-99"), 500, &start, &end));
    QCOMPARE(start, qint64(0));
    QCOMPARE(end, qint64(99));
    QVERIFY(StreamRange::parseRange(QStringLiteral("bytes=400-"), 500, &start, &end));
    QCOMPARE(end, qint64(499));
    QVERIFY(StreamRange::parseRange(QStringLiteral("bytes=-100"), 500, &start, &end));
    QCOMPARE(start, qint64(400));
    QVERIFY(StreamRange::parseRange(QStringLiteral("bytes=0-1000"), 500, &start, &end));
    QCOMPARE(end, qint64(499));
    QVERIFY(!StreamRange::parseRange(QStringLiteral("bytes=0-1,5-6"), 500, &start, &end));
    QVERIFY(!StreamRange::parseRange(QStringLiteral("bytes=-0"), 500, &start, &end));
    QVERIFY(!StreamRange::parseRange(QStringLiteral("bytes=5-4"), 500, &start, &end));
    QVERIFY(!StreamRange::parseRange(QStringLiteral("bytes=0-99"), 0, &start, &end));

    const QList<StreamRange::Part> parts = StreamRange::split(1000, 0, 4, 100);
    QCOMPARE(parts.size(), 4);
    QCOMPARE(parts.at(0).start, qint64(0));
    QCOMPARE(parts.at(0).end, qint64(249));
    QCOMPARE(parts.at(3).start, qint64(750));
    QCOMPARE(parts.at(3).end, qint64(999));

    const QList<StreamRange::Part> tail = StreamRange::split(100, 50, 2, 100);
    QCOMPARE(tail.size(), 1);
    QCOMPARE(tail.at(0).start, qint64(50));
    QCOMPARE(tail.at(0).end, qint64(99));
    QVERIFY(StreamRange::split(100, 100, 2, 100).isEmpty());
}

void CoreLogicTest::streamRangeAppendsAndMergesConsecutive()
{
    auto entry = QSharedPointer<StreamCacheEntry>::create();
    entry->total = 10;
    StreamRange::append(*entry, 0, 4, QByteArrayLiteral("aa"));
    StreamRange::append(*entry, 0, 4, QByteArrayLiteral("bbb"));
    QCOMPARE(entry->rangeParts.value(0), QByteArrayLiteral("aabbb"));

    QVERIFY(!StreamRange::mergeConsecutive(*entry));
    QCOMPARE(entry->data, QByteArrayLiteral("aabbb"));

    StreamRange::append(*entry, 5, 9, QByteArrayLiteral("abcdefg"));
    QCOMPARE(entry->rangeParts.value(5), QByteArrayLiteral("abcde"));

    QVERIFY(StreamRange::mergeConsecutive(*entry));
    QCOMPARE(entry->data, QByteArrayLiteral("aabbbabcde"));
    QVERIFY(entry->complete);
    QVERIFY(entry->rangeParts.isEmpty());
    QVERIFY(entry->rangePartEnds.isEmpty());
}

QTEST_GUILESS_MAIN(CoreLogicTest)

#include "test_core_logic.moc"
