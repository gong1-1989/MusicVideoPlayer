#include "stream/StreamProxy.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {

const QByteArray kPayload = QByteArrayLiteral("0123456789ABCDEF");

/// 极简上游：固定 16 字节正文，支持 bytes=N-M 形式的 Range。
class UpstreamServer : public QTcpServer
{
public:
    using QTcpServer::QTcpServer;

protected:
    void incomingConnection(qintptr handle) override
    {
        auto *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(handle)) {
            delete socket;
            return;
        }
        connect(socket, &QAbstractSocket::readyRead, socket, [socket] {
            const QByteArray request = socket->readAll();
            if (!request.contains("\r\n\r\n"))
                return;
            respond(socket, request);
        });
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
    }

private:
    static void respond(QTcpSocket *socket, const QByteArray &request)
    {
        qint64 start = 0;
        qint64 end = kPayload.size() - 1;
        bool partial = false;

        for (const QByteArray &line : request.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (!trimmed.toLower().startsWith("range:"))
                continue;
            const QString spec = QString::fromLatin1(trimmed.mid(6)).trimmed();
            if (!spec.startsWith(QStringLiteral("bytes=")))
                continue;
            const QStringList parts = spec.mid(6).split(QLatin1Char('-'));
            if (parts.size() != 2)
                continue;
            bool ok = true;
            if (!parts.at(0).isEmpty())
                start = parts.at(0).toLongLong(&ok);
            if (ok && !parts.at(1).isEmpty())
                end = parts.at(1).toLongLong(&ok);
            partial = ok;
        }

        start = qBound<qint64>(0, start, kPayload.size() - 1);
        end = qBound<qint64>(start, end, kPayload.size() - 1);
        const QByteArray body = kPayload.mid(int(start), int(end - start + 1));

        QByteArray head = partial ? QByteArrayLiteral("HTTP/1.1 206 Partial Content\r\n")
                                  : QByteArrayLiteral("HTTP/1.1 200 OK\r\n");
        head += "Content-Type: audio/mpeg\r\n";
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (partial) {
            head += "Content-Range: bytes " + QByteArray::number(start) + '-'
                  + QByteArray::number(end) + '/' + QByteArray::number(kPayload.size()) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";

        socket->write(head + body);
        socket->disconnectFromHost();
    }
};

QNetworkReply *waitFor(QNetworkReply *reply)
{
    QSignalSpy spy(reply, &QNetworkReply::finished);
    if (!spy.wait(5000)) {
        reply->abort();
        return reply;
    }
    return reply;
}

} // namespace

class TestStreamProxy : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void forwardsRange();
    void keepsContentTypeAndLength();
    void acceptsHead();
    void rejectsUnknownToken();

private:
    QUrl upstreamUrl() const;

    UpstreamServer m_upstream;
    StreamProxy *m_proxy = nullptr;
};

QUrl TestStreamProxy::upstreamUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1/audio.mp3").arg(m_upstream.serverPort()));
}

void TestStreamProxy::initTestCase()
{
    QVERIFY(m_upstream.listen(QHostAddress::LocalHost, 0));
    m_proxy = new StreamProxy(this);
    QVERIFY(m_proxy->start());
    QVERIFY(m_proxy->isListening());
}

void TestStreamProxy::cleanupTestCase()
{
    delete m_proxy;
    m_proxy = nullptr;
}

void TestStreamProxy::forwardsRange()
{
    const QUrl proxied = m_proxy->proxyUrlFor(upstreamUrl());
    QVERIFY(proxied.isValid());

    QNetworkAccessManager nam;
    QNetworkRequest request(proxied);
    request.setRawHeader("Range", "bytes=8-");
    QNetworkReply *reply = waitFor(nam.get(request));

    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 206);
    QCOMPARE(reply->rawHeader("Content-Range"), QByteArrayLiteral("bytes 8-15/16"));
    QCOMPARE(reply->readAll(), QByteArrayLiteral("89ABCDEF"));
    reply->deleteLater();
}

void TestStreamProxy::keepsContentTypeAndLength()
{
    const QUrl proxied = m_proxy->proxyUrlFor(upstreamUrl());
    QNetworkAccessManager nam;
    QNetworkReply *reply = waitFor(nam.get(QNetworkRequest(proxied)));

    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(reply->header(QNetworkRequest::ContentTypeHeader).toString(),
             QStringLiteral("audio/mpeg"));
    QCOMPARE(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(), 16);
    QCOMPARE(reply->readAll(), kPayload);
    reply->deleteLater();
}

void TestStreamProxy::acceptsHead()
{
    const QUrl proxied = m_proxy->proxyUrlFor(upstreamUrl());
    QNetworkAccessManager nam;
    QNetworkReply *reply = waitFor(nam.head(QNetworkRequest(proxied)));

    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(), 16);
    QCOMPARE(reply->readAll(), QByteArray());
    reply->deleteLater();
}

void TestStreamProxy::rejectsUnknownToken()
{
    const QUrl unknown(QStringLiteral("http://127.0.0.1:%1/stream/deadbeef")
                           .arg(m_proxy->port()));
    QNetworkAccessManager nam;
    QNetworkReply *reply = waitFor(nam.get(QNetworkRequest(unknown)));

    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
    reply->deleteLater();
}

QTEST_MAIN(TestStreamProxy)
#include "tst_streamproxy.moc"
