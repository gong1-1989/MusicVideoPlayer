#ifndef STREAMPROXY_H
#define STREAMPROXY_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

class HttpClient;
class QTcpServer;
class QTcpSocket;
class QNetworkReply;

/// 本地回环 HTTP 代理。
///
/// QMediaPlayer 只能拿到一个 URL，无法附加 Referer/Cookie，因此由本代理
/// 带着统一请求头去拉上游流，再把状态码、Content-Type、Content-Range 和
/// 字节原样透传给播放器。只监听 127.0.0.1，URL 里带随机 token，
/// 局域网内其他机器访问不到。
class StreamProxy : public QObject
{
    Q_OBJECT

public:
    explicit StreamProxy(QObject *parent = nullptr);
    ~StreamProxy() override;

    bool start();
    bool isListening() const;
    int port() const;

    /// 登记一个上游地址，返回给 QMediaPlayer 使用的本地 URL。
    /// headers 是该源必需的请求头（Referer、Cookie 等），代理在拉流时补上。
    QUrl proxyUrlFor(const QUrl &upstream,
                     const QHash<QString, QString> &headers = QHash<QString, QString>());

signals:
    /// 上游 302 落到 /404 或返回的不是音视频，通常是版权/付费/登录限制。
    void sourceUnavailable(const QUrl &upstream, const QString &contentType);

private slots:
    void onNewConnection();

private:
    struct Request
    {
        QString method;
        QString path;
        QString range;
        bool isValid = false;
    };

    void handleClient(QTcpSocket *client, const QByteArray &payload);
    void attachUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream);
    void flushUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream);
    void replyError(QTcpSocket *client, int status, const QString &reason);

    QByteArray buildResponseHead(QNetworkReply *reply, const QUrl &upstream, int status) const;
    static Request parseRequest(const QByteArray &payload);
    static QByteArray guessContentType(const QUrl &upstream);

    struct Upstream
    {
        QUrl url;
        QHash<QString, QString> headers;
    };

    HttpClient *m_http;
    QTcpServer *m_server;
    QHash<QString, Upstream> m_tokens;
};

#endif // STREAMPROXY_H
