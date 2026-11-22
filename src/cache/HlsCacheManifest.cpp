#include "cache/HlsCacheManifest.h"

#include <QRegularExpression>
#include <QStringList>

namespace {

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

QString uriAttribute(const QString &line)
{
    const QRegularExpressionMatch match = kUri.match(line);
    if (!match.hasMatch())
        return QString();
    return match.captured(1).isEmpty() ? match.captured(2).trimmed()
                                       : match.captured(1).trimmed();
}

QString capture(const QRegularExpression &re, const QString &line)
{
    const QRegularExpressionMatch match = re.match(line);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

int effectivePort(const QUrl &url)
{
    if (url.port() >= 0)
        return url.port();
    return url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
}

} // namespace

namespace HlsCacheManifest {

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
        return true;
    }
    return true;
}

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
    out->singleFile = true;
    const QUrl first = segmentUrls.first();
    out->singleFileUrl = first;
    for (int i = 0; i < segmentUrls.size(); ++i) {
        if (!segmentRanges.at(i) || !sameUrlIgnoringQuery(segmentUrls.at(i), first)) {
            out->singleFile = false;
            out->singleFileUrl = QUrl();
            break;
        }
    }
    return true;
}

bool sameUrlIgnoringQuery(const QUrl &a, const QUrl &b)
{
    if (!a.isValid() || !b.isValid())
        return false;
    return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0
        && a.host().compare(b.host(), Qt::CaseInsensitive) == 0
        && effectivePort(a) == effectivePort(b)
        && a.path() == b.path();
}

} // namespace HlsCacheManifest
