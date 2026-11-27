/// @file HlsCacheManifest.cpp
/// @brief HLS 缓存清单的最小解析实现。
#include "cache/HlsCacheManifest.h"

#include "media/MediaItem.h"
#include "stream/HlsPlaylist.h"

#include <QRegularExpression>
#include <QStringList>

namespace {

// 属性正则只匹配缓存需要的字段，不做完整 HLS 语法校验；解析失败由调用方
// 通过空结果/错误文本处理。
const QRegularExpression kBandwidth(QStringLiteral("\\bBANDWIDTH\\s*=\\s*(\\d+)"));
const QRegularExpression kAudioGroup(QStringLiteral("\\bAUDIO\\s*=\\s*\"([^\"]*)\""));
const QRegularExpression kResolution(QStringLiteral("\\bRESOLUTION\\s*=\\s*([^,\\s]+)"));
const QRegularExpression kCodecs(QStringLiteral("\\bCODECS\\s*=\\s*\"([^\"]*)\""));
const QRegularExpression kGroupId(QStringLiteral("\\bGROUP-ID\\s*=\\s*\"([^\"]*)\""));
const QRegularExpression kName(QStringLiteral("\\bNAME\\s*=\\s*\"([^\"]*)\""));
const QRegularExpression kDefault(QStringLiteral("\\bDEFAULT\\s*=\\s*YES"),
                                  QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kUri(
    QStringLiteral("\\bURI\\s*=\\s*(?:\"([^\"]*)\"|([^,\\s]*))"));

/// @brief 从标签行提取 URI 属性，兼容带引号和不带引号两种写法。
QString uriAttribute(const QString &line)
{
    const QRegularExpressionMatch match = kUri.match(line);
    if (!match.hasMatch())
        return QString();
    return match.captured(1).isEmpty() ? match.captured(2).trimmed()
                                       : match.captured(1).trimmed();
}

/// @brief 返回正则第一个捕获组的去空白文本。
QString capture(const QRegularExpression &re, const QString &line)
{
    const QRegularExpressionMatch match = re.match(line);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

} // namespace

namespace HlsCacheManifest {

/// @brief 解析主播放列表。
/// @param body 响应正文（UTF-8）。
/// @param baseUrl 主列表最终地址，用于解析相对 URI。
/// @param out 解析结果；函数开始时会清空。
/// @param error 失败原因，可为空。
/// @return 成功解析出至少一个变体返回 true；正文不是主列表返回 false。
/// @note 同时收集 AUDIO 组；没有 URI 的音频组（内嵌音轨）不会进入列表。
bool parseMaster(const QByteArray &body, const QUrl &baseUrl, Master *out, QString *error)
{
    if (out == nullptr)
        return false;
    *out = Master();
    if (error != nullptr)
        error->clear();

    const QString text = QString::fromUtf8(body);
    if (!text.contains(QLatin1String("#EXT-X-STREAM-INF")))
        return false;

    bool pendingVariant = false;
    Variant pending;
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &raw : lines) {
        if (raw.endsWith(QLatin1Char('\r')))
            raw.chop(1);
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-SESSION-KEY"))) {
            // 子列表可能完全不重复写 KEY，缓存任务必须把主列表的 SESSION-KEY
            // 带过去，否则会把密文当明文合并进 MP4。
            out->sessionKeyLines.append(line);
            const HlsPlaylist::Key key = HlsPlaylist::parseKeyLine(line, baseUrl);
            if (key.enabled || !key.unsupported.isEmpty())
                out->encrypted = true;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-MEDIA:"))
            && line.contains(QLatin1String("TYPE=AUDIO"))) {
            AudioRendition audio;
            audio.groupId = capture(kGroupId, line);
            audio.name = capture(kName, line);
            audio.isDefault = kDefault.match(line).hasMatch();
            const QString uri = uriAttribute(line);
            if (!uri.isEmpty())
                audio.url = baseUrl.resolved(QUrl(uri));
            if (audio.url.isValid() && !audio.url.isEmpty())
                out->audioRenditions.append(audio);
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF"))) {
            pendingVariant = true;
            pending = Variant();
            const QString bandwidth = capture(kBandwidth, line);
            if (!bandwidth.isEmpty())
                pending.bandwidth = bandwidth.toInt();
            pending.audioGroup = capture(kAudioGroup, line);
            pending.resolution = capture(kResolution, line);
            pending.codecs = capture(kCodecs, line);
            continue;
        }

        if (!pendingVariant || line.startsWith(QLatin1Char('#')))
            continue;

        pending.url = baseUrl.resolved(QUrl(line));
        if (pending.url.isValid() && !pending.url.isEmpty())
            out->variants.append(pending);
        pendingVariant = false;
        pending = Variant();
    }

    if (out->variants.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("主播放列表里没有可用的码率地址");
        return false;
    }
    return true;
}

/// @brief 解析媒体播放列表。
/// @param body 响应正文（UTF-8）。
/// @param baseUrl 媒体列表最终地址，用于解析相对分片。
/// @param out 解析结果；函数开始时会清空。
/// @param error 失败原因，可为空。
/// @return 解析出至少一个分片返回 true；不是媒体列表返回 false。
/// @note 只有 #EXTINF 之后的行才视为分片；所有分片同 URL 且都带
///       EXT-X-BYTERANGE 时标记 singleFile，调用方可整段 Range 下载。
bool parseMediaTrack(const QByteArray &body, const QUrl &baseUrl, MediaTrack *out,
                     QString *error)
{
    if (out == nullptr)
        return false;
    *out = MediaTrack();
    if (error != nullptr)
        error->clear();

    const QString text = QString::fromUtf8(body);
    if (!text.contains(QLatin1String("#EXTINF")))
        return false;

    out->isVod = text.contains(QLatin1String("PLAYLIST-TYPE:VOD"));
    out->hasEndList = text.contains(QLatin1String("#EXT-X-ENDLIST"));

    QList<QUrl> segmentUrls;
    QList<bool> segmentRanges;
    bool pendingDuration = false;
    bool pendingRange = false;
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &raw : lines) {
        if (raw.endsWith(QLatin1Char('\r')))
            raw.chop(1);
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-KEY"))
            || line.startsWith(QLatin1String("#EXT-X-SESSION-KEY"))) {
            const HlsPlaylist::Key key = HlsPlaylist::parseKeyLine(line, baseUrl);
            if (key.enabled || !key.unsupported.isEmpty())
                out->encrypted = true;
            continue;
        }
        if (line.startsWith(QLatin1String("#EXTINF:"))) {
            pendingDuration = true;
            continue;
        }
        if (line.startsWith(QLatin1String("#EXT-X-BYTERANGE:"))) {
            pendingRange = true;
            continue;
        }
        if (line.startsWith(QLatin1Char('#')))
            continue;
        if (!pendingDuration)
            continue; // 主列表的变体地址不是媒体分片

        const QUrl segment = baseUrl.resolved(QUrl(line));
        if (!segment.isValid() || segment.isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("媒体播放列表包含无效分片地址");
            return false;
        }
        segmentUrls.append(segment);
        segmentRanges.append(pendingRange);
        pendingDuration = false;
        pendingRange = false;
    }

    if (segmentUrls.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("媒体播放列表里没有分片");
        return false;
    }

    out->valid = true;
    out->segmentCount = segmentUrls.size();
    out->singleFile = !out->encrypted;
    const QUrl first = segmentUrls.first();
    out->singleFileUrl = out->singleFile ? first : QUrl();
    for (int i = 0; i < segmentUrls.size() && out->singleFile; ++i) {
        if (!segmentRanges.at(i) || !sameUrlIgnoringQuery(segmentUrls.at(i), first)) {
            out->singleFile = false;
            out->singleFileUrl = QUrl();
            break;
        }
    }
    return true;
}

/// @brief 判断两个 URL 是否指向同一资源。
/// @note 忽略会轮换的签名查询参数，但保留 id/seq 等稳定参数；协议和 host
///       比较时统一小写。用于识别多个 BYTERANGE 分片是否属于同一整段文件。
bool sameUrlIgnoringQuery(const QUrl &a, const QUrl &b)
{
    if (!a.isValid() || !b.isValid())
        return false;
    // 只忽略会轮换的签名参数，保留 id/seq 等稳定查询参数；整段忽略 query
    // 会把 ?id=1 与 ?id=2 当成同一资源，错误启用“单文件整段下载”。
    QUrl left = a;
    QUrl right = b;
    left.setScheme(left.scheme().toLower());
    left.setHost(left.host().toLower());
    right.setScheme(right.scheme().toLower());
    right.setHost(right.host().toLower());
    return identityUrlWithoutVolatileQuery(left) == identityUrlWithoutVolatileQuery(right);
}

} // namespace HlsCacheManifest
