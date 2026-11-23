/// @file HttpClient.cpp
/// @brief 统一 HTTP 客户端：请求头安全过滤、手动重定向前跟随、响应体上限与 URL 脱敏。
#include "net/HttpClient.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtGlobal>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <memory>

#include "util/HeaderPolicy.h"
#include "util/NetworkAddressPolicy.h"

Q_LOGGING_CATEGORY(lcHttp, "mvp.net")

namespace {

constexpr int kMaxRedirects = 5;
const char kLimitBytesProperty[] = "mvpReplyLimitBytes";
const char kLimitExceededProperty[] = "mvpReplyLimitExceeded";
const char kLimitHookedProperty[] = "mvpReplyLimitHooked";
const char kReceivedBytesProperty[] = "mvpReplyReceivedBytes";

/// @brief 已接收字节超过上限时中止响应。
/// @param reply 目标响应。
/// @note QNetworkReply 没有累计接收字节的 getter，优先使用 downloadProgress 记录的
///       累计值，bytesAvailable 仅兜底尚未触发 progress 的同步响应。
void abortReplyIfOverLimit(QNetworkReply *reply)
{
    if (reply == nullptr)
        return;
    const qint64 limit = reply->property(kLimitBytesProperty).toLongLong();
    if (limit <= 0 || reply->property(kLimitExceededProperty).toBool())
        return;

    // QNetworkReply 没有公开“已接收总字节”的 getter。downloadProgress 的
    // bytesReceived 是累计值，即使调用方已经 readAll() 消费正文仍然可靠；
    // bytesAvailable 只能兜底还没触发过 progress 的同步响应。
    const qint64 received = qMax(reply->property(kReceivedBytesProperty).toLongLong(),
                                 reply->bytesAvailable());
    if (received <= limit)
        return;

    reply->setProperty(kLimitExceededProperty, true);
    qCWarning(lcHttp) << "响应接收超过" << limit << "字节上限，主动中止："
                      << HttpClient::redactUrl(reply->url());
    reply->abort();
}


/// @brief 判断状态码是否为需要跟随的重定向。
bool isRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/// @brief 只允许 http/https 协议。
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

/// @brief 校验头名是合法 HTTP token，头值不含 CR/LF/NUL。
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

/// @brief 构造带默认头与安全过滤的请求。
/// @param url 目标地址。
/// @param extra 各源自定义头，可覆盖默认值但会经过 HeaderPolicy 与安全校验。
/// @param sourceOrigin 可信源站；敏感头只会发往该源站。
/// @param redirectPolicy Qt 的重定向策略。
QNetworkRequest buildRequest(const QUrl &url, const QHash<QString, QString> &extra,
                             const QUrl &sourceOrigin,
                             QNetworkRequest::RedirectPolicy redirectPolicy =
                                 QNetworkRequest::NoLessSafeRedirectPolicy)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, redirectPolicy);
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
    // 敏感头（Cookie/Authorization 等）只允许发往显式源站；没有源站信息时
    // 也剥离，避免只配 Cookie 没配 Referer 的源把凭据发给 CDN。
    const QHash<QString, QString> safeHeaders =
        HeaderPolicy::sanitizedForTarget(url, extra, sourceOrigin);
    for (auto it = safeHeaders.cbegin(); it != safeHeaders.cend(); ++it) {
        const QString key = it.key();
        QString text = it.value();
        // 配置里手写的 Referer/Origin 也可能带 API key；统一压成
        // scheme+host+port+path，避免合法头把搜索签名外发给第三方。
        if (key.compare(QLatin1String("Referer"), Qt::CaseInsensitive) == 0) {
            const QUrl referer(text.trimmed());
            const QString safe = HeaderPolicy::refererHeaderValue(referer);
            if (!safe.isEmpty())
                text = safe;
        } else if (key.compare(QLatin1String("Origin"), Qt::CaseInsensitive) == 0) {
            const QUrl originValue(text.trimmed());
            const QString safe = HeaderPolicy::originHeaderValue(originValue);
            if (!safe.isEmpty())
                text = safe;
        }
        const QByteArray value = text.toUtf8();
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

/// @brief 构造 HTTP 客户端：15 秒传输超时，并为每个实例保留独立 Cookie 会话。
HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_nam->setTransferTimeout(15000);
    // 每个 HttpClient 一份独立会话：保留站点 Set-Cookie（登录态、验证码等），
    // 回传时由 Qt 按域名/路径匹配，不再手工把 Cookie 塞给所有请求。
    m_nam->setCookieJar(new QNetworkCookieJar(m_nam));
}

/// @brief 发起带 Range 的 GET 请求。
/// @param url 目标地址。
/// @param range Range 头值，空表示整段。
/// @param headers 自定义请求头。
/// @param maxBytes 响应体上限，<=0 不限制。
/// @param sourceOrigin 可信源站。
/// @return 已登记到活动集合的响应对象。
QNetworkReply *HttpClient::get(const QUrl &url, const QByteArray &range,
                               const QHash<QString, QString> &headers, qint64 maxBytes,
                               const QUrl &sourceOrigin)
{
    qCDebug(lcHttp) << "GET" << redactUrl(url) << "分段" << range;
    if (!NetworkAddressPolicy::allowsMediaTarget(url, sourceOrigin)) {
        qCWarning(lcHttp) << "拒绝本机/内网 GET（源站"
                          << redactUrl(sourceOrigin) << "）：" << redactUrl(url);
        return nullptr;
    }
    QNetworkRequest request =
        buildRequest(url, headers, sourceOrigin,
                     QNetworkRequest::SameOriginRedirectPolicy);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);
    return trackReply(m_nam->get(request), maxBytes);
}

/// @brief 以表单方式提交 POST；实际复用 postJson 的请求构建与上限逻辑。
QNetworkReply *HttpClient::postForm(const QUrl &url, const QByteArray &body,
                                    const QHash<QString, QString> &headers, qint64 maxBytes,
                                    const QUrl &sourceOrigin)
{
    return postJson(url, body, headers, maxBytes, sourceOrigin);
}

/// @brief 发起 POST 请求，Content-Type 来自自定义头，默认表单类型。
QNetworkReply *HttpClient::postJson(const QUrl &url, const QByteArray &body,
                                    const QHash<QString, QString> &headers, qint64 maxBytes,
                                    const QUrl &sourceOrigin)
{
    qCDebug(lcHttp) << "POST" << redactUrl(url) << "请求体" << body.size() << "字节";
    if (!NetworkAddressPolicy::allowsMediaTarget(url, sourceOrigin)) {
        qCWarning(lcHttp) << "拒绝本机/内网 POST（源站"
                          << redactUrl(sourceOrigin) << "）：" << redactUrl(url);
        return nullptr;
    }
    QNetworkRequest request =
        buildRequest(url, headers, sourceOrigin,
                     QNetworkRequest::SameOriginRedirectPolicy);
    QByteArray contentType =
        headers.value(QStringLiteral("Content-Type"),
                      QStringLiteral("application/x-www-form-urlencoded")).toUtf8();
    // buildRequest 已校验 extra 头；但这里用的仍是未过滤的原始值，配置里
    // 带 CRLF 的 Content-Type 会绕过循环直接进 QString/QByteArray，必须再查一次。
    if (!isSafeHeader(QStringLiteral("Content-Type"), contentType))
        contentType = QByteArrayLiteral("application/x-www-form-urlencoded");
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    return trackReply(m_nam->post(request, body), maxBytes);
}

/// @brief 小型响应的一步式获取（默认 JSON 上限）。
void HttpClient::fetch(const QUrl &url, const QByteArray &range, const ReplyHandler &onReady)
{
    fetch(url, range, QHash<QString, QString>(), onReady);
}

/// @brief 带自定义头的 fetch（默认 JSON 上限）。
void HttpClient::fetch(const QUrl &url, const QByteArray &range,
                       const QHash<QString, QString> &headers, const ReplyHandler &onReady)
{
    fetch(url, range, headers, onReady, NetLimits::kJsonBytes);
}

/// @brief 完整版 fetch：可指定响应体上限与可信源站。
void HttpClient::fetch(const QUrl &url, const QByteArray &range,
                       const QHash<QString, QString> &headers, const ReplyHandler &onReady,
                       qint64 maxBytes, const QUrl &sourceOrigin)
{
    fetchInternal(url, range, headers, onReady, 0, false, maxBytes, sourceOrigin);
}

/// @brief 流式获取：响应头到达即回调，供音视频边下边播，不设置响应体上限。
void HttpClient::stream(const QUrl &url, const QByteArray &range,
                        const QHash<QString, QString> &headers, const ReplyHandler &onReady,
                        const QUrl &sourceOrigin)
{
    fetchInternal(url, range, headers, onReady, 0, true, 0, sourceOrigin);
}

/// @brief 把响应登记进活动集合，挂接字节统计与上限检查。
/// @note finished 后再从集合摘除；abortAll() 先复制再清空，避免同步 finished
///       回调修改容器导致迭代器失效。
QNetworkReply *HttpClient::trackReply(QNetworkReply *reply, qint64 maxBytes)
{
    if (reply == nullptr)
        return nullptr;
    m_activeReplies.insert(reply);
    // finished 之后再从集合里摘掉；abortAll() 会先复制再清空，避免同步 finished
    // 修改容器导致迭代器失效。
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { m_activeReplies.remove(reply); });
    // 始终记录累计接收字节，后续再设置上限时不会因为调用方已消费正文而失效。
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
                if (received > reply->property(kReceivedBytesProperty).toLongLong())
                    reply->setProperty(kReceivedBytesProperty, received);
                abortReplyIfOverLimit(reply);
            });
    if (maxBytes > 0)
        limitReplySize(reply, maxBytes);
    return reply;
}

/// @brief 为响应设置接收上限；重复设置时保留更严格的值。
void HttpClient::limitReplySize(QNetworkReply *reply, qint64 maxBytes)
{
    if (reply == nullptr || maxBytes <= 0)
        return;

    // 同一条 reply 可能从多个调用点追加上限，永远取更严格的那个。
    const qint64 existing = reply->property(kLimitBytesProperty).toLongLong();
    if (existing <= 0 || maxBytes < existing)
        reply->setProperty(kLimitBytesProperty, maxBytes);

    if (!reply->property(kLimitHookedProperty).toBool()) {
        reply->setProperty(kLimitHookedProperty, true);
        connect(reply, &QNetworkReply::downloadProgress, reply,
                [reply](qint64, qint64) { abortReplyIfOverLimit(reply); });
        connect(reply, &QNetworkReply::readyRead, reply,
                [reply] { abortReplyIfOverLimit(reply); });
    }
    abortReplyIfOverLimit(reply);
}

/// @brief 中止全部在途响应，用于退出或切换搜索任务。
void HttpClient::abortAll()
{
    const QSet<QNetworkReply *> replies = m_activeReplies;
    m_activeReplies.clear();
    for (QNetworkReply *reply : replies) {
        if (reply != nullptr)
            reply->abort();
    }
}

/// @brief fetch/stream 的统一实现：手动跟随重定向并在每跳重新清洗鉴权头。
/// @param hop 当前跳数，超过 kMaxRedirects 直接回调失败。
/// @param deliverOnHeaders 为 true 时响应头一到就交付 running reply（流式播放）。
/// @note 采用 ManualRedirectPolicy：默认策略会拒绝 https→http 降级，
///       而部分站点（如 163）的外链恰好 302 到 http CDN。
void HttpClient::fetchInternal(const QUrl &url, const QByteArray &range,
                               const QHash<QString, QString> &headers,
                               const ReplyHandler &onReady, int hop, bool deliverOnHeaders,
                               qint64 maxBytes, const QUrl &sourceOrigin)
{
    if (hop > kMaxRedirects || !isAllowedScheme(url)) {
        qCWarning(lcHttp) << "重定向链被拒绝：" << redactUrl(url) << "第" << hop << "跳";
        onReady(nullptr);
        return;
    }

    // 每一跳都按可信源站重新判断：公网源 302/播放列表不能把请求带到
    // 127.0.0.1、192.168.x.x 等本机/内网地址。sourceOrigin 为空的旧调用
    // 保持兼容，由调用方在拿不到源站信息时自行 fail closed。
    if (!NetworkAddressPolicy::allowsMediaTarget(url, sourceOrigin)) {
        qCWarning(lcHttp) << "拒绝本机/内网目标（源站"
                          << redactUrl(sourceOrigin) << "）：" << redactUrl(url);
        onReady(nullptr);
        return;
    }

    QNetworkRequest request = buildRequest(url, headers, sourceOrigin);
    // 默认的 NoLessSafeRedirectPolicy 会拒绝 https→http 的降级跳转，
    // 而 163 的 outer/url 恰好 302 到 http 的 CDN，所以这里改为手动跟随。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (!range.isEmpty())
        request.setRawHeader("Range", range);

    qCDebug(lcHttp) << "拉取" << redactUrl(url) << "分段" << range << "第" << hop << "跳";

    QNetworkReply *reply = trackReply(m_nam->get(request), maxBytes);
    auto delivered = std::make_shared<bool>(false);

    connect(reply, &QNetworkReply::metaDataChanged, this,
            [this, reply, url, range, headers, onReady, hop, delivered, deliverOnHeaders,
             maxBytes, sourceOrigin] {
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
        fetchInternal(next, range, nextHeaders, onReady, hop + 1, deliverOnHeaders,
                      maxBytes, sourceOrigin);
    });

    connect(reply, &QNetworkReply::finished, this,
            [reply, onReady, delivered] {
        if (*delivered)
            return;
        *delivered = true;
        onReady(reply); // 网络错误或没有元数据的非 HTTP 传输
    });
}


/// @brief 判断响应是否可当成功处理。
/// @note 超限 abort() 后 error 为 OperationCanceledError，此时 2xx/无状态码仍算成功，
///       交给 readAllLimited 报“超限”；404/500 的大错误页必须按 HTTP 失败。
bool HttpClient::isHttpSuccess(QNetworkReply *reply)
{
    if (!reply)
        return false;
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    // 在途上限触发 abort() 后，error() 会是 OperationCanceledError。
    // 只有 2xx 或非 HTTP 传输才把这类响应交给 readAllLimited 报“超限”；
    // 404/500 的大错误页仍要按 HTTP 失败，不能因为超限吞掉真实状态码。
    if (reply->property(kLimitExceededProperty).toBool()) {
        if (!status.isValid())
            return true;
        const int code = status.toInt();
        return code >= 200 && code < 300;
    }
    if (reply->error() != QNetworkReply::NoError)
        return false;
    if (!status.isValid())
        return true; // 非 HTTP 传输（如 file://）
    const int code = status.toInt();
    return code >= 200 && code < 300;
}

/// @brief 限长读取响应体。
/// @param limit 字节上限，负数表示不限制。
/// @param exceeded 输出是否已超限；超限时不返回任何数据。
QByteArray HttpClient::readAllLimited(QNetworkReply *reply, qint64 limit, bool *exceeded)
{
    if (exceeded != nullptr)
        *exceeded = false;
    if (reply == nullptr)
        return {};
    if (limit >= 0
        && (reply->property(kLimitExceededProperty).toBool()
            || reply->bytesAvailable() > limit)) {
        if (exceeded != nullptr)
            *exceeded = true;
        return {};
    }
    return reply->readAll();
}

/// @brief 生成可读错误描述，有状态码时带上 HTTP 码。
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

/// @brief 脱敏 URL：打码账号密码、路径中的签名段与敏感查询参数，供日志使用。
/// @return 可直接写日志的地址；空输入返回空。
QString HttpClient::redactUrl(const QUrl &url)
{
    if (url.isEmpty())
        return QString();

    QUrl copy(url);
    if (!copy.userName().isEmpty())
        copy.setUserName(QStringLiteral("***"));
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
            || lower == QLatin1String("sig")
            || lower == QLatin1String("token")
            || lower == QLatin1String("auth")
            || lower == QLatin1String("session")
            || lower == QLatin1String("credential")
            || lower == QLatin1String("hash")
            || lower == QLatin1String("md5");
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
                               || key.contains(QLatin1String("hdnts"))
                               || key.contains(QLatin1String("hash"))
                               || key.contains(QLatin1String("md5"))
                               || key.contains(QLatin1String("sha1"))
                               || key.contains(QLatin1String("sha256"))
                               // Subsonic 兼容接口用单字母参数：u=用户名、p=密码、t=token、s=salt。
                               // 只按参数名打码，不影响 query 里的关键词和分页参数。
                               || key == QLatin1String("u")
                               || key == QLatin1String("p")
                               || key == QLatin1String("t")
                               || key == QLatin1String("s")
                               || key == QLatin1String("sig")
                               || key == QLatin1String("key")
                               || key.endsWith(QLatin1String("key"))
                               || key.contains(QLatin1String("api_key"))
                               || key.contains(QLatin1String("access_key"))
                               || key == QLatin1String("timestamp");
        clean.addQueryItem(item.first, sensitive ? QStringLiteral("***") : item.second);
    }
    copy.setQuery(clean);
    return copy.toString();
}
