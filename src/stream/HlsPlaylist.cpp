#include "stream/HlsPlaylist.h"
#include "net/NetLimits.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

QSharedPointer<HlsPlaylist::Info> HlsPlaylist::parse(const QByteArray &body,
                                                    const QUrl &playlistUrl)
{
    const QString source = QString::fromUtf8(body);
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
        if (!segment.isValid()
            || (segment.scheme() != QLatin1String("http")
                && segment.scheme() != QLatin1String("https"))) {
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

bool HlsPlaylist::sameScope(const QUrl &a, const QUrl &b)
{
    if (a.host().compare(b.host(), Qt::CaseInsensitive) != 0)
        return false;

    const auto directory = [](const QUrl &url) {
        const QString path = url.path();
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return slash >= 0 ? path.left(slash + 1) : QStringLiteral("/");
    };
    const QString left = directory(a);
    const QString right = directory(b);
    return left.startsWith(right) || right.startsWith(left);
}

QByteArray HlsPlaylist::rewrite(const QByteArray &body, const QUrl &playlistUrl,
                                const ProxyResolver &proxyFor)
{
    const QString source = QString::fromUtf8(body);
    const bool crlf = source.contains(QLatin1String("\r\n"));
    QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    QUrl base = playlistUrl;
    QHash<QString, QString> tokenCache;

    auto proxyForSegment = [&base, &proxyFor, &tokenCache](const QString &raw) -> QString {
        const QString uri = raw.trimmed();
        if (uri.isEmpty())
            return raw;

        const QUrl absolute = base.resolved(QUrl(uri));
        if (!absolute.isValid()
            || (absolute.scheme() != QLatin1String("http")
                && absolute.scheme() != QLatin1String("https"))) {
            return raw; // data:、skd: 等非 HTTP 资源原样保留
        }

        const QString key = absolute.toString(QUrl::FullyEncoded);
        if (tokenCache.contains(key))
            return tokenCache.value(key);

        const QUrl proxy = proxyFor(absolute);
        if (!proxy.isValid())
            return raw;

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

    static const QRegularExpression uriAttribute(QStringLiteral("URI=\"([^\"]*)\""));
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;

        if (!trimmed.startsWith(QLatin1Char('#'))) {
            line = proxyForSegment(trimmed);
            continue;
        }

        if (trimmed.startsWith(QLatin1String("#EXT-X-BASE:"))) {
            const QString value =
                trimmed.mid(QStringLiteral("#EXT-X-BASE:").size()).trimmed();
            const QUrl next = base.resolved(QUrl(value));
            if (next.isValid())
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
            rewritten += line.mid(last, match.capturedStart(1) - last);
            rewritten += proxyForSegment(match.captured(1));
            last = match.capturedEnd(1);
        }
        rewritten += line.mid(last);
        line = rewritten;
    }

    return lines.join(crlf ? QStringLiteral("\r\n") : QStringLiteral("\n")).toUtf8();
}
