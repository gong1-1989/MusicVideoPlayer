#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

/// 统一网络入口：集中管理 UA、Referer、Cookie、超时与重定向策略，
/// 业务模块不再各自拼请求头。
class HttpClient : public QObject
{
    Q_OBJECT

public:
    using ReplyHandler = std::function<void(QNetworkReply *)>;

    explicit HttpClient(QObject *parent = nullptr);

    /// 额外请求头（各源自定义的头、Cookie 等）会覆盖默认头
    QNetworkReply *get(const QUrl &url, const QByteArray &range = QByteArray(),
                       const QHash<QString, QString> &headers = {});
    QNetworkReply *postForm(const QUrl &url, const QByteArray &body,
                            const QHash<QString, QString> &headers = {});
    QNetworkReply *postJson(const QUrl &url, const QByteArray &body,
                            const QHash<QString, QString> &headers = {});

    /// 手动跟随重定向（含 https→http 降级），把最终可读的 reply 交给 onReady。
    /// onReady 只调用一次；重定向超限或目标协议不被允许时回调 nullptr。
    void fetch(const QUrl &url, const QByteArray &range, const ReplyHandler &onReady);
    void fetch(const QUrl &url, const QByteArray &range,
               const QHash<QString, QString> &headers, const ReplyHandler &onReady);

    /// 与 fetch 相同，但在最终响应头到达时立即回调，正文仍可继续 readyRead。
    /// 用于流式代理/下载；调用方负责连接 readyRead、finished 并 deleteLater。
    void stream(const QUrl &url, const QByteArray &range,
                const QHash<QString, QString> &headers, const ReplyHandler &onReady);

    void setCookie(const QString &cookie);

    static bool isHttpSuccess(QNetworkReply *reply);
    static QString describeError(QNetworkReply *reply);

private:
    void fetchInternal(const QUrl &url, const QByteArray &range,
                       const QHash<QString, QString> &headers,
                       const ReplyHandler &onReady, int hop, bool deliverOnHeaders);

    QNetworkAccessManager *m_nam;
    QString m_cookie;
};

#endif // HTTPCLIENT_H
