#include "stream/StreamProxy.h"

#include "net/HttpClient.h"

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>

Q_LOGGING_CATEGORY(lcProxy, "mvp.stream")

namespace {

const char *const kHeaderSent = "proxyHeadersSent";
const char *const kHandled = "proxyHandled";
const char *const kBuffer = "proxyRequestBuffer";
const char *const kMethod = "proxyMethod";

/// 网易云对无版权/付费歌曲不会返回错误码，而是 302 到 /404 再返回整页 HTML。
/// 把 HTML 当音频流喂给 FFmpeg 只会得到含糊的 “Could not open file”，
/// 所以这里先按最终 URL 和 Content-Type 拦一道。
bool looksLikeMedia(QNetworkReply *reply)
{
    if (reply->url().path().endsWith(QLatin1String("/404")))
        return false;

    const QString type =
        reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    if (type.isEmpty())
        return true; // 类型未知时不武断拒绝，交给播放器自己判断
    return type.startsWith(QLatin1String("audio/"))
        || type.startsWith(QLatin1String("video/"))
        || type.contains(QLatin1String("octet-stream"))
        || type.contains(QLatin1String("mpegurl"));
}

QByteArray statusReason(int status)
{
    switch (status) {
    case 200:
        return QByteArrayLiteral("OK");
    case 206:
        return QByteArrayLiteral("Partial Content");
    case 301:
        return QByteArrayLiteral("Moved Permanently");
    case 302:
        return QByteArrayLiteral("Found");
    case 400:
        return QByteArrayLiteral("Bad Request");
    case 403:
        return QByteArrayLiteral("Forbidden");
    case 404:
        return QByteArrayLiteral("Not Found");
    case 416:
        return QByteArrayLiteral("Range Not Satisfiable");
    case 500:
        return QByteArrayLiteral("Internal Server Error");
    case 502:
        return QByteArrayLiteral("Bad Gateway");
    default:
        return QByteArrayLiteral("Error");
    }
}

} // namespace

StreamProxy::StreamProxy(QObject *parent)
    : QObject(parent)
    , m_http(new HttpClient(this))
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &StreamProxy::onNewConnection);
}

StreamProxy::~StreamProxy()
{
    m_server->close();
}

bool StreamProxy::start()
{
    if (m_server->isListening())
        return true;

    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(lcProxy) << "listen failed:" << m_server->errorString();
        return false;
    }
    qCInfo(lcProxy) << "listening on 127.0.0.1:" << m_server->serverPort();
    return true;
}

bool StreamProxy::isListening() const
{
    return m_server->isListening();
}

int StreamProxy::port() const
{
    return m_server->serverPort();
}

QUrl StreamProxy::proxyUrlFor(const QUrl &upstream)
{
    if (!isListening() && !start())
        return QUrl();

    const QString token = QString::number(QRandomGenerator::global()->generate64(), 16);
    m_tokens.insert(token, upstream);
    return QUrl(QStringLiteral("http://127.0.0.1:%1/stream/%2")
                    .arg(m_server->serverPort())
                    .arg(token));
}

void StreamProxy::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *client = m_server->nextPendingConnection();
        client->setProperty(kHeaderSent, false);
        client->setProperty(kHandled, false);
        client->setProperty(kBuffer, QByteArray());
        client->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        connect(client, &QAbstractSocket::disconnected, client, &QObject::deleteLater);
        connect(client, &QAbstractSocket::readyRead, this, [this, client] {
            if (client->property(kHandled).toBool()) {
                client->readAll(); // 请求体不关心，丢弃避免缓冲增长
                return;
            }

            QByteArray buffer = client->property(kBuffer).toByteArray();
            buffer += client->readAll();
            if (!buffer.contains("\r\n\r\n")) {
                if (buffer.size() > 16 * 1024)
                    replyError(client, 400, QStringLiteral("Request header too large"));
                else
                    client->setProperty(kBuffer, buffer);
                return;
            }

            client->setProperty(kHandled, true);
            handleClient(client, buffer);
        });
    }
}

void StreamProxy::handleClient(QTcpSocket *client, const QByteArray &payload)
{
    const Request request = parseRequest(payload);
    if (!request.isValid) {
        replyError(client, 400, QStringLiteral("Bad Request"));
        return;
    }

    const QString token = request.path.section(QLatin1Char('/'), -1);
    const QUrl upstream = m_tokens.value(token);
    if (!upstream.isValid()) {
        qCWarning(lcProxy) << "unknown token" << token;
        replyError(client, 404, QStringLiteral("Not Found"));
        return;
    }

    client->setProperty(kMethod, request.method);
    const QByteArray range = request.range.toUtf8();
    qCDebug(lcProxy) << request.method << "upstream" << upstream.toString() << "range" << request.range;

    // 由 HttpClient 手动跟随 302（含 https→http 降级），拿到可读的上游 reply
    QPointer<QTcpSocket> guard(client);
    m_http->fetch(upstream, range, [this, guard, upstream](QNetworkReply *reply) {
        if (guard.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        QTcpSocket *client = guard.data();
        if (reply == nullptr) {
            replyError(client, 502, QStringLiteral("Upstream redirect rejected"));
            return;
        }
        if (client->state() != QAbstractSocket::ConnectedState) {
            reply->abort();
            reply->deleteLater();
            return;
        }
        if (!looksLikeMedia(reply)) {
            const QString type =
                reply->header(QNetworkRequest::ContentTypeHeader).toString();
            qCWarning(lcProxy) << "upstream is not media:" << reply->url() << type;
            emit sourceUnavailable(upstream, type);
            replyError(client, 404, QStringLiteral("no audio source"));
            reply->abort();
            reply->deleteLater();
            return;
        }
        attachUpstream(client, reply, upstream);
        flushUpstream(client, reply, upstream);
        if (reply->isFinished()) {
            client->disconnectFromHost();
            reply->deleteLater();
        }
    });
}

void StreamProxy::attachUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream)
{
    // 播放器断开（切歌、停止）时立刻掐掉上游，避免继续吃带宽
    connect(client, &QAbstractSocket::disconnected, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::readyRead, client, [this, client, reply, upstream] {
        flushUpstream(client, reply, upstream);
    });

    connect(reply, &QNetworkReply::finished, client, [this, client, reply, upstream] {
        flushUpstream(client, reply, upstream);
        if (client->state() == QAbstractSocket::ConnectedState)
            client->disconnectFromHost();
        reply->deleteLater();
    });
}

void StreamProxy::flushUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream)
{
    if (client->state() != QAbstractSocket::ConnectedState)
        return;

    if (!client->property(kHeaderSent).toBool()) {
        client->setProperty(kHeaderSent, true);
        if (reply->error() != QNetworkReply::NoError)
            qCWarning(lcProxy) << "upstream error:" << reply->errorString();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        client->write(buildResponseHead(reply, upstream, status));

        if (client->property(kMethod).toString() == QLatin1String("HEAD")) {
            reply->abort();
            client->disconnectFromHost();
            return;
        }
    }

    const QByteArray chunk = reply->readAll();
    if (!chunk.isEmpty())
        client->write(chunk);
}

QByteArray StreamProxy::buildResponseHead(QNetworkReply *reply, const QUrl &upstream, int status) const
{
    if (status <= 0)
        status = 502;

    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    const bool hasLength = length.isValid() && length.toLongLong() >= 0;

    // 长度未知时用 HTTP/1.0 + Connection: close，让客户端以连接关闭作为正文结束，
    // 避免 HTTP/1.1 既无 Content-Length 又无 chunked 的歧义。
    QByteArray head = hasLength ? QByteArrayLiteral("HTTP/1.1 ") : QByteArrayLiteral("HTTP/1.0 ");
    head += QByteArray::number(status) + ' ' + statusReason(status) + "\r\n";

    QByteArray contentType;
    const QVariant typeHeader = reply->header(QNetworkRequest::ContentTypeHeader);
    if (typeHeader.isValid())
        contentType = typeHeader.toString().toUtf8();
    if (contentType.isEmpty())
        contentType = guessContentType(upstream);
    head += "Content-Type: " + contentType + "\r\n";

    if (hasLength)
        head += "Content-Length: " + QByteArray::number(length.toLongLong()) + "\r\n";

    const QByteArray contentRange = reply->rawHeader("Content-Range");
    if (!contentRange.isEmpty())
        head += "Content-Range: " + contentRange + "\r\n";

    head += "Accept-Ranges: bytes\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Connection: close\r\n\r\n";
    return head;
}

void StreamProxy::replyError(QTcpSocket *client, int status, const QString &reason)
{
    const QByteArray body = reason.toUtf8();
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusReason(status) + "\r\n";
    head += "Content-Type: text/plain; charset=utf-8\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    client->write(head + body);
    client->disconnectFromHost();
}

StreamProxy::Request StreamProxy::parseRequest(const QByteArray &payload)
{
    Request request;

    const QList<QByteArray> lines = payload.split('\n');
    if (lines.isEmpty())
        return request;

    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2)
        return request;

    request.method = QString::fromLatin1(parts.at(0));
    request.path = QString::fromLatin1(parts.at(1));

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            break;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        if (key == QLatin1String("range"))
            request.range = QString::fromLatin1(line.mid(colon + 1)).trimmed();
    }

    request.isValid = request.method == QLatin1String("GET")
                   || request.method == QLatin1String("HEAD");
    return request;
}

QByteArray StreamProxy::guessContentType(const QUrl &upstream)
{
    static const QHash<QString, QByteArray> types{
        {QStringLiteral("mp3"), QByteArrayLiteral("audio/mpeg")},
        {QStringLiteral("wav"), QByteArrayLiteral("audio/wav")},
        {QStringLiteral("m4a"), QByteArrayLiteral("audio/mp4")},
        {QStringLiteral("aac"), QByteArrayLiteral("audio/aac")},
        {QStringLiteral("flac"), QByteArrayLiteral("audio/flac")},
        {QStringLiteral("ogg"), QByteArrayLiteral("audio/ogg")},
        {QStringLiteral("opus"), QByteArrayLiteral("audio/ogg")},
        {QStringLiteral("mp4"), QByteArrayLiteral("video/mp4")},
        {QStringLiteral("m4v"), QByteArrayLiteral("video/mp4")},
        {QStringLiteral("mkv"), QByteArrayLiteral("video/x-matroska")},
        {QStringLiteral("webm"), QByteArrayLiteral("video/webm")},
        {QStringLiteral("avi"), QByteArrayLiteral("video/x-msvideo")},
        {QStringLiteral("mov"), QByteArrayLiteral("video/quicktime")},
    };

    const QString suffix = upstream.path().section(QLatin1Char('.'), -1).toLower();
    return types.value(suffix, QByteArrayLiteral("application/octet-stream"));
}
