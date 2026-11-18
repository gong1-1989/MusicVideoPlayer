#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

/// 统一网络入口：集中管理 UA、Referer、Cookie、超时与重定向策略，
/// 业务模块不再各自拼请求头。
class HttpClient : public QObject
{
    Q_OBJECT

public:
    explicit HttpClient(QObject *parent = nullptr);

    QNetworkReply *get(const QUrl &url, const QByteArray &range = QByteArray());
    QNetworkReply *postForm(const QUrl &url, const QByteArray &body);

    void setCookie(const QString &cookie);

    static bool isHttpSuccess(QNetworkReply *reply);
    static QString describeError(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_nam;
    QString m_cookie;
};

#endif // HTTPCLIENT_H
