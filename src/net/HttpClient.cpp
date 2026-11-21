#include "net/HttpClient.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

#include "util/HeaderPolicy.h"

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

/// 跨 host 或 https→http 降级时剥离鉴权头，避免 Cookie/Authorization
/// 被带到第三方 CDN 或明文连接。
QHash<QString, QString> headersForRedirect(const QHash<QString, QString> &headers,
                                           const QUrl &from, const QUrl &to)
{
    return HeaderPolicy::sanitizedForTarget(to, headers, from);
}

bool isSafeHeader(const QString &name, const QByteArray &value)
{
    if (name.isEmpty())
        return false;
    // 头名只允许 HTTP token 字符，禁止中文/空格/冒号，避免拼出畸形请求
    for (const QChar ch : name) {
        const ushort u = ch.unicode();
        const bool alphaNum = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                              || (u >= 'A' && u <= 'Z');
        const bool symbol = QStringLiteral("!#$%&'*+-.^_`|~").contains(ch);
        if (!alphaNum && !symbol)
            return false;
    }
    // 头值不允许 CR/LF/NUL，否则可被配置注入额外头或截断请求
    return !value.contains('\r') && !value.contains('\n') && !value.contains('\0');
}

QNetworkRequest buildRequest(const QUrl &url, const QHash<QString, QString> &extra)
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

    // 各搜索源自定义的头放在最后，允许覆盖上面的默认值。
    // 敏感头（Cookie/Authorization 等）只允许发往 Referer/Origin 对应的源站；
    // 跨 host 或 https→http 降级时统一剥离，避免直链/分片把凭据带到第三方。
    const QHash<QString, QString> safeHeaders = HeaderPolicy::sanitizedForTarget(url, extra);
    for (auto it = safeHeaders.cbegin(); it != safeHeaders.cend(); ++it) {
        const QString key = it.key();
        const QByteArray value = it.value().toUtf8();
        if (!isSafeHeader(key, value)) {
            qCWarning(lcHttp) << "忽略非法请求头：" << key;
            continue;
        }
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
    // 每个 HttpClient 一份独立会话：保留站点 Set-Cookie（登录态、验证码等），
    // 回传时由 Qt 按域名/路径匹配，不再手工把 Cookie 塞给所有请求。
    m_nam->setCookieJar(new QNetworkCookieJar(m_nam));
}

QNetworkReply *HttpClient::get(const QUrl &url, const QByteArray &range,
                               const QHash<QString, QString> &headers)
{
    qCDebug(lcHttp) << "GET" << redactUrl(url) << "分段" << range;
    QNetworkRequest request = buildRequest(url, headers);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);
    return trackReply(m_nam->get(request));
}

QNetworkReply *HttpClient::postForm(const QUrl &url, const QByteArray &body,
                                    const QHash<QString, QString> &headers)
{
    return postJson(url, body, headers);
}

QNetworkReply *HttpClient::postJson(const QUrl &url, const QByteArray &body,
                                    const QHash<QString, QString> &headers)
{
    qCDebug(lcHttp) << "POST" << redactUrl(url) << "请求体" << body.size() << "字节";
    QNetworkRequest request = buildRequest(url, headers);
    const QByteArray contentType =
        headers.value(QStringLiteral("Content-Type"),
                      QStringLiteral("application/x-www-form-urlencoded")).toUtf8();
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    return trackReply(m_nam->post(request, body));
}

void HttpClient::fetch(const QUrl &url, const QByteArray &range, const ReplyHandler &onReady)
{
    fetch(url, range, QHash<QString, QString>(), onReady);
}

void HttpClient::fetch(const QUrl &url, const QByteArray &range,
                       const QHash<QString, QString> &headers, const ReplyHandler &onReady)
{
    fetchInternal(url, range, headers, onReady, 0, false);
}

void HttpClient::stream(const QUrl &url, const QByteArray &range,
                        const QHash<QString, QString> &headers, const ReplyHandler &onReady)
{
    fetchInternal(url, range, headers, onReady, 0, true);
}

QNetworkReply *HttpClient::trackReply(QNetworkReply *reply)
{
    if (reply == nullptr)
        return nullptr;
    m_activeReplies.insert(reply);
    // finished 之后再从集合里摘掉；abortAll() 会先复制再清空，避免同步 finished
    // 修改容器导致迭代器失效。
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { m_activeReplies.remove(reply); });
    return reply;
}

void HttpClient::abortAll()
{
    const QSet<QNetworkReply *> replies = m_activeReplies;
    m_activeReplies.clear();
    for (QNetworkReply *reply : replies) {
        if (reply != nullptr)
            reply->abort();
    }
}

void HttpClient::fetchInternal(const QUrl &url, const QByteArray &range,
                               const QHash<QString, QString> &headers,
                               const ReplyHandler &onReady, int hop, bool deliverOnHeaders)
{
    if (hop > kMaxRedirects || !isAllowedScheme(url)) {
        qCWarning(lcHttp) << "重定向链被拒绝：" << redactUrl(url) << "第" << hop << "跳";
        onReady(nullptr);
        return;
    }

    QNetworkRequest request = buildRequest(url, headers);
    // 默认的 NoLessSafeRedirectPolicy 会拒绝 https→http 的降级跳转，
    // 而 163 的 outer/url 恰好 302 到 http 的 CDN，所以这里改为手动跟随。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);

    qCDebug(lcHttp) << "拉取" << redactUrl(url) << "分段" << range << "第" << hop << "跳";

    QNetworkReply *reply = trackReply(m_nam->get(request));
    auto delivered = std::make_shared<bool>(false);

    connect(reply, &QNetworkReply::metaDataChanged, this,
            [this, reply, url, range, headers, onReady, hop, delivered, deliverOnHeaders] {
        if (*delivered)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!isRedirectStatus(status)) {
            // 流式场景：响应头一到就把 running reply 交给调用方，
            // 否则整段音视频会先被 QNetworkReply 缓冲完才“开始播放/下载”。
            if (deliverOnHeaders && status > 0) {
                *delivered = true;
                onReady(reply);
            }
            return; // 普通 fetch、以及还没有状态码的响应，等 finished 统一下发
        }

        const QUrl target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QUrl next = target.isRelative() ? url.resolved(target) : target;

        *delivered = true;
        reply->abort();
        reply->deleteLater();

        if (!next.isValid()) {
            onReady(nullptr);
            return;
        }
        qCInfo(lcHttp) << "重定向" << status << redactUrl(url) << "->" << redactUrl(next);
        const QHash<QString, QString> nextHeaders = headersForRedirect(headers, url, next);
        fetchInternal(next, range, nextHeaders, onReady, hop + 1, deliverOnHeaders);
    });

    connect(reply, &QNetworkReply::finished, this,
            [reply, onReady, delivered] {
        if (*delivered)
            return;
        *delivered = true;
        onReady(reply); // 网络错误或没有元数据的非 HTTP 传输
    });
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

QByteArray HttpClient::readAllLimited(QNetworkReply *reply, qint64 limit, bool *exceeded)
{
    if (exceeded != nullptr)
        *exceeded = false;
    if (reply == nullptr)
        return {};
    if (limit >= 0 && reply->bytesAvailable() > limit) {
        if (exceeded != nullptr)
            *exceeded = true;
        return {};
    }
    return reply->readAll();
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

QString HttpClient::redactUrl(const QUrl &url)
{
    if (url.isEmpty())
        return QString();

    QUrl copy(url);
    if (!copy.password().isEmpty())
        copy.setPassword(QStringLiteral("***"));

    // 有些 CDN 把签名放在路径段里，只处理 query 不够。
    // 保留文件名和短目录，长度较长且不含扩展名的段按签名/令牌打码。
    const QStringList rawSegments = copy.path().split(QLatin1Char('/'), Qt::KeepEmptyParts);
    QStringList safeSegments;
    safeSegments.reserve(rawSegments.size());
    bool redactNext = false;
    for (const QString &segment : rawSegments) {
        const QString lower = segment.toLower();
        const bool namedSecret = lower == QLatin1String("sign")
            || lower == QLatin1String("signature")
            || lower == QLatin1String("token")
            || lower == QLatin1String("auth")
            || lower == QLatin1String("session")
            || lower == QLatin1String("credential");
        const bool longOpaque = segment.size() >= 20
            && !segment.contains(QLatin1Char('.'))
            && !segment.contains(QLatin1Char('%'))
            && !segment.contains(QLatin1Char(' '));
        if (redactNext || (longOpaque && !namedSecret)) {
            safeSegments.append(QStringLiteral("***"));
            redactNext = false;
            continue;
        }
        safeSegments.append(segment);
        redactNext = namedSecret;
    }
    copy.setPath(safeSegments.join(QLatin1Char('/')));

    if (!copy.hasQuery())
        return copy.toString();

    // 直链常带签名/时效参数，日志只保留参数名，值统一打码
    const QList<QPair<QString, QString>> items = QUrlQuery(url).queryItems();
    QUrlQuery clean;
    for (const auto &item : items) {
        const QString key = item.first.toLower();
        const bool sensitive = key.contains(QLatin1String("sign"))
                               || key.contains(QLatin1String("token"))
                               || key.contains(QLatin1String("secret"))
                               || key.contains(QLatin1String("auth"))
                               || key.contains(QLatin1String("password"))
                               || key.contains(QLatin1String("expire"))
                               || key.contains(QLatin1String("nonce"))
                               || key.contains(QLatin1String("session"))
                               || key.contains(QLatin1String("credential"))
                               || key.contains(QLatin1String("vkey"))
                               || key.contains(QLatin1String("vuutv"))
                               || key == QLatin1String("key")
                               || key == QLatin1String("timestamp");
        clean.addQueryItem(item.first, sensitive ? QStringLiteral("***") : item.second);
    }
    copy.setQuery(clean);
    return copy.toString();
}
