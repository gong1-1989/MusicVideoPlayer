/// @file HlsPlaylist.cpp
/// @brief HLS 播放列表解析、分片时间轴定位与代理地址重写。
#include "stream/HlsPlaylist.h"
#include "net/NetLimits.h"
#include "util/NetworkAddressPolicy.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>

namespace {
// UTF-8 BOM 解码后会变成 U+FEFF，`trimmed()` 不会删它。带 BOM 时首行
// 变成 `\uFEFF#EXTM3U`，既不是注释也不是合法相对 URI，重写会把整份
// 播放列表判成非法分片并返回 502，所以在解析和重写入口统一剥掉。
QString decodePlaylist(const QByteArray &body)
{
    QString source = QString::fromUtf8(body);
    if (source.startsWith(QChar(0xFEFF)))
        source.remove(0, 1);
    return source;
}

/// 从 #EXT-X-STREAM-INF 行里取属性；属性值可能带引号，也可能不带。
QString playlistAttribute(const QString &line, const QString &name)
{
    // 这里按调用构造正则：解析只发生在 master 加载时，不值得为了微小开销
    // 引入可变的全局正则缓存，避免代理线程和测试线程同时解析时的竞态。
    const QRegularExpression pattern(
        QStringLiteral("%1=(?:\"([^\"]*)\"|([^,\\s]*))")
            .arg(QRegularExpression::escape(name)));
    const QRegularExpressionMatch match = pattern.match(line);
    if (!match.hasMatch())
        return {};
    return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
}
} // namespace

/// @brief 解析媒体播放列表，建立分片地址与起始时间轴。
/// @param body 播放列表原文。
/// @param playlistUrl 播放列表 URL，用于解析相对地址和 BASE。
/// @return 解析成功返回 Info；无分片、分片超限或全为非 HTTP(S) 资源时返回空。
/// @note 只有带 EXTINF 的行才被当作分片；master 列表的变体地址会被跳过。
///       BYTERANGE 与非 HTTP(S) 分片会关闭预取，避免重复下载或越权访问。
QSharedPointer<HlsPlaylist::Info> HlsPlaylist::parse(const QByteArray &body,
                                                    const QUrl &playlistUrl)
{
    const QString source = decodePlaylist(body);
    QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    QUrl base = playlistUrl;
    QList<QUrl> segments;
    QList<qint64> starts;
    qint64 cumulativeMs = 0;
    double pendingDuration = -1.0;
    bool hasByteRange = false;
    bool hasNonHttp = false;

    for (const QString &raw : std::as_const(lines)) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-BASE:"))) {
            const QString value = line.mid(QStringLiteral("#EXT-X-BASE:").size()).trimmed();
            const QUrl next = base.resolved(QUrl(value));
            if (next.isValid())
                base = next;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXTINF:"))) {
            const QString value =
                line.mid(QStringLiteral("#EXTINF:").size()).section(QLatin1Char(','), 0, 0);
            bool ok = false;
            const double seconds = value.trimmed().toDouble(&ok);
            pendingDuration = ok && seconds > 0.0 ? seconds : -1.0;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-BYTERANGE:"))) {
            hasByteRange = true;
            continue;
        }

        if (line.startsWith(QLatin1Char('#')))
            continue;

        if (pendingDuration < 0.0)
            continue; // 这是 master playlist 的变体地址，不是媒体分片

        const QUrl segment = base.resolved(QUrl(line));
        if (!NetworkAddressPolicy::isHttpMediaUrl(segment)) {
            hasNonHttp = true;
        } else {
            // 分片数量异常多时不做预取规划，播放仍可直接走代理列表，
            // 避免为超大列表维护 starts/segments 两份数组。
            if (segments.size() >= NetLimits::kPlaylistSegments)
                return {};
            segments.append(segment);
            starts.append(cumulativeMs);
        }
        cumulativeMs += qRound64(pendingDuration * 1000.0);
        pendingDuration = -1.0;
    }

    if (segments.isEmpty())
        return {};

    auto info = QSharedPointer<Info>::create();
    info->source = playlistUrl;
    info->segments = segments;
    info->starts = starts;
    info->totalMs = cumulativeMs;
    info->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    // 带 BYTERANGE 的播放列表无法只靠 URL 缓存整段，直接关掉预取，避免重复下载。
    info->prefetchable = !hasByteRange && !hasNonHttp;
    return info;
}

/// @brief 二分查找给定播放位置所在的分片下标。
/// @param info 已解析的播放列表。
/// @param positionMs 播放位置（毫秒）。
/// @return 分片下标；无分片时返回 -1，越界时夹到首/尾分片。
int HlsPlaylist::segmentIndexAt(const Info &info, qint64 positionMs)
{
    if (info.starts.isEmpty())
        return -1;
    if (positionMs <= info.starts.first())
        return 0;
    if (positionMs >= info.starts.last())
        return info.starts.size() - 1;

    int low = 0;
    int high = info.starts.size(); // 上界取不到
    while (low + 1 < high) {
        const int mid = (low + high) / 2;
        if (info.starts.at(mid) <= positionMs)
            low = mid;
        else
            high = mid;
    }
    return low;
}

/// @brief 判断主列表与媒体列表是否属于同一预取作用域。
/// @param a 第一个 URL。
/// @param b 第二个 URL。
/// @return 同协议、同主机、同端口且目录相同（或仅多一级常见码率/音轨子目录）时为 true。
/// @note 只允许父目录 + 一级变体子目录的形态，防止把另一部片的索引当成当前片分片。
bool HlsPlaylist::sameScope(const QUrl &a, const QUrl &b)
{
    if (a.scheme().compare(b.scheme(), Qt::CaseInsensitive) != 0)
        return false;
    if (a.host().compare(b.host(), Qt::CaseInsensitive) != 0)
        return false;
    const int portA = a.port() >= 0 ? a.port()
                                    : (a.scheme().compare(QLatin1String("https"),
                                                         Qt::CaseInsensitive) == 0 ? 443 : 80);
    const int portB = b.port() >= 0 ? b.port()
                                    : (b.scheme().compare(QLatin1String("https"),
                                                         Qt::CaseInsensitive) == 0 ? 443 : 80);
    if (portA != portB)
        return false;

    const auto directory = [](const QUrl &url) {
        const QString path = url.path();
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return slash >= 0 ? path.left(slash + 1) : QStringLiteral("/");
    };
    const QString left = directory(a);
    const QString right = directory(b);
    // 只认完全相同目录。主列表与媒体列表同目录时仍能回退预取；
    // /videos/2024/ 与 /videos/2024/other/ 这种父子目录不能再共用，
    // 否则会把另一部片的索引当成当前片的分片。
    if (left == right)
        return true;

    const QString &shorter = left.size() < right.size() ? left : right;
    const QString &longer = left.size() < right.size() ? right : left;
    if (shorter == QLatin1String("/") || !longer.startsWith(shorter))
        return false;

    // 只允许“主列表在父目录、媒体列表在直接的码率/音轨子目录”。
    // 子目录名必须是 hls 常见的变体名，避免把 /videos/2024/other/ 这种
    // 任意内容目录也当成同一部片的清晰度目录。
    const QString tail = longer.mid(shorter.size());
    if (tail.isEmpty() || !tail.endsWith(QLatin1Char('/')))
        return false;
    const QString component = tail.left(tail.size() - 1);
    if (component.isEmpty() || component.contains(QLatin1Char('/')))
        return false;
    static const QRegularExpression variantDir(
        QStringLiteral("^(?:\\d{3,4}[pP]?|\\d{3,4}[kK]|hd|sd|uhd|4k|8k|audio|video|hls|mp4|fmp4|"
                       "chunklist|chunks?|segments?|index|playlist|streams?|level|bitrate|"
                       "media|main|v\\d+)$"),
        QRegularExpression::CaseInsensitiveOption);
    return variantDir.match(component).hasMatch();
}

/// @brief 解析 master 中的视频码率变体，供播放区域码率栏展示和选择。
/// @param body master 正文。
/// @param playlistUrl master 地址，用于解析相对子列表。
/// @return 有效 HTTP(S) 子列表的变体列表；序号保留原始文件顺序。
QList<HlsPlaylist::Variant> HlsPlaylist::variants(const QByteArray &body,
                                                  const QUrl &playlistUrl)
{
    const QString source = decodePlaylist(body);
    QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    QUrl base = playlistUrl;
    QList<Variant> result;
    int variantIndex = -1;
    bool pending = false;
    Variant current;

    for (const QString &raw : std::as_const(lines)) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-BASE:"))) {
            const QString value = line.mid(QStringLiteral("#EXT-X-BASE:").size()).trimmed();
            const QUrl next = base.resolved(QUrl(value));
            if (next.isValid())
                base = next;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
            ++variantIndex;
            current = Variant();
            current.index = variantIndex;
            const QString bandwidth = playlistAttribute(line, QStringLiteral("BANDWIDTH"));
            bool bandwidthOk = false;
            current.bandwidth = bandwidth.toInt(&bandwidthOk);
            if (!bandwidthOk)
                current.bandwidth = 0;
            current.resolution = playlistAttribute(line, QStringLiteral("RESOLUTION"));
            current.codecs = playlistAttribute(line, QStringLiteral("CODECS"));
            current.audioGroup = playlistAttribute(line, QStringLiteral("AUDIO"));
            pending = true;
            continue;
        }

        if (!pending)
            continue;
        if (line.startsWith(QLatin1Char('#')))
            continue;

        const QUrl uri = base.resolved(QUrl(line));
        if (NetworkAddressPolicy::isHttpMediaUrl(uri)) {
            current.uri = uri;
            result.append(current);
        }
        pending = false;
    }
    return result;
}

/// @brief 只保留用户选择的视频变体，其他码率块从 master 中删掉。
/// @param body master 正文。
/// @param playlistUrl master 地址（保留参数是为了和 variants 保持同一接口形态）。
/// @param variantIndex 变体在原始文件里的序号；小于 0 表示不裁剪。
/// @return 裁剪后的正文；序号不存在时原样返回，避免选错时播放直接失败。
QByteArray HlsPlaylist::restrictToVariant(const QByteArray &body, const QUrl &playlistUrl,
                                           int variantIndex)
{
    if (variantIndex < 0)
        return body;

    const QString source = decodePlaylist(body);
    const bool crlf = source.contains(QLatin1String("\r\n"));
    QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    Q_UNUSED(playlistUrl)

    QStringList output;
    QStringList pendingLines;
    int currentIndex = -1;
    bool pending = false;
    bool keep = false;
    bool found = false;

    for (const QString &raw : std::as_const(lines)) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
            // 正常 master 不会在上一个变体 URI 之前再开一个变体；真遇到
            // 这种畸形文件时先原样吐出前一个块，避免丢内容。
            if (pending)
                output.append(pendingLines);
            ++currentIndex;
            keep = currentIndex == variantIndex;
            found = found || keep;
            pending = true;
            pendingLines.clear();
            pendingLines.append(raw);
            continue;
        }

        if (pending) {
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                pendingLines.append(raw);
                continue;
            }
            if (keep) {
                output.append(pendingLines);
                output.append(raw);
            }
            pending = false;
            pendingLines.clear();
            continue;
        }

        output.append(raw);
    }
    if (pending && keep)
        output.append(pendingLines);

    if (!found)
        return body;
    return output.join(crlf ? QStringLiteral("\r\n") : QStringLiteral("\n")).toUtf8();
}

/// @brief 把播放列表中的分片、KEY、MAP、MEDIA 等 URI 全部改写为本地代理地址。
/// @param body 原始播放列表。
/// @param playlistUrl 播放列表 URL，用于解析相对地址。
/// @param proxyFor 将远端绝对 URL 转成本地代理目录 URL 的回调。
/// @param error 失败原因输出，可为空。
/// @return 改写后的播放列表；遇到非 HTTP(S) 资源或代理建立失败时返回空。
/// @note 保留原换行风格；同一 URL 只建立一次代理并复用；引号形式的 URI 会连引号一起替换。
QByteArray HlsPlaylist::rewrite(const QByteArray &body, const QUrl &playlistUrl,
                                const ProxyResolver &proxyFor, QString *error)
{
    if (error != nullptr)
        error->clear();

    const QString source = decodePlaylist(body);
    const bool crlf = source.contains(QLatin1String("\r\n"));
    QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    QUrl base = playlistUrl;
    QHash<QString, QString> tokenCache;
    QString failure;

    auto proxyForSegment = [&base, &proxyFor, &tokenCache, &failure](const QString &raw) -> QString {
        const QString uri = raw.trimmed();
        if (uri.isEmpty())
            return raw;

        const QUrl absolute = base.resolved(QUrl(uri));
        if (!NetworkAddressPolicy::isHttpMediaUrl(absolute)) {
            failure = QObject::tr("播放列表包含非 HTTP(S) 资源：%1").arg(uri.left(160));
            return QString();
        }

        const QString key = absolute.toString(QUrl::FullyEncoded);
        if (tokenCache.contains(key))
            return tokenCache.value(key);

        const QUrl proxy = proxyFor(absolute);
        if (!proxy.isValid()) {
            failure = QObject::tr("无法为播放列表资源建立本地代理：%1").arg(uri.left(160));
            return QString();
        }

        // proxyUrlFor 返回的是“目录”形式（/stream/<token>/）。如果把整个分片地址
        // 直接换成这个目录，FFmpeg 会把同一个目录当成所有分片反复请求，最终报
        // “URL ... is not in allowed_segment_extensions”。必须把分片文件名带回路径，
        // 再由 StreamProxy 把后缀拼到 token 对应的绝对地址上。
        QString value = proxy.toString(QUrl::FullyEncoded);
        const QString fileName = absolute.fileName(QUrl::FullyEncoded);
        if (!fileName.isEmpty())
            value += fileName;
        const QString query = absolute.query(QUrl::FullyEncoded);
        if (!query.isEmpty())
            value += QLatin1Char('?') + query;
        tokenCache.insert(key, value);
        return value;
    };

    // URI 属性既可能带引号也可能不带引号，两种写法都要识别并改写；
    // 仅匹配带引号的形式会让 URI=file:///... 绕过代理。
    static const QRegularExpression uriAttribute(
        QStringLiteral("URI=(?:\"([^\"]*)\"|([^,\\s]*))"));
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;

        if (!trimmed.startsWith(QLatin1Char('#'))) {
            line = proxyForSegment(trimmed);
            if (!failure.isEmpty())
                break;
            continue;
        }

        if (trimmed.startsWith(QLatin1String("#EXT-X-BASE:"))) {
            const QString value =
                trimmed.mid(QStringLiteral("#EXT-X-BASE:").size()).trimmed();
            const QUrl next = base.resolved(QUrl(value));
            if (!NetworkAddressPolicy::isHttpMediaUrl(next)) {
                failure = QObject::tr("播放列表的 BASE 不是 HTTP(S)：%1").arg(value.left(160));
                break;
            }
            base = next;
            line = trimmed;
            continue;
        }

        if (!trimmed.contains(QLatin1String("URI=")))
            continue;

        // #EXT-X-KEY / #EXT-X-MAP / #EXT-X-MEDIA / LL-HLS 的 PART 等都带 URI 属性，
        // 不重写的话播放器会直接去 CDN 取 key/init 段，丢掉 Referer/Cookie。
        QString rewritten;
        int last = 0;
        QRegularExpressionMatchIterator it = uriAttribute.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const bool quoted = match.capturedStart(1) >= 0;
            const int valueStart = quoted ? match.capturedStart(1) : match.capturedStart(2);
            const int valueEnd = quoted ? match.capturedEnd(1) : match.capturedEnd(2);
            const QString rawValue = quoted ? match.captured(1) : match.captured(2);
            // capturedStart 指向引号内的内容；替换时必须连原引号一起吃掉，
            // 否则会拼出 URI=""http://..."" 这种双引号。
            const int replaceStart = quoted ? valueStart - 1 : valueStart;
            const int replaceEnd = quoted ? valueEnd + 1 : valueEnd;
            rewritten += line.mid(last, replaceStart - last);
            const QString newValue = proxyForSegment(rawValue);
            if (!failure.isEmpty())
                break;
            if (quoted)
                rewritten += QLatin1Char('"') + newValue + QLatin1Char('"');
            else
                rewritten += newValue;
            last = replaceEnd;
        }
        if (!failure.isEmpty())
            break;
        rewritten += line.mid(last);
        line = rewritten;
    }

    if (!failure.isEmpty()) {
        if (error != nullptr)
            *error = failure;
        return {};
    }
    return lines.join(crlf ? QStringLiteral("\r\n") : QStringLiteral("\n")).toUtf8();
}
