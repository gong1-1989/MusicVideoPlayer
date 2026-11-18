#include "net/HttpClient.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

Q_LOGGING_CATEGORY(lcHttp, "mvp.net")

namespace {

QNetworkRequest buildRequest(const QUrl &url, const QString &cookie)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // 头名必须是 "User-Agent"，写成 User_Agent 会被服务端忽略
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                                     "Chrome/120.0.0.0 Safari/537.36"));
    request.setRawHeader("Accept", "*/*");

    if (url.host().contains(QLatin1String("music.163.com"))) {
        request.setRawHeader("Referer", "https://music.163.com/");
        request.setRawHeader("Origin", "https://music.163.com");
    }
    if (!cookie.isEmpty())
        request.setRawHeader("Cookie", cookie.toUtf8());
    return request;
}

} // namespace

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_nam->setTransferTimeout(15000);
}

QNetworkReply *HttpClient::get(const QUrl &url, const QByteArray &range)
{
    qCDebug(lcHttp) << "GET" << url.toString() << "range" << range;
    QNetworkRequest request = buildRequest(url, m_cookie);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);
    return m_nam->get(request);
}

QNetworkReply *HttpClient::postForm(const QUrl &url, const QByteArray &body)
{
    qCDebug(lcHttp) << "POST" << url.toString() << "body" << body.size() << "bytes";
    QNetworkRequest request = buildRequest(url, m_cookie);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    return m_nam->post(request, body);
}

void HttpClient::setCookie(const QString &cookie)
{
    m_cookie = cookie;
}

bool HttpClient::isHttpSuccess(QNetworkReply *reply)
{
    if (!reply || reply->error() != QNetworkReply::NoError)
        return false;
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!status.isValid())
        return true; // 非 HTTP 传输（如 file://）
    const int code = status.toInt();
    return code >= 200 && code < 300;
}

QString HttpClient::describeError(QNetworkReply *reply)
{
    if (!reply)
        return QStringLiteral("unknown error");

    QString message = reply->errorString();
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid())
        message = QStringLiteral("HTTP %1: %2").arg(status.toInt()).arg(message);
    return message;
}
