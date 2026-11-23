#include "util/NetworkAddressPolicy.h"

#include <QHostAddress>

namespace {

/// IPv4 私有/保留段判断。这里不依赖 DNS，只处理字面 IP。
bool isPrivateIpv4(quint32 value)
{
    const quint8 a = static_cast<quint8>(value >> 24);
    const quint8 b = static_cast<quint8>(value >> 16);
    const quint8 c = static_cast<quint8>(value >> 8);

    // 0/8（含 0.0.0.0）、10/8、100.64/10（CGNAT）、127/8（回环）
    if (a == 0 || a == 10 || a == 127)
        return true;
    if (a == 100 && b >= 64 && b <= 127)
        return true;
    // 169.254/16 链路本地、172.16/12、192.168/16
    if (a == 169 && b == 254)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    if (a == 192 && b == 168)
        return true;
    // 198.18/15 基准测试、224/4 组播、240/4 保留
    if (a == 198 && (b == 18 || b == 19))
        return true;
    if (a >= 224)
        return true;
    // 192.0.0/24、192.0.2/24、198.51.100/24、203.0.113/24 等文档/保留段
    if (a == 192 && b == 0 && (c == 0 || c == 2))
        return true;
    if (a == 198 && b == 51 && c == 100)
        return true;
    if (a == 203 && b == 0 && c == 113)
        return true;
    return false;
}

bool isPrivateIpv6(const QIPv6Address &address)
{
    const quint8 *bytes = address.c;
    // ::1 回环 / :: 未指定
    bool allZeroExceptLast = true;
    for (int i = 0; i < 15; ++i)
        allZeroExceptLast = allZeroExceptLast && bytes[i] == 0;
    if (allZeroExceptLast && (bytes[15] == 0 || bytes[15] == 1))
        return true;
    // fe80::/10 链路本地
    if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)
        return true;
    // fc00::/7 唯一本地地址
    if ((bytes[0] & 0xfe) == 0xfc)
        return true;
    // ::ffff:a.b.c.d IPv4 映射地址：按 IPv4 规则再判一次
    bool mappedPrefix = true;
    for (int i = 0; i < 10; ++i)
        mappedPrefix = mappedPrefix && bytes[i] == 0;
    if (mappedPrefix && bytes[10] == 0xff && bytes[11] == 0xff) {
        const quint32 v4 = (static_cast<quint32>(bytes[12]) << 24)
            | (static_cast<quint32>(bytes[13]) << 16)
            | (static_cast<quint32>(bytes[14]) << 8)
            | static_cast<quint32>(bytes[15]);
        return isPrivateIpv4(v4);
    }
    return false;
}

} // namespace

bool NetworkAddressPolicy::isLocalOrPrivateHost(const QUrl &url)
{
    QString host = url.host().trimmed().toLower();
    if (host.endsWith(QLatin1Char('.')) && host.size() > 1)
        host.chop(1);
    if (host.isEmpty())
        return false;

    // 常见本机/云元数据域名。只做精确匹配或 .localhost 后缀，避免把
    // `localhost.evil.example` 误判成可信本机地址。
    if (host == QLatin1String("localhost")
        || host.endsWith(QLatin1String(".localhost"))
        || host == QLatin1String("localhost.localdomain")
        || host == QLatin1String("ip6-localhost")
        || host == QLatin1String("metadata")
        || host == QLatin1String("metadata.google.internal")
        || host == QLatin1String("instance-data")) {
        return true;
    }

    QHostAddress address;
    if (!address.setAddress(host))
        return false;

    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return isPrivateIpv4(address.toIPv4Address());
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return isPrivateIpv6(address.toIPv6Address());
    return false;
}

bool NetworkAddressPolicy::isHttpMediaUrl(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

bool NetworkAddressPolicy::allowsMediaTarget(const QUrl &target, const QUrl &sourceOrigin)
{
    if (!isHttpMediaUrl(target))
        return false;
    if (!isLocalOrPrivateHost(target))
        return true;

    // 没有可信源站信息的旧条目/手工条目保持兼容；有源站信息时，只有用户
    // 自己配置的本地源才允许访问本机/内网，防止公网页面把请求引到内网。
    if (!sourceOrigin.isValid() || sourceOrigin.host().isEmpty())
        return true;
    return isLocalOrPrivateHost(sourceOrigin);
}
