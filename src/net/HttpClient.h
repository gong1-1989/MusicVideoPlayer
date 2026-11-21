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

    static bool isHttpSuccess(QNetworkReply *reply);
    static QString describeError(QNetworkReply *reply);
    /// 读取整个响应体；超过 limit 时不读取并置 exceeded，避免把超大响应
    /// 整段复制进内存。调用方需自行把 exceeded 转成用户可见错误。
    static QByteArray readAllLimited(QNetworkReply *reply, qint64 limit,
                                     bool *exceeded = nullptr);
    /// 日志里隐藏签名/令牌类 query，只保留参数名
    static QString redactUrl(const QUrl &url);

private:
    void fetchInternal(const QUrl &url, const QByteArray &range,
                       const QHash<QString, QString> &headers,
                       const ReplyHandler &onReady, int hop, bool deliverOnHeaders);

    QNetworkAccessManager *m_nam;
};

#endif // HTTPCLIENT_H
