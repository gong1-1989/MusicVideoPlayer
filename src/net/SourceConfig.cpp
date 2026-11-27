/// @file SourceConfig.cpp
/// @brief 搜索源配置的 JSON 序列化、校验与敏感头处理实现。
#include "net/SourceConfig.h"

#include "util/HeaderPolicy.h"
#include "util/SecretStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QUrl>

namespace {

/// @brief 把请求头表转成 JSON 对象，跳过空键。
QJsonObject headersToJson(const QHash<QString, QString> &headers)
{
    QJsonObject object;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (!it.key().trimmed().isEmpty())
            object.insert(it.key().trimmed(), it.value());
    }
    return object;
}

/// @brief 把 JSON 对象转成请求头表，值统一转字符串。
QHash<QString, QString> headersFromJson(const QJsonObject &object)
{
    QHash<QString, QString> headers;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        headers.insert(it.key(), it.value().toString());
    return headers;
}

/// @brief 判断是否为连接级、必须由网络层生成的请求头。
/// @return true 表示配置里禁止透传。
/// @note 配置请求头只表达“业务身份”（Referer/Origin/UA 等）；Host/Length
///       原样透传会破坏重定向、分块传输，甚至让请求共享错误的长度。
bool isForbiddenRequestHeader(const QString &name)
{
    const QString lower = name.trimmed().toLower();
    return lower == QLatin1String("host")
        || lower == QLatin1String("content-length")
        || lower == QLatin1String("transfer-encoding")
        || lower == QLatin1String("connection")
        || lower == QLatin1String("proxy-connection")
        || lower == QLatin1String("keep-alive")
        || lower == QLatin1String("upgrade")
        || lower == QLatin1String("te")
        || lower == QLatin1String("trailer")
        || lower == QLatin1String("expect")
        || lower == QLatin1String("proxy-authenticate");
}

/// @brief 检测文本中是否含 CR/LF，防止请求头注入。
bool hasHeaderInjection(const QString &text)
{
    return text.contains(QLatin1Char('\r')) || text.contains(QLatin1Char('\n'));
}

/// @brief 把请求头写到 JSON：普通头明文，敏感头用 DPAPI 加密到 headersProtected。
/// @param headers 待序列化头表。
/// @param sourceId 加密附加熵的一部分，绑定源 id。
/// @param protectedHeaders 输出敏感头密文对象，可为空。
/// @param exportPlainSecrets true 时按用户显式导出写明文。
/// @return 明文头对象；无法加密的敏感头直接丢弃，不退回明文。
QJsonObject requestHeadersToJson(const QHash<QString, QString> &headers,
                                 const QString &sourceId,
                                 QJsonObject *protectedHeaders,
                                 bool exportPlainSecrets = false)
{
    QJsonObject object;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const QString key = it.key().trimmed();
        if (key.isEmpty())
            continue;
        if (HeaderPolicy::isSensitiveHeader(key)) {
            if (exportPlainSecrets) {
                // 导出是用户显式搬配置的动作；DPAPI 密文跨机器无法解密，
                // 这里写明文并在 UI 侧提示用户妥善保管。
                object.insert(key, it.value());
                continue;
            }
            const QString cipher = SecretStore::protectForStorage(
                it.value(), sourceId + QLatin1Char('/') + key);
            if (cipher.isEmpty())
                continue; // 无法加密时宁可丢字段，也不退回明文
            if (protectedHeaders != nullptr)
                protectedHeaders->insert(key, cipher);
            continue;
        }
        object.insert(key, it.value());
    }
    return object;
}

} // namespace

/// @brief 判断配置是否足以发起搜索。
/// @return 必填字段齐全且地址/详情地址为 http(s)、存在可用 streamUrl 字段。
/// @note 规则与 validationError() 保持同口径，避免 isValid 通过但提示为空。
bool SourceConfig::isValid() const
{
    if (id.trimmed().isEmpty() || name.trimmed().isEmpty())
        return false;
    if (hasHeaderInjection(cookie))
        return false;

    if (kind == QLatin1String("builtin")) {
        // 内置实现见 createSearchProvider()：目前只剩网页型影视（内置抓取）
        if (provider == QLatin1String("maccms_web")) {
            if (mediaType != MediaType::Video || urlTemplate.trimmed().isEmpty())
                return false;
            // 站点地址最终交给 QNetworkRequest；file:/// 等协议必须在启停判定
            // 阶段就拒绝，与 validationError() 保持同一口径。
            const QString scheme = QUrl(urlTemplate).scheme();
            return scheme == QLatin1String("http") || scheme == QLatin1String("https");
        }
        return false;
    }

    if (kind == QLatin1String("external")) {
        // 命令可以是绝对路径、PATH 里的程序名，或相对程序目录的脚本
        return !command.trimmed().isEmpty();
    }

    const QString scheme = QUrl(urlTemplate).scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    // 详情模板同样走 QNetworkRequest；手工改 sources.json 时 file:/// 会让
    // 客户端去读本地文件，必须在 isValid() 里与 validationError() 保持同规则。
    if (needsDetailFetch()) {
        const QString detailScheme = QUrl(detailUrlTemplate).scheme();
        if (detailScheme != QLatin1String("http") && detailScheme != QLatin1String("https"))
            return false;
    }
    // 没有播放地址就没法用：要么搜索结果里直接给，要么二次解析详情里给
    const bool streamFromList = !fields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    const bool streamFromDetail = needsDetailFetch()
                                  && !detailFields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    if (!streamFromList && !streamFromDetail)
        return false;
    if (fields.value(QStringLiteral("title")).trimmed().isEmpty())
        return false;
    return true;
}

/// @brief 返回首个校验失败的中文原因；通过时返回空。
QString SourceConfig::validationError() const
{
    if (id.trimmed().isEmpty())
        return QObject::tr("缺少 id");
    if (name.trimmed().isEmpty())
        return QObject::tr("缺少名称");
    if (hasHeaderInjection(cookie))
        return QObject::tr("Cookie 里不能包含换行符");

    if (kind == QLatin1String("builtin")) {
        if (provider == QLatin1String("maccms_web")) {
            if (mediaType != MediaType::Video)
                return QObject::tr("网页型影视源只支持视频类型");
            if (urlTemplate.trimmed().isEmpty())
                return QObject::tr("网页型影视源没填站点地址");
            const QString scheme = QUrl(urlTemplate).scheme();
            if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
                return QObject::tr("站点地址不是 http/https");
            return QString();
        }
        return QObject::tr("内置源类型未知：%1").arg(provider);
    }

    if (kind == QLatin1String("external")) {
        if (command.trimmed().isEmpty())
            return QObject::tr("外部解析器没填命令");
        return QString();
    }

    if (urlTemplate.trimmed().isEmpty())
        return QObject::tr("搜索地址为空");

    const QString scheme = QUrl(urlTemplate).scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QObject::tr("搜索地址不是 http/https");

    if (fields.value(QStringLiteral("title")).trimmed().isEmpty())
        return QObject::tr("缺少标题字段");
    const bool streamFromList = !fields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    const bool streamFromDetail = needsDetailFetch()
                                  && !detailFields.value(QStringLiteral("streamUrl")).trimmed().isEmpty();
    if (!streamFromList && !streamFromDetail)
        return QObject::tr("缺少播放地址字段（可直接给 streamUrl，或用二次解析从详情里取）");
    if (needsDetailFetch()) {
        const QString detailScheme = QUrl(detailUrlTemplate).scheme();
        if (detailScheme != QLatin1String("http") && detailScheme != QLatin1String("https"))
            return QObject::tr("详情地址模板不是 http/https");
    }
    return QString();
}

/// @brief 源实现类型显示名（内置/外部解析器/配置）。
QString SourceConfig::kindLabel() const
{
    if (kind == QLatin1String("builtin"))
        return QObject::tr("内置");
    if (kind == QLatin1String("external"))
        return QObject::tr("外部解析器");
    return QObject::tr("配置");
}

/// @brief 把源配置序列化为 sources.json 中的对象。
/// @param config 待序列化配置。
/// @param exportPlainSecrets true 时敏感头/Cookie 写明文（用户显式导出）。
/// @return JSON 对象；敏感头默认写 headersProtected 密文。
QJsonObject sourceConfigToJson(const SourceConfig &config, bool exportPlainSecrets)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), config.id);
    object.insert(QStringLiteral("name"), config.name);
    object.insert(QStringLiteral("enabled"), config.enabled);
    object.insert(QStringLiteral("builtin"), config.builtin);
    object.insert(QStringLiteral("mediaType"),
                  config.mediaType == MediaType::Video ? QStringLiteral("video")
                                                       : QStringLiteral("music"));
    object.insert(QStringLiteral("kind"), config.kind);
    if (!config.provider.isEmpty())
        object.insert(QStringLiteral("provider"), config.provider);
    if (config.kind == QLatin1String("external")) {
        object.insert(QStringLiteral("command"), config.command);
        object.insert(QStringLiteral("arguments"), config.arguments);
    }
    // 0 表示自动预算，不落盘；大于 0 时配置型/内置型/外部解析器都按该值限时。
    if (config.timeoutMs > 0)
        object.insert(QStringLiteral("timeoutMs"), config.timeoutMs);
    object.insert(QStringLiteral("method"), config.method.toUpper());
    object.insert(QStringLiteral("url"), config.urlTemplate);
    if (!config.bodyTemplate.isEmpty()) {
        object.insert(QStringLiteral("body"), config.bodyTemplate);
        object.insert(QStringLiteral("bodyIsJson"), config.bodyIsJson);
    }
    QJsonObject protectedHeaders;
    const QJsonObject plainHeaders = requestHeadersToJson(config.headers, config.id,
                                                           &protectedHeaders,
                                                           exportPlainSecrets);
    if (!plainHeaders.isEmpty())
        object.insert(QStringLiteral("headers"), plainHeaders);
    if (!protectedHeaders.isEmpty())
        object.insert(QStringLiteral("headersProtected"), protectedHeaders);
    if (!config.cookie.isEmpty()) {
        if (exportPlainSecrets) {
            object.insert(QStringLiteral("cookie"), config.cookie);
        } else {
            const QString cipher = SecretStore::protectForStorage(
                config.cookie, config.id + QStringLiteral("/cookie"));
            if (!cipher.isEmpty())
                object.insert(QStringLiteral("cookie"), cipher);
        }
    }
    object.insert(QStringLiteral("itemsPath"), config.itemsPath);
    object.insert(QStringLiteral("fields"), headersToJson(config.fields));
    if (config.streamKind != QLatin1String("direct"))
        object.insert(QStringLiteral("streamKind"), config.streamKind);
    if (!config.skipIf.isEmpty())
        object.insert(QStringLiteral("skipIf"), config.skipIf);
    if (config.needsDetailFetch()) {
        object.insert(QStringLiteral("detailUrl"), config.detailUrlTemplate);
        if (!config.detailItemsPath.isEmpty())
            object.insert(QStringLiteral("detailItemsPath"), config.detailItemsPath);
        if (!config.detailFields.isEmpty())
            object.insert(QStringLiteral("detailFields"), headersToJson(config.detailFields));
    }
    return object;
}

/// @brief 从 JSON 对象构造源配置。
/// @param object sources.json 中的单个源对象。
/// @param ok 输出配置是否通过 isValid()，可为空。
/// @return 构造出的配置；缺失字段用默认值，非法头会被过滤。
SourceConfig sourceConfigFromJson(const QJsonObject &object, bool *ok)
{
    SourceConfig config;
    config.id = object.value(QStringLiteral("id")).toString();
    config.name = object.value(QStringLiteral("name")).toString();
    config.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    config.builtin = object.value(QStringLiteral("builtin")).toBool(false);
    config.mediaType = object.value(QStringLiteral("mediaType")).toString() == QLatin1String("video")
                           ? MediaType::Video
                           : MediaType::Music;
    config.kind = object.value(QStringLiteral("kind")).toString(QStringLiteral("config"));
    config.provider = object.value(QStringLiteral("provider")).toString();
    // 旧的网页型影视源可能没写 mediaType；既然实现固定返回视频结果，
    // 读取时就把它归一到视频，避免配置被新校验判为无效。
    if (config.kind == QLatin1String("builtin")
        && config.provider == QLatin1String("maccms_web")) {
        config.mediaType = MediaType::Video;
    }
    config.command = object.value(QStringLiteral("command")).toString();
    config.arguments = object.value(QStringLiteral("arguments")).toString();
    // 未填写或过小的值都视为“自动”，由 SearchCoordinator 按源类型/条数分配；
    // 旧配置里残留的 100ms 不能继续把请求瞬间掐断。
    config.timeoutMs = object.value(QStringLiteral("timeoutMs")).toInt(0);
    if (config.timeoutMs > 0 && config.timeoutMs < 1000) {
        // 用户显式填了 1-999ms 时不能悄悄变成“自动”（0）；按最小可用的
        // 1 秒处理，至少保证请求有机会完成。
        config.timeoutMs = 1000;
    } else if (config.timeoutMs < 0) {
        config.timeoutMs = 0;
    }
    config.method = object.value(QStringLiteral("method")).toString(QStringLiteral("GET")).toUpper();
    config.urlTemplate = object.value(QStringLiteral("url")).toString();
    config.bodyTemplate = object.value(QStringLiteral("body")).toString();
    config.bodyIsJson = object.value(QStringLiteral("bodyIsJson")).toBool(false);
    config.headers.clear();
    const QJsonObject plainHeaders = object.value(QStringLiteral("headers")).toObject();
    for (auto it = plainHeaders.constBegin(); it != plainHeaders.constEnd(); ++it) {
        const QString key = it.key();
        if (isForbiddenRequestHeader(key) || hasHeaderInjection(key)
            || hasHeaderInjection(it.value().toString()))
            continue;
        QString value = it.value().toString();
        // 兼容旧版：敏感头过去也写在 headers 里，读出后下次保存会迁到 headersProtected。
        if (HeaderPolicy::isSensitiveHeader(key))
            value = SecretStore::revealStored(value, config.id + QLatin1Char('/') + key);
        if (!value.isEmpty())
            config.headers.insert(key, value);
    }
    const QJsonObject protectedHeaders =
        object.value(QStringLiteral("headersProtected")).toObject();
    for (auto it = protectedHeaders.constBegin(); it != protectedHeaders.constEnd(); ++it) {
        if (isForbiddenRequestHeader(it.key()) || hasHeaderInjection(it.key())
            || hasHeaderInjection(it.value().toString()))
            continue;
        const QString value = SecretStore::revealStored(
            it.value().toString(), config.id + QLatin1Char('/') + it.key());
        if (!value.isEmpty())
            config.headers.insert(it.key(), value);
    }
    config.cookie = SecretStore::revealStored(object.value(QStringLiteral("cookie")).toString(),
                                              config.id + QStringLiteral("/cookie"));
    // Cookie 和普通 header 走同一条请求通道；CR/LF 会在 HttpClient 层被丢弃，
    // 但读入时先清掉，避免 UI 显示“已配置”而实际请求从未带上。
    if (hasHeaderInjection(config.cookie))
        config.cookie.clear();
    config.itemsPath = object.value(QStringLiteral("itemsPath")).toString();
    config.fields = headersFromJson(object.value(QStringLiteral("fields")).toObject());
    config.streamKind = object.value(QStringLiteral("streamKind"))
                            .toString(QStringLiteral("direct"));
    if (config.streamKind != QLatin1String("episodes"))
        config.streamKind = QStringLiteral("direct");
    config.skipIf = object.value(QStringLiteral("skipIf")).toObject();
    config.detailUrlTemplate = object.value(QStringLiteral("detailUrl")).toString();
    config.detailItemsPath = object.value(QStringLiteral("detailItemsPath")).toString();
    config.detailFields =
        headersFromJson(object.value(QStringLiteral("detailFields")).toObject());

    if (ok != nullptr)
        *ok = config.isValid();
    return config;
}
