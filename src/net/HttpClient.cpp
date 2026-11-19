#include "net/HttpClient.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <memory>

Q_LOGGING_CATEGORY(lcHttp, "mvp.net")

namespace {

constexpr int kMaxRedirects = 5;

bool isRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool isAllowedScheme(const QUrl &url)
{
    return url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https");
}

QNetworkRequest buildRequest(const QUrl &url, const QString &cookie,
                             const QHash<QString, QString> &extra)
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

    const QString host = url.host();
    if (host.contains(QLatin1String("music.163.com"))
        || host.endsWith(QLatin1String(".music.126.net"))) {
        request.setRawHeader("Referer", "https://music.163.com/");
        request.setRawHeader("Origin", "https://music.163.com");
    }
    if (!cookie.isEmpty())
        request.setRawHeader("Cookie", cookie.toUtf8());

    // 各搜索源自定义的头放在最后，允许覆盖上面的默认值
    for (auto it = extra.cbegin(); it != extra.cend(); ++it) {
        const QString key = it.key();
        const QByteArray value = it.value().toUtf8();
        if (key.compare(QLatin1String("User-Agent"), Qt::CaseInsensitive) == 0) {
            request.setHeader(QNetworkRequest::UserAgentHeader, it.value());
            continue;
        }
        request.setRawHeader(key.toUtf8(), value);
    }
    return request;
}

} // namespace

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_nam->setTransferTimeout(15000);
}

QNetworkReply *HttpClient::get(const QUrl &url, const QByteArray &range,
                               const QHash<QString, QString> &headers)
{
    qCDebug(lcHttp) << "GET" << url.toString() << "分段" << range;
    QNetworkRequest request = buildRequest(url, m_cookie, headers);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);
    return m_nam->get(request);
}

QNetworkReply *HttpClient::postForm(const QUrl &url, const QByteArray &body,
                                    const QHash<QString, QString> &headers)
{
    return postJson(url, body, headers);
}

QNetworkReply *HttpClient::postJson(const QUrl &url, const QByteArray &body,
                                    const QHash<QString, QString> &headers)
{
    qCDebug(lcHttp) << "POST" << url.toString() << "请求体" << body.size() << "字节";
    QNetworkRequest request = buildRequest(url, m_cookie, headers);
    const QByteArray contentType =
        headers.value(QStringLiteral("Content-Type"),
                      QStringLiteral("application/x-www-form-urlencoded")).toUtf8();
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    return m_nam->post(request, body);
}

void HttpClient::fetch(const QUrl &url, const QByteArray &range, const ReplyHandler &onReady)
{
    fetch(url, range, QHash<QString, QString>(), onReady);
}

void HttpClient::fetch(const QUrl &url, const QByteArray &range,
                       const QHash<QString, QString> &headers, const ReplyHandler &onReady)
{
    fetchInternal(url, range, headers, onReady, 0);
}

void HttpClient::fetchInternal(const QUrl &url, const QByteArray &range,
                               const QHash<QString, QString> &headers,
                               const ReplyHandler &onReady, int hop)
{
    if (hop > kMaxRedirects || !isAllowedScheme(url)) {
        qCWarning(lcHttp) << "重定向链被拒绝：" << url.toString() << "第" << hop << "跳";
        onReady(nullptr);
        return;
    }

    QNetworkRequest request = buildRequest(url, m_cookie, headers);
    // 默认的 NoLessSafeRedirectPolicy 会拒绝 https→http 的降级跳转，
    // 而 163 的 outer/url 恰好 302 到 http 的 CDN，所以这里改为手动跟随。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);

    qCDebug(lcHttp) << "拉取" << url.toString() << "分段" << range << "第" << hop << "跳";

    QNetworkReply *reply = m_nam->get(request);
    auto delivered = std::make_shared<bool>(false);

    connect(reply, &QNetworkReply::metaDataChanged, this,
            [this, reply, url, range, headers, onReady, hop, delivered] {
        if (*delivered)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!isRedirectStatus(status))
            return; // 正常响应，等 finished 分支统一下发

        const QUrl target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QUrl next = target.isRelative() ? url.resolved(target) : target;

        *delivered = true;
        reply->abort();
        reply->deleteLater();

        if (!next.isValid()) {
            onReady(nullptr);
            return;
        }
        qCInfo(lcHttp) << "重定向" << status << url.toString() << "->" << next.toString();
        fetchInternal(next, range, headers, onReady, hop + 1);
    });

    connect(reply, &QNetworkReply::finished, this,
            [reply, onReady, delivered] {
        if (*delivered)
            return;
        *delivered = true;
        onReady(reply); // 网络错误或没有元数据的非 HTTP 传输
    });
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
