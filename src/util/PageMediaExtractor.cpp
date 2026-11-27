#include "util/PageMediaExtractor.h"

#include "util/HtmlText.h"
#include "util/MediaExtensions.h"
#include "util/NetworkAddressPolicy.h"

#include <QRegularExpression>
#include <QStringList>

namespace {

/// 网页里的播放地址常写成 &#x2F; 这种数字实体，或 JSON 里的 \/
/// 转义；不还原会让 QUrl 解析出坏路径。
QString decodeCandidate(const QString &raw)
{
    QString candidate = HtmlText::decodeEntities(raw, HtmlTextPolicy{1, false, true, true});
    candidate.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    candidate.replace(QStringLiteral("\\u0026"), QStringLiteral("&"));
    candidate.replace(QStringLiteral("\\x26"), QStringLiteral("&"));
    return candidate.trimmed();
}

} // namespace

QUrl PageMediaExtractor::extractMediaUrl(const QString &html, const QUrl &base)
{
    if (html.isEmpty())
        return QUrl();

    // 只在地址里找带媒体后缀的字符串；页面里还有大量普通 http 链接，
    // 不能把导航栏、广告位或封面图当成播放源。
    static const QStringList patterns = {
        // OpenGraph 视频声明。
        QStringLiteral("property=[\"']og:video(?::url)?[\"'][^>]*content=[\"']([^\"']+)[\"']"),
        QStringLiteral("content=[\"']([^\"']+)[\"'][^>]*property=[\"']og:video(?::url)?[\"']"),
        // 原生 video/source 标签。
        QStringLiteral("<video[^>]+src=[\"']([^\"']+)[\"']"),
        QStringLiteral("<source[^>]+src=[\"']([^\"']+)[\"']"),
        // DPlayer 一类播放器常写成 const vid = '...m3u8'。
        QStringLiteral("(?:const|let|var)\\s+[\\w$]+\\s*=\\s*[\"']"
                       "([^\"']+\\.(?:m3u8|mp4|flv|webm|mpd|ts|mkv|mov|m4v)[^\"']*)[\"']"),
        // 播放器配置对象：url: '...' / "playUrl": "..."。
        QStringLiteral("[\"']?(?:playUrl|videoUrl|url|src|file|source|stream|vid)[\"']?"
                       "\\s*[:=]\\s*[\"']"
                       "([^\"']+\\.(?:m3u8|mp4|flv|webm|mpd|ts|mkv|mov|m4v)[^\"']*)[\"']"),
        // 兜底：任何带媒体后缀的绝对地址。
        QStringLiteral("[\"']((?:https?:)?//[^\"']+\\.(?:m3u8|mp4|flv|webm|mpd|ts|mkv|mov|m4v)[^\"']*)[\"']"),
        // 兜底：站内相对地址。
        QStringLiteral("[\"']([^\"']+\\.(?:m3u8|mp4|flv|webm|mpd|ts|mkv|mov|m4v)[^\"']*)[\"']"),
    };

    for (const QString &pattern : patterns) {
        const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(html);
        if (!match.hasMatch())
            continue;

        const QString candidate = decodeCandidate(match.captured(1));
        if (candidate.isEmpty() || candidate.size() > 2048)
            continue;
        const QUrl resolved = base.resolved(QUrl(candidate));
        // 只接受 http/https 且带明确媒体后缀：og:video 有时给的是播放页，
        // 把它再当成媒体地址会让播放器继续打开 HTML。
        if (NetworkAddressPolicy::isHttpMediaUrl(resolved)
            && MediaExtensions::isDirectLinkSuffix(MediaExtensions::suffixFromUrl(resolved))) {
            return resolved;
        }
    }
    return QUrl();
}
