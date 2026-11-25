/// @file SourceProbe.cpp
/// @brief 搜索源探查器：抓首页候选入口、按规则识别接口/网页型源，并试搜验真。
#include "net/SourceProbe.h"

#include "net/ConfigSearchProvider.h"
#include "net/HttpClient.h"
#include "net/MaccmsWebProvider.h"
#include "net/NetLimits.h"
#include "util/NetworkAddressPolicy.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

Q_LOGGING_CATEGORY(lcProbe, "mvp.net.probe")

namespace {

constexpr int kBudgetMs = 24000;        // 整轮探测的总时限
constexpr int kBudgetMsMusic = 32000;   // 音乐接口更多，且多数快速 404
constexpr int kBudgetMsAuto = 40000;    // 自动识别（两种类型都试）的时限
constexpr int kRequestTimeoutMs = 6000; // 单次请求超时
constexpr int kWebValidationTimeoutMs = 8000; // 网页型影视只做轻量试搜
// 内置路径影视 10 条、音乐 16 条，首页还可能抓到 /from/xxx/ 等非标准接口。
// 上限必须给首页线索留出余量，否则后面的内置路径根本没机会被尝试。
constexpr int kMaxAttempts = 16;
// 自动识别要覆盖影视 10 + 音乐 16 条，不能再用 20 次把后 6 条截掉。
constexpr int kMaxAttemptsAuto = 30;
// 候选列表里的网页型试搜标记；不是真实 URL，只用来在候选顺序中占位。
constexpr const char *kWebFallbackMarker = "__MVP_MACCM_WEB__";

QString renderUrl(const QString &tpl, const QString &keyword, int limit)
{
    QString text = tpl;
    text.replace(QStringLiteral("{keyword}"),
                 QString::fromUtf8(QUrl::toPercentEncoding(keyword)));
    text.replace(QStringLiteral("{limit}"), QString::number(limit));
    return text;
}

/// 只认苹果 CMS/影视站模板的强特征，避免把普通 HTML 站点误配成网页源。
bool hasMaccmsHomeSignals(const QString &html, const QUrl &url)
{
    if (html.trimmed().isEmpty())
        return false;

    const QString lower = html.toLower();
    if (lower.contains(QStringLiteral("maccms"))
        || lower.contains(QStringLiteral("苹果cms"))
        || lower.contains(QStringLiteral("苹果 cms"))
        || lower.contains(QStringLiteral("mac cms")))
        return true;

    // 搜索/详情路径足够具体：普通资讯站几乎不会同时出现这两个路径。
    if ((lower.contains(QStringLiteral("vodsearch"))
         || lower.contains(QStringLiteral("voddetail")))
        && lower.contains(QStringLiteral("vod")))
        return true;

    if (lower.contains(QStringLiteral("/index.php/vod/"))
        || lower.contains(QStringLiteral("ajax/suggest"))
        || lower.contains(QStringLiteral("index.php/ajax/suggest")))
        return true;

    const QString path = url.path().toLower();
    if (path.contains(QStringLiteral("/vodsearch"))
        || path.contains(QStringLiteral("/voddetail")))
        return true;

    return false;
}

/// 把首页里抓到的接口地址补成“能带关键词搜索”的模板。
/// 影视沿用 ac/wd；音乐按接口族补 query/q/search/name/searchTerm 等参数。
QString appendProbeQuery(QString location, MediaType type)
{
    location = location.trimmed();
    if (location.isEmpty())
        return {};
    location.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    const int fragment = location.indexOf(QLatin1Char('#'));
    if (fragment >= 0)
        location.truncate(fragment);

    const bool music = (type == MediaType::Music);
    const QString lower = location.toLower();
    auto add = [&location](const QString &pair) {
        if (location.endsWith(QLatin1Char('?')) || location.endsWith(QLatin1Char('&')))
            location += pair;
        else if (location.contains(QLatin1Char('?')))
            location += QLatin1Char('&') + pair;
        else
            location += QLatin1Char('?') + pair;
    };
    auto has = [&location](const QString &key) {
        return location.contains(key + QLatin1Char('='), Qt::CaseInsensitive);
    };

    if (location.contains(QLatin1String("{keyword}"))) {
        if (!music)
            return location;
    } else {
        // 用户可能直接粘贴了一条带示例关键词的搜索地址。把固定值换成占位符，
        // 比丢掉这条地址更有用；音乐接口的关键词参数名很多，不能只认 wd。
        const QStringList names = {
            QStringLiteral("wd"), QStringLiteral("q"), QStringLiteral("query"),
            QStringLiteral("searchTerm"), QStringLiteral("search"),
            QStringLiteral("term"), QStringLiteral("name"), QStringLiteral("filter"),
            QStringLiteral("keyword"), QStringLiteral("w"), QStringLiteral("s")
        };
        QRegularExpression re(
            QStringLiteral("([?&])(") + names.join(QLatin1Char('|')) + QStringLiteral(")=[^&]*"),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(location).hasMatch())
            location.replace(re, QStringLiteral("\\1\\2={keyword}"));

        // Radio Browser 的 byname/bytag 把关键词放在路径里，不替换就只能丢掉。
        if (music) {
            QRegularExpression pathRe(
                QStringLiteral("(/json/stations/(?:byname|bytag)/)([^/?#]+)"),
                QRegularExpression::CaseInsensitiveOption);
            location.replace(pathRe, QStringLiteral("\\1{keyword}"));
        }
    }

    if (!music) {
        if (location.contains(QLatin1String("{keyword}")))
            return location;
        if (location.contains(QLatin1String("wd="), Qt::CaseInsensitive)) {
            QRegularExpression wdRe(QStringLiteral("([?&])wd=[^&]*"),
                                    QRegularExpression::CaseInsensitiveOption);
            location.replace(wdRe, QStringLiteral("\\1wd={keyword}"));
            return location;
        }
        if (location.contains(QLatin1String("/index.php/ajax/data"))) {
            if (!has(QStringLiteral("mid")))
                add(QStringLiteral("mid=1"));
            if (!has(QStringLiteral("limit")))
                add(QStringLiteral("limit={limit}"));
            add(QStringLiteral("wd={keyword}"));
            return location;
        }
        if (location.contains(QLatin1String("ac="), Qt::CaseInsensitive)) {
            add(QStringLiteral("wd={keyword}"));
            return location;
        }
        add(QStringLiteral("ac=detail&wd={keyword}"));
        return location;
    }

    // 音乐：先按接口族补齐各自的关键词/分页参数；已经带 {keyword} 的路径也要补，
    // 否则 Subsonic 的 /rest/search3 会缺 f=json，只能返回 XML。
    if (lower.contains(QLatin1String("/rest/search"))) {
        if (!has(QStringLiteral("query")))
            add(QStringLiteral("query={keyword}"));
        if (!has(QStringLiteral("songCount")))
            add(QStringLiteral("songCount={limit}"));
        QRegularExpression fRe(QStringLiteral("([?&])f=[^&]*"),
                               QRegularExpression::CaseInsensitiveOption);
        if (fRe.match(location).hasMatch())
            location.replace(fRe, QStringLiteral("\\1f=json"));
        else
            add(QStringLiteral("f=json"));
        return location;
    }
    if (lower.contains(QLatin1String("/json/stations/search"))) {
        if (!has(QStringLiteral("name")))
            add(QStringLiteral("name={keyword}"));
        if (!has(QStringLiteral("limit")))
            add(QStringLiteral("limit={limit}"));
        if (!has(QStringLiteral("hidebroken")))
            add(QStringLiteral("hidebroken=true"));
        return location;
    }
    if (lower.contains(QLatin1String("/json/stations/byname/"))) {
        if (!has(QStringLiteral("limit")))
            add(QStringLiteral("limit={limit}"));
        return location;
    }
    if (lower.contains(QLatin1String("/api/query"))) {
        if (!has(QStringLiteral("f")))
            add(QStringLiteral("f=json"));
        if (!has(QStringLiteral("search")))
            add(QStringLiteral("search={keyword}"));
        if (!has(QStringLiteral("limit")))
            add(QStringLiteral("limit={limit}"));
        return location;
    }
    if (lower.contains(QLatin1String("/api/v1/tracks"))
        || lower.contains(QLatin1String("/api/v1/albums"))
        || lower.contains(QLatin1String("/api/v1/artists"))) {
        if (!has(QStringLiteral("q")))
            add(QStringLiteral("q={keyword}"));
        if (!has(QStringLiteral("page_size")))
            add(QStringLiteral("page_size={limit}"));
        return location;
    }
    if (lower.contains(QLatin1String("/api/v2/albums"))) {
        if (!has(QStringLiteral("q")))
            add(QStringLiteral("q={keyword}"));
        return location;
    }
    if (lower.contains(QLatin1String("/items"))) {
        if (!has(QStringLiteral("searchTerm")))
            add(QStringLiteral("searchTerm={keyword}"));
        if (!has(QStringLiteral("IncludeItemTypes")))
            add(QStringLiteral("IncludeItemTypes=Audio"));
        if (!has(QStringLiteral("Limit")))
            add(QStringLiteral("Limit={limit}"));
        return location;
    }
    if (lower.contains(QLatin1String("/wp-json/wp/v2/search"))) {
        if (!has(QStringLiteral("search")))
            add(QStringLiteral("search={keyword}"));
        if (!has(QStringLiteral("per_page")))
            add(QStringLiteral("per_page={limit}"));
        return location;
    }
    if (lower.contains(QLatin1String("/ajax/search"))) {
        if (!has(QStringLiteral("keyword")))
            add(QStringLiteral("keyword={keyword}"));
        if (!has(QStringLiteral("limit")))
            add(QStringLiteral("limit={limit}"));
        return location;
    }
    // 通用 /search 与 /api/search：优先 q；如果已经有 term/search/query/keyword
    // 就不要再塞 q，避免 iTunes 这类接口同时收到 term 和 q。
    if (lower.contains(QLatin1String("/search"))) {
        const bool hasKeyword = has(QStringLiteral("q")) || has(QStringLiteral("query"))
                                || has(QStringLiteral("searchTerm")) || has(QStringLiteral("search"))
                                || has(QStringLiteral("term")) || has(QStringLiteral("name"))
                                || has(QStringLiteral("keyword")) || has(QStringLiteral("w"))
                                || has(QStringLiteral("s"));
        if (!hasKeyword)
            add(QStringLiteral("q={keyword}"));
        if (!has(QStringLiteral("limit")))
            add(QStringLiteral("limit={limit}"));
        return location;
    }

    add(QStringLiteral("q={keyword}&limit={limit}"));
    return location;
}

/// Subsonic/Navidrome 的搜索结果通常只有歌曲 id，没有直链。
/// 把 /rest/search3?u=...&p=... 改写成 /rest/stream?id={id}&u=...，
/// 交给 ConfigMapper 的 @ 模板在运行时替换 {id}。
QString restStreamTemplate(const QString &searchUrl)
{
    const QUrl url(searchUrl);
    if (!url.isValid())
        return {};
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (!path.contains(QLatin1String("/rest/search"), Qt::CaseInsensitive))
        return {};
    const QString originalPath = path;
    path.replace(QRegularExpression(QStringLiteral("/rest/search(?:2|3)?$"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral("/rest/stream"));
    if (path == originalPath)
        return {};

    QString origin = url.scheme() + QStringLiteral("://") + url.host();
    if (url.port() > 0)
        origin += QLatin1Char(':') + QString::number(url.port());

    QStringList params;
    params << QStringLiteral("id={id}");
    const QUrlQuery query(url);
    for (const QString &key : {QStringLiteral("u"), QStringLiteral("p"),
                               QStringLiteral("t"), QStringLiteral("s"),
                               QStringLiteral("v"), QStringLiteral("c")}) {
        if (query.hasQueryItem(key)) {
            params << key + QLatin1Char('=')
                          + QString::fromUtf8(QUrl::toPercentEncoding(query.queryItemValue(key)));
        }
    }
    return origin + path + QLatin1Char('?') + params.join(QLatin1Char('&'));
}

bool looksLikeApiPath(const QString &path)
{
    const QString lower = path.toLower();
    return lower.contains(QLatin1String("provide/vod"))
           || lower.contains(QLatin1String("/inc/api"))
           || lower.contains(QLatin1String("ajax/data"))
           || lower.contains(QLatin1String("api.php"))
           || lower.contains(QLatin1String("/rest/"))
           || lower.contains(QLatin1String("/wp-json/"))
           || lower.contains(QLatin1String("/json/stations/"))
           || lower.contains(QLatin1String("/api/v1/"))
           || lower.contains(QLatin1String("/api/v2/"))
           || lower.contains(QLatin1String("/api/query"))
           || lower.contains(QLatin1String("/api/search"))
           || lower.contains(QLatin1String("/items"))
           || lower.contains(QLatin1String("/search/hints"))
           || lower.contains(QLatin1String("/search"));
}

/// 首页 HTML/JS 里经常直接写着采集接口，先把它抓出来，
/// 比盲试内置路径更可靠，也能覆盖 /inc/api.php、/from/xxx/ 这类非标准路径。
QStringList extractHomeApiHints(const QString &html)
{
    QString normalized = html;
    normalized.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    QRegularExpression re(
        QStringLiteral("(/api\\.php/provide/vod(?:/from/[A-Za-z0-9_]+)?"
                       "(?:/at/(?:json|xml))?/?"
                       "|/inc/api(?:_mac10|_mac_m3u8)?\\.php"
                       "|/index\\.php/ajax/data"
                       "|/rest/search(?:2|3)?"
                       "|/api/v(?:1|2)/(?:tracks|albums|artists)"
                       "|/json/stations/search"
                       "|/wp-json/wp/v2/search"
                       "|/api/query"
                       "|/api/search"
                       "|/Items)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList hints;
    QRegularExpressionMatchIterator it = re.globalMatch(normalized);
    while (it.hasNext() && hints.size() < 10) {
        const QString path = it.next().captured(1);
        if (!hints.contains(path))
            hints.append(path);
    }
    return hints;
}

/// 二次关键词校验：两次返回完全同一组标题，说明接口忽略了 wd，
/// 只能列目录、不能按关键词搜索，不能当作可搜索源保存。
bool sameResultTitles(const QVector<MediaItem> &first, const QVector<MediaItem> &second)
{
    if (first.size() != second.size() || first.isEmpty())
        return false;
    // 必须按“标题出现次数”比较：只比较集合会把 A,A,B 与 A,B,B 误判成同一组，
    // 结果是顺序/重复变化被当成固定列表，或者反过来漏判。
    QHash<QString, int> counts;
    counts.reserve(second.size());
    for (const MediaItem &item : second)
        ++counts[item.title.trimmed()];
    for (const MediaItem &item : first) {
        const QString title = item.title.trimmed();
        auto it = counts.find(title);
        if (it == counts.end() || it.value() <= 0)
            return false;
        --it.value();
    }
    for (int count : counts) {
        if (count != 0)
            return false;
    }
    return true;
}

QUrl originOf(const QUrl &url)
{
    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(url.host());
    origin.setPort(url.port());
    return origin;
}

struct FieldSample
{
    QString key;
    QStringList values;
};

void addFieldSample(QVector<FieldSample> *samples, QHash<QString, int> *index,
                    const QString &key, const QJsonValue &value)
{
    if (samples->size() >= 80)
        return;
    int slot = index->value(key, -1);
    if (slot < 0) {
        FieldSample sample;
        sample.key = key;
        samples->append(sample);
        slot = samples->size() - 1;
        index->insert(key, slot);
    }
    if (value.isString()) {
        const QString text = value.toString();
        if (!text.isEmpty())
            samples->operator[](slot).values.append(text);
    } else if (value.isDouble()) {
        samples->operator[](slot).values.append(QString::number(value.toDouble()));
    }
}

void collectObjectSamples(const QJsonObject &object, const QString &prefix, int depth,
                          QVector<FieldSample> *samples, QHash<QString, int> *index)
{
    if (depth > 3 || samples->size() >= 80)
        return;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString key = prefix.isEmpty() ? it.key() : prefix + QLatin1Char('.') + it.key();
        const QJsonValue value = it.value();
        if (value.isString() || value.isDouble()) {
            addFieldSample(samples, index, key, value);
        } else if (value.isObject()) {
            collectObjectSamples(value.toObject(), key, depth + 1, samples, index);
        } else if (value.isArray()) {
            const QJsonArray array = value.toArray();
            for (int i = 0; i < qMin(array.size(), 2); ++i) {
                const QJsonValue child = array.at(i);
                const QString childKey = key + QLatin1Char('[') + QString::number(i) + QLatin1Char(']');
                if (child.isObject())
                    collectObjectSamples(child.toObject(), childKey, depth + 1, samples, index);
                else if (child.isString() && !child.toString().isEmpty())
                    addFieldSample(samples, index, childKey, child);
            }
        }
    }
}

/// 取前几条对象的字段样本，用来猜哪个字段是标题、播放地址……
/// 音乐接口的标题/直链经常在嵌套对象里（stream.url、files[0].download_url、
/// album.name、artists[0].name），只收顶层字段会漏掉，因此这里按路径展开。
QVector<FieldSample> collectSamples(const QJsonArray &items)
{
    QVector<FieldSample> samples;
    QHash<QString, int> index;
    const int take = qMin(items.size(), 5);
    for (int i = 0; i < take; ++i) {
        if (!items.at(i).isObject())
            continue;
        collectObjectSamples(items.at(i).toObject(), QString(), 0, &samples, &index);
    }
    return samples;
}

bool looksNumeric(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return false;
    bool ok = false;
    trimmed.toDouble(&ok);
    return ok;
}

/// requireText=true 时跳过“值全是数字”的字段，
/// 避免把 artistId / albumId 这类 id 当成作者或标题。
QString pickKey(const QVector<FieldSample> &samples, const QStringList &needles,
                const QSet<QString> &exclude = {}, bool requireText = false)
{
    auto usable = [&](const FieldSample &sample) {
        if (exclude.contains(sample.key))
            return false;
        if (!requireText)
            return true;
        if (sample.values.isEmpty())
            return false;
        for (const QString &value : sample.values) {
            if (!looksNumeric(value))
                return true;
        }
        return false;
    };

    // 先按“完全等于”匹配，再退回“包含”
    for (const QString &needle : needles) {
        for (const FieldSample &sample : samples) {
            if (!usable(sample))
                continue;
            if (sample.key.compare(needle, Qt::CaseInsensitive) == 0)
                return sample.key;
        }
    }
    for (const QString &needle : needles) {
        for (const FieldSample &sample : samples) {
            if (!usable(sample))
                continue;
            if (sample.key.toLower().contains(needle))
                return sample.key;
        }
    }
    return QString();
}

bool looksPlayable(const QString &value)
{
    const QString lower = value.toLower();
    return lower.contains(QLatin1String(".m3u8")) || lower.contains(QLatin1String(".mp4"))
           || lower.contains(QLatin1String(".mp3")) || lower.contains(QLatin1String(".m4a"))
           || lower.contains(QLatin1String(".flac")) || lower.contains(QLatin1String(".aac"))
           || lower.contains(QLatin1String(".ogg")) || lower.contains(QLatin1String(".opus"))
           || lower.contains(QLatin1String(".wma")) || lower.contains(QLatin1String(".aiff"));
}

/// 音乐接口的直链字段经常没有 .mp3 后缀（电台 url_resolved、Audius stream.url、
/// Subsonic 的 stream 端点），所以还要按字段名判断；但绝不能把文件名当直链。
bool looksPlayableMusic(const QString &value, const QString &fieldKey)
{
    const QString text = value.trimmed();
    const QString lower = text.toLower();
    if (!lower.startsWith(QLatin1String("http")) && !lower.startsWith(QLatin1String("//")))
        return false;

    const QString key = fieldKey.toLower();
    if (key.contains(QLatin1String("filename")) || key.contains(QLatin1String("file_name"))
        || key.contains(QLatin1String("upload_name")) || key.contains(QLatin1String("page_url"))
        || key.contains(QLatin1String("weburl")) || key.contains(QLatin1String("permalink")))
        return false;

    if (looksPlayable(text))
        return true;
    return key.contains(QLatin1String("url_resolved"))
           || key.contains(QLatin1String("stream.url"))
           || key.contains(QLatin1String("stream_url"))
           || key.contains(QLatin1String("previewurl"))
           || key.contains(QLatin1String("preview_url"))
           || key.contains(QLatin1String("download_url"))
           || key.contains(QLatin1String("listen_url"))
           || key.contains(QLatin1String("playurl"))
           || key.contains(QLatin1String("play_url"))
           || lower.contains(QLatin1String("/listen/"))
           || lower.contains(QLatin1String("/stream/"));
}

struct StreamPick
{
    QString key;
    QString kind;   // direct / episodes
};

/// 播放地址字段：优先认带 ".m3u8/.mp3/..." 或 "名称$地址#..." 形态的字段，
/// 音乐还要认 url_resolved、stream.url、download_url 这类没有后缀的直链字段，
/// 这样不会把详情页链接或 orig_filename 误当成播放地址。
StreamPick pickStream(const QVector<FieldSample> &samples, MediaType type)
{
    StreamPick best;
    int bestScore = 0;
    for (const FieldSample &sample : samples) {
        const QString keyLower = sample.key.toLower();
        if (keyLower.contains(QLatin1String("filename"))
            || keyLower.contains(QLatin1String("file_name"))
            || keyLower.contains(QLatin1String("upload_name"))
            || keyLower.contains(QLatin1String("page_url")))
            continue;
        int score = 0;
        QString kind;
        QStringList sampleValues = sample.values.mid(0, 3);
        for (const QString &value : sampleValues) {
            const QString text = value.trimmed();
            if (text.contains(QLatin1String("$$$"))
                || (text.contains(QLatin1Char('#')) && text.contains(QLatin1String("http")))
                || text.contains(QLatin1String("$http"))) {
                score = qMax(score, 3);
                kind = QStringLiteral("episodes");
            } else if (type == MediaType::Music) {
                // 音乐不能回退到“只要值里含 .mp3 就算直链”：Subsonic 的 path
                // 是本地文件路径（Artist/Album/01.mp3），不是可请求的播放地址。
                if (looksPlayableMusic(text, sample.key)) {
                    score = qMax(score, 2);
                    // 同一站点同时有 url 和 url_resolved 时优先取 resolved 版本；
                    // 电台的 url 可能是 http 跳转地址，url_resolved 才是最终流。
                    if (keyLower.contains(QLatin1String("url_resolved"))
                        || keyLower.contains(QLatin1String("stream.url"))
                        || keyLower.contains(QLatin1String("stream_url"))
                        || keyLower.contains(QLatin1String("previewurl"))
                        || keyLower.contains(QLatin1String("preview_url"))
                        || keyLower.contains(QLatin1String("download_url"))
                        || keyLower.contains(QLatin1String("listen_url"))
                        || keyLower.contains(QLatin1String("playurl"))
                        || keyLower.contains(QLatin1String("play_url"))) {
                        score = qMax(score, 3);
                    }
                    if (kind.isEmpty())
                        kind = QStringLiteral("direct");
                }
            } else if (looksPlayable(text)) {
                score = qMax(score, 2);
                if (kind.isEmpty())
                    kind = QStringLiteral("direct");
            }
        }
        // 同分时优先字段名里带 play 的，避免抓到 "picUrl" 之类
        if (score > bestScore
            || (score == bestScore && score > 0
                && keyLower.contains(QLatin1String("play")))) {
            bestScore = score;
            best.key = sample.key;
            best.kind = kind;
        }
    }
    return best;
}

/// 从返回数据反推这到底是影视站还是音乐站：
/// 分集列表 / m3u8 / mp4 / vod_* 字段算影视，音频后缀 / 歌手类字段算音乐。
/// 两边都没线索时，用候选路径自带的类型兜底。
MediaType detectType(const QVector<FieldSample> &samples, const StreamPick &stream,
                     MediaType fallback)
{
    int videoScore = 0;
    int musicScore = 0;

    if (stream.kind == QLatin1String("episodes"))
        videoScore += 5;

    for (const FieldSample &sample : samples) {
        const QString key = sample.key.toLower();
        if (key.startsWith(QLatin1String("vod_")) || key.contains(QLatin1String("episode"))
            || key.contains(QLatin1String("season")) || key.contains(QLatin1String("movie"))) {
            videoScore += 2;
        }
        if (key.contains(QLatin1String("artist")) || key.contains(QLatin1String("track"))
            || key.contains(QLatin1String("album")) || key.contains(QLatin1String("song"))
            || key.contains(QLatin1String("previewurl")) || key.contains(QLatin1String("url_resolved"))
            || key.contains(QLatin1String("listen_url")) || key.contains(QLatin1String("stream.url"))
            || key.contains(QLatin1String("upload_name"))) {
            musicScore += 2;
        }

        const QStringList values = sample.values.mid(0, 3);
        for (const QString &value : values) {
            const QString text = value.toLower();
            // .m3u8 既可能是视频切片，也可能是电台/音乐的 HLS 流。
            // 用户已经明确选音乐时，不能仅凭 .m3u8 把电台源改判成影视。
            const bool hlsOnly = text.contains(QLatin1String(".m3u8"));
            if (text.contains(QLatin1String(".mp4")) || text.contains(QLatin1String(".flv"))
                || text.contains(QLatin1String(".mkv"))
                || (hlsOnly && fallback != MediaType::Music)) {
                videoScore += 3;
            }
            if (text.contains(QLatin1String(".mp3")) || text.contains(QLatin1String(".m4a"))
                || text.contains(QLatin1String(".flac")) || text.contains(QLatin1String(".aac"))
                || text.contains(QLatin1String(".ogg")) || text.contains(QLatin1String(".wav"))
                || text.contains(QLatin1String(".opus"))) {
                musicScore += 3;
            }
        }
    }

    if (videoScore > musicScore)
        return MediaType::Video;
    if (musicScore > videoScore)
        return MediaType::Music;
    return fallback;
}

/// 数组能否当“条目列表”的候选：首元素必须是对象，且至少有一个标量字段。
/// 只有嵌套列表的对象（如 `[{"list":[...]}]`）属于包装层，不能当成结果数组。
int itemFieldCount(const QJsonArray &array)
{
    if (array.isEmpty() || !array.first().isObject())
        return 0;
    int count = 0;
    const QJsonObject first = array.first().toObject();
    for (auto it = first.constBegin(); it != first.constEnd(); ++it) {
        if (!it.value().isArray() && !it.value().isObject())
            ++count;
    }
    return count;
}

/// 包装层数组的特征：首元素里的标量字段只有 total/count/code 这类分页或状态
/// 元数据，真正的条目在它内部的数组里。结果项也可能带一个很大的 episodes/
/// tracks 数组，但结果项还有 title/vod_name 等业务字段，不能继续下钻，否则
/// 会把 10 条结果识别成第 1 条结果里的 50 个播放片段。
bool isWrapperArray(const QJsonArray &array)
{
    if (array.isEmpty() || !array.first().isObject())
        return false;
    static const QSet<QString> metadataKeys = {
        QStringLiteral("total"), QStringLiteral("count"), QStringLiteral("page"),
        QStringLiteral("pages"), QStringLiteral("pagecount"), QStringLiteral("size"),
        QStringLiteral("limit"), QStringLiteral("offset"), QStringLiteral("code"),
        QStringLiteral("status"), QStringLiteral("success"), QStringLiteral("message"),
        QStringLiteral("msg"), QStringLiteral("hasmore"), QStringLiteral("more"),
        QStringLiteral("totalcount"), QStringLiteral("totalpage"),
    };
    const QJsonObject first = array.first().toObject();
    bool hasChildArray = false;
    for (auto it = first.constBegin(); it != first.constEnd(); ++it) {
        if (it.value().isArray()) {
            hasChildArray = true;
            continue;
        }
        if (it.value().isObject())
            continue;
        if (!metadataKeys.contains(it.key().toLower()))
            return false;
    }
    return hasChildArray;
}

/// 在 JSON 里找最像“条目列表”的数组，并生成带数组索引的路径（如 `[0].list`）。
/// 根为 `[{"list":[...]}]` 这类包装层时继续向下扫描，避免只把外层 1 条当结果；
/// 索引路径可直接交给 ConfigMapper::valueAtPath。
void scanArrays(const QJsonValue &node, const QString &path, int depth, int *bestScore,
                QJsonArray *best, QString *bestPath, int *budget)
{
    if (depth > 6 || budget == nullptr || *budget <= 0)
        return;
    --*budget;
    if (node.isArray()) {
        const QJsonArray array = node.toArray();
        const int fields = itemFieldCount(array);
        // 条目数优先；条目数相同时字段更丰富的一层更像真实结果列表。
        const int score = fields > 0 ? array.size() * 32 + qMin(fields, 31) : -1;
        if (score > *bestScore) {
            *bestScore = score;
            *best = array;
            *bestPath = path;
        }
        if (!isWrapperArray(array))
            return;
        // 包装层通常只有 1 条对象；仍限制每层扫描数量，避免异常数据放大耗时。
        const int scanCount = qMin(array.size(), 16);
        for (int i = 0; i < scanCount; ++i) {
            const QString child = path + QLatin1Char('[') + QString::number(i)
                                  + QLatin1Char(']');
            scanArrays(array.at(i), child, depth + 1, bestScore, best, bestPath, budget);
        }
        return;
    }
    if (node.isObject()) {
        const QJsonObject object = node.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString child = path.isEmpty() ? it.key() : path + QLatin1Char('.') + it.key();
            scanArrays(it.value(), child, depth + 1, bestScore, best, bestPath, budget);
        }
    }
}

/// Subsonic/Navidrome 认证失败时 HTTP 仍是 200，只在正文里给
/// {"subsonic-response":{"status":"failed","error":{"code":40}}}。
/// 不单独识别就会误报成“JSON 没有结果数组”。
bool subsonicAuthFailure(const QJsonValue &root)
{
    if (!root.isObject())
        return false;
    const QJsonValue sub = root.toObject().value(QStringLiteral("subsonic-response"));
    if (!sub.isObject())
        return false;
    return sub.toObject().value(QStringLiteral("status")).toString()
               .compare(QLatin1String("failed"), Qt::CaseInsensitive) == 0;
}

} // namespace

namespace SourceProbe {

QStringList defaultPaths(MediaType mediaType)
{
    if (mediaType == MediaType::Video) {
        // 基于 186 个真实影视节点验证：MacCMS 的 ac 取值、接口文件位置、
        // 是否带尾斜杠都可能不同；所有模板都必须带 {keyword}，否则会
        // 把“只能列目录”的接口误判成可搜索源。
        return {QStringLiteral("/api.php/provide/vod/?ac=detail&wd={keyword}"),
                QStringLiteral("/api.php/provide/vod/?ac=videolist&wd={keyword}"),
                QStringLiteral("/api.php/provide/vod/?ac=list&wd={keyword}"),
                QStringLiteral("/api.php/provide/vod/at/json/?ac=detail&wd={keyword}"),
                QStringLiteral("/index.php/ajax/data?mid=1&wd={keyword}&limit={limit}"),
                QStringLiteral("/inc/api.php?ac=detail&wd={keyword}"),
                QStringLiteral("/inc/api_mac10.php?ac=detail&wd={keyword}"),
                QStringLiteral("/inc/api_mac_m3u8.php?ac=detail&wd={keyword}"),
                QStringLiteral("/api.php/provide/vod?ac=detail&wd={keyword}"),
                QStringLiteral("/api.php/provide/vod/?wd={keyword}")};
    }
    // 音乐：基于 150 个真实节点/10 轮验证，真正可搜索且有直链的只有
    // iTunes、Audius、ccMixter、Radio Browser 这几族；Subsonic/Navidrome
    // 能搜索但只返回 id，需要额外生成 /rest/stream 模板；Funkwhale、
    // Jellyfin、Ampache 匿名接口普遍 401/404，仍保留路径供带凭据的用户使用。
    // 每条都必须带 {keyword}，否则“能列目录”会被误判成“能搜索”。
    return {QStringLiteral("/search?term={keyword}&media=music&limit={limit}"),
            QStringLiteral("/rest/search3?query={keyword}&songCount={limit}&f=json"),
            QStringLiteral("/rest/search2?query={keyword}&songCount={limit}&f=json"),
            QStringLiteral("/rest/search?query={keyword}&songCount={limit}&f=json"),
            QStringLiteral("/api/v1/tracks/?q={keyword}&page_size={limit}"),
            QStringLiteral("/api/v1/albums/?q={keyword}&page_size={limit}"),
            QStringLiteral("/api/v1/artists/?q={keyword}&page_size={limit}"),
            QStringLiteral("/api/v2/albums?q={keyword}"),
            QStringLiteral("/json/stations/search?name={keyword}&limit={limit}"),
            QStringLiteral("/json/stations/byname/{keyword}?limit={limit}"),
            QStringLiteral("/api/query?f=json&search={keyword}&limit={limit}"),
            QStringLiteral("/Items?searchTerm={keyword}&IncludeItemTypes=Audio&Limit={limit}"),
            QStringLiteral("/wp-json/wp/v2/search?search={keyword}&per_page={limit}"),
            QStringLiteral("/api/search?q={keyword}&limit={limit}"),
            QStringLiteral("/search?q={keyword}&limit={limit}"),
            QStringLiteral("/ajax/search?keyword={keyword}&limit={limit}")};
}

/// @brief 构造探查器，探查过程复用调用方的 HTTP 客户端。
Prober::Prober(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

/// @brief 释放探查器并中止在途请求。
Prober::~Prober()
{
    m_finished = true;
    if (m_activeReply != nullptr)
        m_activeReply->abort();
    if (m_validator != nullptr) {
        m_validator->abort();
        m_validator->deleteLater();
        m_validator = nullptr;
    }
}

/// @brief 开始探查指定站点。
/// @param site 站点入口。
/// @param hintType 期望的媒体类型（未知时由探查结果推断）。
/// @param extraPaths 额外的候选路径。
/// @note 全程受请求预算约束，超预算按失败收尾，避免单站点拖死界面。
void Prober::start(const QUrl &site, MediaType hintType, const QStringList &extraPaths,
                   bool autoDetect)
{
    // 新的一轮探测必须换一个代次：上一轮首页请求可能还在路上，
    // 不能让它复用同一个 Prober 时把迟到响应写进本轮结果。
    ++m_runGeneration;
    m_finished = false;
    m_type = hintType;
    m_auto = autoDetect;
    m_extraPaths = extraPaths;
    m_result = Result();
    m_result.mediaType = hintType;
    m_result.hintType = hintType;
    m_candidateIndex = 0;
    m_candidates.clear();
    m_candidateTypes.clear();
    m_siteName.clear();
    m_inputPath.clear();
    m_homeApiHints.clear();
    // 重定向域名是上一站的兜底地址；复用同一个 Prober 时必须清空，
    // 否则会把上一站的接口再拼到新站上试一遍。
    m_redirectOrigin = QUrl();
    m_webFallbackPossible = false;
    m_webFallbackTried = false;
    if (m_webValidator != nullptr) {
        m_webValidator->abort();
        m_webValidator->deleteLater();
        m_webValidator = nullptr;
    }
    m_validationFirstItems.clear();
    m_validationPasses = 0;
    m_validationNote.clear();
    // 失败归类计数必须每次探测清零，否则同一个 Prober 复用会把上一站的失败累积过来
    m_homeOk = false;
    m_connFail = 0;
    m_denied = 0;
    m_unauthorized = 0;
    m_authRequired = 0;
    m_notFound = 0;
    m_notJson = 0;
    m_xml = 0;
    m_jsonNoItems = 0;
    m_jsonNoFields = 0;
    m_clock.start();

    m_site = site;
    if (!NetworkAddressPolicy::isHttpMediaUrl(m_site)) {
        fail(tr("网址无效，必须以 http:// 或 https:// 开头"));
        return;
    }
    if (m_site.path().isEmpty())
        m_site.setPath(QStringLiteral("/"));
    m_inputPath = m_site.path();
    m_origin = originOf(m_site);
    fetchHome();
}

/// @brief 取消探查并释放在途请求。
void Prober::cancel()
{
    // 让首页回调的代次立即失效；首页请求无法在这里单独 abort（HttpClient
    // 由多个源共享），但迟到回调只做 deleteLater，不再访问本对象。
    ++m_runGeneration;
    m_finished = true;
    if (m_activeReply != nullptr)
        m_activeReply->abort();
    // 候选校验用的 ConfigSearchProvider 是独立对象，cancel() 不能只停
    // 当前探测请求；必须同时 abort 它，否则旧 validator 会继续跑到超时，
    // 还可能在取消后回调到已经复用的 Prober。
    if (m_validator != nullptr) {
        m_validator->abort();
        m_validator->deleteLater();
        m_validator = nullptr;
    }
    if (m_webValidator != nullptr) {
        m_webValidator->abort();
        m_webValidator->deleteLater();
        m_webValidator = nullptr;
    }
}

/// @brief 是否还有剩余请求预算。
bool Prober::budgetLeft() const
{
    // 自动识别要试两种类型的候选，音乐接口也比影视多，给的时间略多
    const int budget = m_auto ? kBudgetMsAuto : (m_type == MediaType::Music ? kBudgetMsMusic : kBudgetMs);
    return m_clock.elapsed() < budget;
}

/// @brief 记录一次候选请求结果，供最终失败诊断使用。
void Prober::recordAttempt(const QString &url, int status, const QString &note)
{
    Attempt attempt;
    attempt.url = url;
    attempt.status = status;
    attempt.note = note;
    m_result.attempts.append(attempt);
    qCInfo(lcProbe) << "试探" << status << note
                    << HttpClient::redactUrl(QUrl(url));
}

/// @brief 抓取首页并从链接/脚本中收集候选搜索入口。
void Prober::fetchHome()
{
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), m_origin.toString() + QLatin1Char('/'));

    // 首页请求的完成回调可能晚于 Prober/探测对话框的析构；这里用 QPointer
    // 作为生命周期护栏，并带上本轮代次，迟到响应只释放 reply，不再碰本对象。
    const int generation = m_runGeneration;
    QPointer<Prober> self(this);
    // 首页只用来拿站点标题和真实域名，打不开也照样去试接口
    m_http->fetch(m_site, QByteArray(), headers, [this, self, generation](QNetworkReply *reply) {
        if (self.isNull()) {
            if (reply != nullptr)
                reply->deleteLater();
            return;
        }
        if (m_finished || generation != m_runGeneration) {
            if (reply != nullptr)
                reply->deleteLater();
            return;
        }
        if (reply != nullptr) {
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QUrl finalUrl = reply->url();
            // 首页跳转只作为“备用地址”：很多站点首页会跳到营销站（itunes.apple.com
            // 就跳到 www.apple.com），接口其实还在用户输入的那个域名上。
            if (finalUrl.isValid() && !finalUrl.host().isEmpty()) {
                m_site = finalUrl;
                m_redirectOrigin = originOf(finalUrl);
            }
            bool tooLarge = false;
            const QByteArray raw =
                HttpClient::readAllLimited(reply, NetLimits::kHtmlBytes, &tooLarge);
            if (HttpClient::isHttpSuccess(reply) && !tooLarge) {
                m_homeOk = true;
                const QString html = QString::fromUtf8(raw);
                const QRegularExpression titleRe(QStringLiteral("(?is)<title[^>]*>(.*?)</title>"));
                const QRegularExpressionMatch match = titleRe.match(html);
                if (match.hasMatch()) {
                    m_siteName = match.captured(1).simplified();
                    m_siteName.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
                }
                // 首页直接写出的接口路径优先于盲试内置路径；
                // 这是覆盖 /inc/api.php、/from/xxx/、非标准端口的关键。
                m_homeApiHints = extractHomeApiHints(html);
                if (m_type == MediaType::Video || m_auto)
                    m_webFallbackPossible = hasMaccmsHomeSignals(html, m_site);
            } else {
                // 首页失败也要归类，否则用户只看到一句笼统的“没有找到接口”
                if (status == 0)
                    ++m_connFail;
                else if (status == 401 || status == 403)
                    ++m_denied;
                else if (status == 404)
                    ++m_notFound;
                recordAttempt(m_site.toString(), status, tr("首页打不开"));
            }
            reply->deleteLater();
        }

        // 候选路径：用户明确给的接口 > 首页里抓到的接口 > 用户自定义路径 > 内置路径。
        // 首页线索里 /at/xml/ 不能解析，必须排在 JSON 线索之后，
        // 否则真实站点会先浪费一次请求（apibdzy 首页同时暴露 xml 和 json）。
        QStringList jsonHints;
        QStringList xmlHints;
        for (const QString &hint : m_homeApiHints) {
            if (hint.contains(QLatin1String("/at/xml"), Qt::CaseInsensitive))
                xmlHints.append(hint);
            else
                jsonHints.append(hint);
        }
        m_homeApiHints = jsonHints + xmlHints;
        const QString userPath = m_inputPath;
        const MediaType otherType =
            m_type == MediaType::Video ? MediaType::Music : MediaType::Video;
        QStringList samePaths;
        QStringList otherPaths;
        QVector<MediaType> sameTypes;
        QVector<MediaType> otherTypes;
        QSet<QString> seenPaths;
        auto addPath = [&](QString path, MediaType type, bool other) {
            path = appendProbeQuery(path, type);
            if (path.isEmpty() || !path.contains(QLatin1String("{keyword}")))
                return; // 没有关键词占位符的地址不能当搜索源
            if (seenPaths.contains(path))
                return;
            seenPaths.insert(path);
            if (other) {
                otherPaths.append(path);
                otherTypes.append(type);
            } else {
                samePaths.append(path);
                sameTypes.append(type);
            }
        };

        if (userPath.size() > 1 && looksLikeApiPath(userPath))
            addPath(userPath, m_type, false);

        for (const QString &hint : m_homeApiHints)
            addPath(hint, m_type, false);

        for (const QString &extra : m_extraPaths) {
            const QString trimmed = extra.trimmed();
            if (trimmed.isEmpty())
                continue;
            if (trimmed.startsWith(QLatin1String("http")))
                addPath(trimmed, m_type, false);
            else
                addPath(trimmed.startsWith(QLatin1Char('/')) ? trimmed
                                                             : QLatin1Char('/') + trimmed,
                        m_type, false);
        }

        for (const QString &path : defaultPaths(m_type))
            addPath(path, m_type, false);
        if (m_auto) {
            for (const QString &path : defaultPaths(otherType))
                addPath(path, otherType, true);
        }

        m_candidates.clear();
        m_candidateTypes.clear();
        QSet<QString> seenCandidates;
        auto appendCandidate = [&](const QString &url, MediaType type) {
            if (seenCandidates.contains(url))
                return;
            seenCandidates.insert(url);
            m_candidates.append(url);
            m_candidateTypes.append(type);
        };

        // 完整 URL 只试一次；相对路径先走用户输入的域名，再走首页跳转域名。
        // 先试当前类型的全部候选，再决定是否转网页型，最后才试另一种类型。
        // 否则影视网页站会把 16 次音乐路径都浪费掉，甚至在 20 次上限前触不到
        // 网页型 fallback。
        const bool hasRedirect = m_redirectOrigin.isValid() && !m_redirectOrigin.host().isEmpty()
                              && m_redirectOrigin != m_origin;
        const QVector<QUrl> origins = hasRedirect ? QVector<QUrl>{m_origin, m_redirectOrigin}
                                                  : QVector<QUrl>{m_origin};
        auto appendForOrigin = [&](const QStringList &paths,
                                   const QVector<MediaType> &types,
                                   const QUrl &origin) {
            for (int i = 0; i < paths.size(); ++i) {
                const QString path = paths.at(i);
                if (path.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
                    appendCandidate(path, types.value(i, m_type));
                    continue;
                }
                const QString relative = path.startsWith(QLatin1Char('/'))
                                             ? path
                                             : QLatin1Char('/') + path;
                appendCandidate(origin.toString() + relative, types.value(i, m_type));
            }
        };

        for (const QUrl &origin : origins)
            appendForOrigin(samePaths, sameTypes, origin);

        // 同类型 JSON 候选都失败后再给网页型影视一次机会；标记不是真实 URL。
        if (m_webFallbackPossible)
            appendCandidate(QString::fromLatin1(kWebFallbackMarker), MediaType::Video);

        if (m_auto) {
            for (const QUrl &origin : origins)
                appendForOrigin(otherPaths, otherTypes, origin);
        }

        probeNextCandidate();
    }, NetLimits::kHtmlBytes, m_origin);
}

/// @brief 按顺序探查下一个候选入口，命中则进入试搜验证。
void Prober::probeNextCandidate()
{
    if (m_finished)
        return;
    const int baseAttempts = m_auto ? kMaxAttemptsAuto : kMaxAttempts;
    // 首页线索、用户自定义路径、用户直接粘贴的接口都是额外候选，
    // 不能占用内置路径的预算，否则最后几条默认路径会被截断。
    const int userPathBudget =
        (m_inputPath.size() > 1 && looksLikeApiPath(m_inputPath)) ? 1 : 0;
    const int maxAttempts = baseAttempts + m_extraPaths.size() + m_homeApiHints.size()
                            + userPathBudget;
    if (m_candidateIndex >= m_candidates.size() || m_candidateIndex >= maxAttempts) {
        // 候选末尾还没走到网页型标记时，也要给网页型一次机会。
        if (m_webFallbackPossible && !m_webFallbackTried) {
            tryWebFallback();
            return;
        }
        fail(diagnoseFailure());
        return;
    }
    if (!budgetLeft()) {
        if (m_webFallbackPossible && !m_webFallbackTried) {
            tryWebFallback();
            return;
        }
        fail(tr("探测超时：%1 个候选里没有一个在时限内返回").arg(m_candidateIndex));
        return;
    }

    const int candidateIndex = m_candidateIndex++;
    const QString tpl = m_candidates.at(candidateIndex);
    if (tpl == QString::fromLatin1(kWebFallbackMarker)) {
        tryWebFallback();
        return;
    }
    const MediaType candidateType = m_candidateTypes.value(candidateIndex, m_type);
    // 音乐真实验证用的是 love/jazz，而不是中文“音乐/歌曲”：
    // 国际音乐 API（iTunes/Audius/ccMixter/Radio Browser）对英文基准词更稳定，
    // 中文曲库（网易云/QQ/酷狗）也普遍能返回 love 的结果。
    const QString keyword = candidateType == MediaType::Video ? tr("电影") : QStringLiteral("love");
    const QString probeUrl = renderUrl(tpl, keyword, 10);

    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), m_origin.toString() + QLatin1Char('/'));

    QNetworkReply *reply =
        m_http->get(QUrl(probeUrl), QByteArray(), headers, NetLimits::kJsonBytes, m_origin);
    if (reply == nullptr) {
        recordAttempt(probeUrl, 0, tr("请求未发出"));
        probeNextCandidate();
        return;
    }
    m_activeReply = reply;

    QTimer::singleShot(kRequestTimeoutMs, this, [this, reply] {
        if (m_activeReply == reply) {
            m_activeReply = nullptr;
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, probeUrl, tpl, keyword, candidateType] {
        if (m_finished) {
            reply->deleteLater();
            return;
        }
        if (m_activeReply == reply)
            m_activeReply = nullptr;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool tooLarge = false;
        const QByteArray payload =
            HttpClient::readAllLimited(reply, NetLimits::kJsonBytes, &tooLarge);
        const bool success = HttpClient::isHttpSuccess(reply);
        const QString errorText = success ? QString() : HttpClient::describeError(reply);
        reply->deleteLater();

        if (!success) {
            if (status == 0)
                ++m_connFail;
            else if (status == 401 || status == 403)
                ++m_denied;
            else if (status == 404)
                ++m_notFound;
            recordAttempt(probeUrl, status, errorText.isEmpty() ? tr("HTTP 失败") : errorText);
            probeNextCandidate();
            return;
        }

        if (tooLarge) {
            // 超限只对 2xx 正文成立；404/500 的大错误页已在上面按 HTTP 状态
            // 记录，不能把真实的 404/403 覆盖成“响应过大”。
            recordAttempt(probeUrl, status, tr("响应超过 2 MB 上限"));
            probeNextCandidate();
            return;
        }

        // 先按正文形态归类，避免把所有非 JSON 响应都笼统说成“返回网页”。
        const QString prefix = QString::fromUtf8(payload.left(512)).trimmed();
        const QString lowerPrefix = prefix.toLower();
        if (prefix.contains(QStringLiteral("未授权"))
            || prefix.contains(QStringLiteral("已到期"))
            || prefix.contains(QStringLiteral("授权已到期"))) {
            ++m_unauthorized;
            recordAttempt(probeUrl, status, tr("接口提示 IP 未授权/已到期"));
            probeNextCandidate();
            return;
        }
        const bool looksXml = lowerPrefix.startsWith(QLatin1String("<?xml"))
                           || lowerPrefix.startsWith(QLatin1String("<rss"))
                           || lowerPrefix.startsWith(QLatin1String("<list"))
                           || (lowerPrefix.startsWith(QLatin1Char('<'))
                               && !lowerPrefix.startsWith(QLatin1String("<html"))
                               && !lowerPrefix.startsWith(QLatin1String("<!doctype")));
        if (looksXml) {
            ++m_xml;
            recordAttempt(probeUrl, status, tr("返回 XML 接口（当前只解析 JSON）"));
            probeNextCandidate();
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            ++m_notJson;
            recordAttempt(probeUrl, status, tr("返回的不是 JSON"));
            probeNextCandidate();
            return;
        }

        const QJsonValue root =
            document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());

        if (subsonicAuthFailure(root)) {
            ++m_authRequired;
            recordAttempt(probeUrl, status, tr("Subsonic/Navidrome 返回认证失败"));
            probeNextCandidate();
            return;
        }

        QJsonArray items;
        QString itemsPath;
        int itemsScore = -1;
        int nodeBudget = 4096;
        scanArrays(root, QString(), 0, &itemsScore, &items, &itemsPath, &nodeBudget);
        if (items.isEmpty()) {
            ++m_jsonNoItems;
            recordAttempt(probeUrl, status, tr("JSON 里没有结果数组"));
            probeNextCandidate();
            return;
        }

        const QVector<FieldSample> samples = collectSamples(items);
        const QString titleKey =
            pickKey(samples, {QStringLiteral("vod_name"), QStringLiteral("trackname"),
                              QStringLiteral("track_name"), QStringLiteral("upload_name"),
                              QStringLiteral("songname"), QStringLiteral("song_name"),
                              QStringLiteral("song"), QStringLiteral("title"),
                              QStringLiteral("name")}, {}, true);
        const StreamPick stream = pickStream(samples, candidateType);

        // Subsonic/Navidrome 搜索结果只有歌曲 id，没有直链；
        // 这里生成 @https://host/rest/stream?id={id}&u=... 模板，
        // 由 ConfigMapper 在运行时把 {id} 换成实际歌曲 id。
        // 必须只对 searchResult*.song 生效：searchResult*.album 的 id 是专辑 id，
        // 拿来拼 /rest/stream 会得到一个不能播放的地址。
        QString subsonicIdKey;
        QString subsonicStreamSpec;
        if (stream.key.isEmpty() && candidateType == MediaType::Music
            && itemsPath.contains(QLatin1String("searchResult"), Qt::CaseInsensitive)
            && itemsPath.contains(QLatin1String("song"), Qt::CaseInsensitive)) {
            subsonicIdKey = pickKey(samples, {QStringLiteral("id"), QStringLiteral("songid")});
            const QString streamTemplate = restStreamTemplate(tpl);
            if (!subsonicIdKey.isEmpty() && !streamTemplate.isEmpty())
                subsonicStreamSpec = QStringLiteral("@") + streamTemplate;
        }

        if (titleKey.isEmpty() || (stream.key.isEmpty() && subsonicStreamSpec.isEmpty())) {
            ++m_jsonNoFields;
            recordAttempt(probeUrl, status,
                          titleKey.isEmpty() ? tr("没有标题字段") : tr("没有播放地址字段"));
            probeNextCandidate();
            return;
        }

        // 组装建议配置
        const MediaType detected = detectType(samples, stream, candidateType);
        SourceConfig config;
        // 同一个 host 上可能挂多个接口（例如 /api.php 与 /index.php），
        // 只用 host 生成 id 会互相覆盖；把站点路径一起算进 8 位哈希。
        const QByteArray identity =
            (m_origin.toString() + m_site.path()).toUtf8();
        const QString suffix = QString::fromLatin1(
            QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex().left(8));
        config.id = QStringLiteral("site_")
                    + m_origin.host().replace(QLatin1Char('.'), QLatin1Char('_'))
                    + QLatin1Char('_') + suffix;
        config.name = m_siteName.isEmpty() ? m_origin.host() : m_siteName;
        config.enabled = true;
        config.kind = QStringLiteral("config");
        config.method = QStringLiteral("GET");
        config.mediaType = detected;
        config.urlTemplate = tpl;
        config.itemsPath = itemsPath;
        config.streamKind = stream.kind.isEmpty() ? QStringLiteral("direct") : stream.kind;
        config.headers.insert(QStringLiteral("Referer"),
                              m_origin.toString() + QLatin1Char('/'));
        config.fields.insert(QStringLiteral("title"), titleKey);
        config.fields.insert(QStringLiteral("streamUrl"),
                             subsonicStreamSpec.isEmpty() ? stream.key : subsonicStreamSpec);

        QSet<QString> used{titleKey};
        if (!stream.key.isEmpty())
            used.insert(stream.key);
        else if (!subsonicIdKey.isEmpty())
            used.insert(subsonicIdKey);

        const QString artistKey =
            pickKey(samples, {QStringLiteral("artistname"), QStringLiteral("artist"),
                              QStringLiteral("artist.name"), QStringLiteral("uploader"),
                              QStringLiteral("singer"), QStringLiteral("author"),
                              QStringLiteral("vod_actor"), QStringLiteral("performer")}, used, true);
        if (!artistKey.isEmpty()) {
            config.fields.insert(QStringLiteral("artist"), artistKey);
            used.insert(artistKey);
        }

        const QString posterKey =
            pickKey(samples, {QStringLiteral("vod_pic"), QStringLiteral("pic"),
                              QStringLiteral("cover"), QStringLiteral("cover_xl"),
                              QStringLiteral("album.cover"), QStringLiteral("artworkurl100"),
                              QStringLiteral("artwork_url"), QStringLiteral("image_url"),
                              QStringLiteral("poster"), QStringLiteral("thumb"),
                              QStringLiteral("thumbnail"), QStringLiteral("image")}, used);
        if (!posterKey.isEmpty()) {
            config.fields.insert(QStringLiteral("posterUrl"), posterKey);
            used.insert(posterKey);
        }

        const QString yearKey =
            pickKey(samples, {QStringLiteral("vod_year"), QStringLiteral("year")}, used);
        if (!yearKey.isEmpty()) {
            config.fields.insert(QStringLiteral("year"), yearKey);
            used.insert(yearKey);
        }

        const QString areaKey =
            pickKey(samples, {QStringLiteral("vod_area"), QStringLiteral("area"),
                              QStringLiteral("region"), QStringLiteral("country")}, used);
        if (!areaKey.isEmpty()) {
            config.fields.insert(QStringLiteral("area"), areaKey);
            used.insert(areaKey);
        }

        const QString genreKey =
            pickKey(samples, {QStringLiteral("vod_class"), QStringLiteral("class"),
                              QStringLiteral("cate"), QStringLiteral("category"),
                              QStringLiteral("genre"), QStringLiteral("type")}, used);
        if (!genreKey.isEmpty()) {
            config.fields.insert(QStringLiteral("genre"), genreKey);
            used.insert(genreKey);
        }

        const QString descKey =
            pickKey(samples, {QStringLiteral("vod_content"), QStringLiteral("content"),
                              QStringLiteral("intro"), QStringLiteral("desc"),
                              QStringLiteral("description"), QStringLiteral("summary")}, used);
        if (!descKey.isEmpty())
            config.fields.insert(QStringLiteral("description"), descKey);

        const QString webKey =
            pickKey(samples, {QStringLiteral("detail"), QStringLiteral("weburl"),
                              QStringLiteral("web_url"), QStringLiteral("pageurl"),
                              QStringLiteral("page_url"), QStringLiteral("permalink"),
                              QStringLiteral("file_page_url"), QStringLiteral("link")}, used);
        if (!webKey.isEmpty())
            config.fields.insert(QStringLiteral("webUrl"), webKey);

        recordAttempt(probeUrl, status,
                      subsonicStreamSpec.isEmpty()
                          ? tr("接口可用，正在试搜校验")
                          : tr("接口可用，已把歌曲 id 映射为 /rest/stream，正在试搜校验"));
        validate(config, keyword);
    });
}

/// @brief 用示例关键词试搜，确认生成的源能真正返回条目。
void Prober::validate(const SourceConfig &config, const QString &keyword)
{
    if (!config.urlTemplate.contains(QLatin1String("{keyword}"))) {
        recordAttempt(config.urlTemplate, 0, tr("接口地址不含关键词占位符，不能作为搜索源"));
        probeNextCandidate();
        return;
    }
    m_validationFirstItems.clear();
    startValidation(config, keyword, 1);
}

/// @brief 第 pass 轮验证：失败则按诊断结果调整字段后再试。
void Prober::startValidation(const SourceConfig &config, const QString &keyword, int pass)
{
    qCInfo(lcProbe) << "校验配置" << config.id << config.name << "第" << pass << "轮";

    ConfigSearchProvider *provider = new ConfigSearchProvider(config, m_http, this);
    m_validator = provider;
    // 捕获本次 provider 的 QPointer：回调里只能清理自己，不能把同一轮
    // 已经启动的新 validator 当成旧对象 deleteLater。
    QPointer<ConfigSearchProvider> guard(provider);
    connect(provider, &SearchProviderBase::finished, this,
            [this, config, pass, guard](const QString &sourceId, const QVector<MediaItem> &items,
                                        int filtered, const QString &error) {
                Q_UNUSED(sourceId)
                Q_UNUSED(filtered)
                if (m_finished || guard.isNull() || m_validator.data() != guard.data())
                    return;
                if (!guard.isNull())
                    guard->deleteLater();
                m_validator = nullptr;

                if (pass == 1) {
                    if (!error.isEmpty()) {
                        recordAttempt(config.urlTemplate, 0, tr("试搜失败：%1").arg(error));
                        probeNextCandidate();
                        return;
                    }
                    if (items.isEmpty()) {
                        recordAttempt(config.urlTemplate, 0, tr("试搜没有结果"));
                        probeNextCandidate();
                        return;
                    }
                    // 第一次用“电影”搜到结果，再用另一个常见词确认接口真的处理 wd。
                    // 很多站点的 ac=detail 会忽略 wd 并返回固定最新列表，只做一次
                    // 试搜会把这种“只能列目录”的接口误存成可搜索源。
                    m_validationFirstItems = items;
                    const QString secondKeyword =
                        config.mediaType == MediaType::Video ? tr("电视剧") : QStringLiteral("jazz");
                    startValidation(config, secondKeyword, 2);
                    return;
                }

                if (pass == 2) {
                    if (error.isEmpty() && !items.isEmpty()
                        && sameResultTitles(m_validationFirstItems, items)) {
                        recordAttempt(config.urlTemplate, 0,
                                      tr("接口忽略关键词，只返回固定列表，不能作为搜索源"));
                        m_validationFirstItems.clear();
                        probeNextCandidate();
                        return;
                    }
                    if (error.isEmpty() && items.isEmpty()) {
                        // 第二次恰好没有结果并不等于接口不能用：影视站的片库可能
                        // 没有“电视剧”，音乐 API 也可能没有 jazz。再补一个高频词，
                        // 三轮都空才降级为“仅首轮命中”，并明确提示用户。
                        const QString thirdKeyword =
                            config.mediaType == MediaType::Video ? tr("庆余年")
                                                                  : QStringLiteral("hello");
                        startValidation(config, thirdKeyword, 3);
                        return;
                    }
                    if (!error.isEmpty()) {
                        m_validationPasses = 1;
                        m_validationNote = tr("第二关键词请求失败，按首轮结果接受");
                    } else {
                        m_validationPasses = 2;
                        m_validationNote = tr("两轮关键词校验通过");
                    }
                    acceptConfig(config, items.isEmpty() ? m_validationFirstItems : items);
                    return;
                }

                // 第三轮：有结果就再确认一次“不是固定列表”；仍为空则按首轮接受，
                // 但 validationPasses 保持 1，让界面知道只验证了一个关键词。
                if (error.isEmpty() && !items.isEmpty()
                    && sameResultTitles(m_validationFirstItems, items)) {
                    recordAttempt(config.urlTemplate, 0,
                                  tr("接口忽略关键词，只返回固定列表，不能作为搜索源"));
                    m_validationFirstItems.clear();
                    probeNextCandidate();
                    return;
                }
                if (error.isEmpty() && !items.isEmpty()) {
                    m_validationPasses = 3;
                    m_validationNote = tr("三轮关键词校验通过");
                    acceptConfig(config, items);
                    return;
                }
                m_validationPasses = 1;
                m_validationNote = tr("第三关键词仍无结果，仅首轮命中，建议手工确认");
                qCInfo(lcProbe) << config.id << "第三关键词没有命中，按首轮结果接受";
                acceptConfig(config, m_validationFirstItems);
            });
    provider->search(keyword, 10);
}

/// @brief 接口型探查全部失败时，回退尝试网页型影视源。
void Prober::tryWebFallback()
{
    if (m_finished || m_webFallbackTried)
        return;
    m_webFallbackTried = true;

    if (!m_webFallbackPossible || (!m_auto && m_type != MediaType::Video)) {
        probeNextCandidate();
        return;
    }

    QUrl origin = m_origin;
    if (!origin.isValid() || origin.host().isEmpty())
        origin = originOf(m_site);
    if (!origin.isValid() || origin.host().isEmpty()) {
        recordAttempt(m_site.toString(), 0, tr("网页型影视源没有可用站点域名"));
        probeNextCandidate();
        return;
    }

    SourceConfig config;
    QString hostPart = origin.host().toLower();
    hostPart.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    if (hostPart.isEmpty())
        hostPart = QStringLiteral("site");
    const QString suffix = QString::fromLatin1(
        QCryptographicHash::hash(origin.toString().toUtf8(), QCryptographicHash::Sha1)
            .toHex()
            .left(6));
    config.id = QStringLiteral("web_") + hostPart + QLatin1Char('_') + suffix;
    config.name = m_siteName.isEmpty() ? origin.host() : m_siteName;
    if (!config.name.contains(QStringLiteral("网页")))
        config.name += tr("（网页源）");
    config.enabled = true;
    config.builtin = false; // 用户添加的网页源，允许删除
    config.kind = QStringLiteral("builtin");
    config.provider = QStringLiteral("maccms_web");
    config.mediaType = MediaType::Video;
    config.urlTemplate = origin.toString() + QLatin1Char('/');
    config.headers.insert(QStringLiteral("Referer"), m_origin.toString() + QLatin1Char('/'));

    if (!config.isValid()) {
        recordAttempt(config.urlTemplate, 0, tr("网页型影视源配置不完整"));
        probeNextCandidate();
        return;
    }

    // 只验搜索页是否真的按关键词返回影片链接，不抓详情/播放页；
    // 完整播放链交给保存后的“测试”按钮，避免探测阶段长时间卡住。
    MaccmsWebProvider *provider = new MaccmsWebProvider(config, m_http, this);
    m_webValidator = provider;
    provider->setValidationMode(true);
    QPointer<MaccmsWebProvider> guard(provider);

    QTimer::singleShot(kWebValidationTimeoutMs, this, [this, guard] {
        if (m_finished || guard.isNull() || m_webValidator != guard)
            return;
        m_webValidator->abort();
        m_webValidator->deleteLater();
        m_webValidator = nullptr;
        recordAttempt(m_site.toString(), 0, tr("网页型影视试搜超时"));
        probeNextCandidate();
    });

    connect(provider, &SearchProviderBase::finished, this,
            [this, config, guard](const QString &sourceId, const QVector<MediaItem> &items,
                                  int filtered, const QString &error) {
                Q_UNUSED(sourceId)
                Q_UNUSED(filtered)
                if (m_finished)
                    return;
                if (m_webValidator != guard)
                    return;
                if (!guard.isNull())
                    guard->deleteLater();
                m_webValidator = nullptr;

                if (error.isEmpty() && !items.isEmpty()) {
                    acceptWebConfig(config, items);
                    return;
                }
                recordAttempt(config.urlTemplate, 0,
                              error.isEmpty() ? tr("网页型影视试搜没有结果")
                                              : tr("网页型影视试搜失败：%1").arg(error));
                probeNextCandidate();
            });

    provider->search(tr("电影"), 3);
}

/// @brief 接受网页型源并携带试搜样本返回。
void Prober::acceptWebConfig(const SourceConfig &config, const QVector<MediaItem> &items)
{
    Result result = m_result;
    result.ok = true;
    result.siteKind = SiteKind::MaccmsWeb;
    result.mediaType = MediaType::Video;
    result.autoDetected = m_result.hintType != MediaType::Video;
    result.siteName = config.name;
    result.siteUrl = m_site;
    result.config = config;
    result.sampleCount = items.size();
    result.validationPasses = 1;
    result.validationNote = tr("网页型影视源试搜命中 %1 条").arg(items.size());
    const int take = qMin(items.size(), 5);
    for (int i = 0; i < take; ++i) {
        QString line = items.at(i).displayName();
        const QString meta = items.at(i).metaLine();
        if (!meta.isEmpty())
            line += QStringLiteral("  ·  ") + meta;
        result.sampleTitles.append(line);
    }
    done(result);
}

/// @brief 接受接口型源并返回验证通过的样本。
void Prober::acceptConfig(const SourceConfig &config, const QVector<MediaItem> &items)
{
    Result result = m_result;
    result.ok = true;
    result.siteKind = SiteKind::JsonConfig;
    result.mediaType = config.mediaType;
    result.autoDetected = config.mediaType != m_result.hintType;
    result.siteName = config.name;
    result.siteUrl = m_site;
    result.config = config;
    result.sampleCount = items.size();
    result.validationPasses = m_validationPasses;
    result.validationNote = m_validationNote;
    const int take = qMin(items.size(), 5);
    for (int i = 0; i < take; ++i) {
        QString line = items.at(i).displayName();
        const QString meta = items.at(i).metaLine();
        if (!meta.isEmpty())
            line += QStringLiteral("  ·  ") + meta;
        result.sampleTitles.append(line);
    }
    m_validationFirstItems.clear();
    done(result);
}

/// @brief 汇总失败原因，优先给出最能指导用户修改的提示。
QString Prober::diagnoseFailure() const
{
    const bool music = (m_type == MediaType::Music);
    if (m_denied > 0)
        return music
            ? tr("站点拒绝程序访问（HTTP 401/403，命中 %1 次）：音乐服务多为 Subsonic/Navidrome/Funkwhale/Jellyfin，需要用户名密码或 API key，不是路径问题")
                  .arg(m_denied)
            : tr("站点拒绝程序访问（HTTP 401/403，命中 %1 次）：拦的是访问方式，不是接口路径，换地址也没用，只能用浏览器打开")
                  .arg(m_denied);
    if (m_authRequired > 0)
        return tr("音乐接口返回认证失败（%1 次）：Subsonic/Navidrome 需要 u/p 或 token，Jellyfin/Ampache 需要 API key；公开 demo 凭据不会写进内置配置，请填带凭据的接口地址")
            .arg(m_authRequired);
    if (m_unauthorized > 0)
        return music
            ? tr("音乐接口提示未授权/已到期（%1 次）：需要有效的 API key 或账号，换公开 demo 地址通常无效")
                  .arg(m_unauthorized)
            : tr("接口返回“IP 未授权/已到期”（%1 次）：这不是路径问题，需要在资源站后台授权当前 IP 或更换有授权的域名")
                  .arg(m_unauthorized);
    if (m_xml > 0)
        return music
            ? tr("音乐接口返回 XML（%1 次）：Subsonic 默认 XML，请在地址后加 f=json；当前探查器只解析 JSON，XML 需要外部解析器")
                  .arg(m_xml)
            : tr("接口返回 XML（%1 次）：当前探查器只解析 JSON；如果站点同时提供 /at/json/，请改用 JSON 路径，否则需要手动配置外部解析器")
                  .arg(m_xml);
    if (m_connFail > 0 && m_notFound == 0 && m_notJson == 0)
        return tr("候选接口连接失败（%1 次）：%2；本机到该域名的连接被断开或超时，不是路径 404")
            .arg(m_connFail)
            .arg(m_homeOk ? tr("首页能打开，但接口请求连接失败")
                          : tr("首页也打不开，域名或网络不可达"));
    if (m_jsonNoItems > 0 && m_notFound == 0 && m_notJson == 0)
        return music
            ? tr("音乐接口返回 JSON 但没有结果数组（%1 次）：Subsonic 可能是认证失败（status=failed），或关键词参数不是 query/q/name/searchTerm")
                  .arg(m_jsonNoItems)
            : tr("接口返回 JSON 但没有结果数组（%1 次）：可能只支持分类/列表参数，或关键词参数名不是 wd")
                  .arg(m_jsonNoItems);
    if (m_jsonNoFields > 0 && m_notFound == 0)
        return music
            ? tr("音乐接口有结果数组但猜不出可播放字段（%1 次）：如果只有 id（Subsonic/Navidrome），应生成 /rest/stream?id={id} 模板；Jellyfin 用 /Audio/{id}/stream；只有网页链接的不算音频源")
                  .arg(m_jsonNoFields)
            : tr("接口有结果数组但猜不出标题/播放地址字段（%1 次）：该站点字段命名不标准，需要手动配置字段映射")
                  .arg(m_jsonNoFields);
    if (m_webFallbackPossible || m_webFallbackTried)
        return tr("已识别为网页型影视站，但网页搜索试搜没有命中：站点搜索页可能改版、关闭搜索或需要登录；"
                  "可以先保存为网页型影视源，再在源管理里点“测试”确认");
    if (m_notJson > 0 && m_notFound == 0)
        return music
            ? tr("地址能访问但返回网页/播放器（%1 次），不是音乐 JSON 接口：请直接填 API 地址，例如 /rest/search3?f=json、/api/v1/tracks/?q=、/json/stations/search?name=")
                  .arg(m_notJson)
            : tr("地址能访问但返回的是网页（%1 次），不是 JSON 接口：请直接填接口地址，例如 https://域名/api.php/provide/vod/")
                  .arg(m_notJson);
    if (m_homeOk) {
        if (m_connFail > 0) {
            return tr("首页能打开，但候选接口里有 %1 次连接失败、%2 次 404：先确认接口域名/端口可用，再检查路径")
                .arg(m_connFail)
                .arg(m_notFound);
        }
        return music
            ? tr("首页能打开，但 %1 条候选路径全部 404：这个站可能只有网页/播放器，没有公开音乐 API，需要它的接口地址（如 /rest/search3、/api/v1/tracks）")
                  .arg(m_notFound)
            : tr("首页能打开，但 %1 条候选路径全部 404：这个站很可能只有网页、没有采集接口，需要它的接口地址（形如 https://域名/api.php/provide/vod/）")
                  .arg(m_notFound);
    }
    return music
        ? tr("没有找到可用的音乐接口地址：这个网站可能不是标准 JSON API，需要手动配置，或先填带认证的 Subsonic/Jellyfin 地址")
        : tr("没有找到可用的接口地址，这个网站可能不是标准 JSON 接口，需要手动配置");
}

/// @brief 探查失败收尾：中止在途请求并发出带诊断的失败结果。
void Prober::fail(const QString &error)
{
    Result result = m_result;
    result.ok = false;
    result.mediaType = m_result.hintType;
    result.siteName = m_siteName;
    result.siteUrl = m_site;
    result.error = error;
    if (m_denied > 0 || m_authRequired > 0)
        result.siteKind = SiteKind::AuthRequired;
    else if (m_xml > 0)
        result.siteKind = SiteKind::XmlOnly;
    else if (m_webFallbackPossible || m_webFallbackTried)
        result.siteKind = SiteKind::WebOnlyNoApi;
    else
        result.siteKind = SiteKind::Unknown;
    done(result);
}

/// @brief 探查结束：只发出一次结果并延迟删除自身。
void Prober::done(const Result &result)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_validator != nullptr) {
        m_validator->deleteLater();
        m_validator = nullptr;
    }
    if (m_webValidator != nullptr) {
        m_webValidator->deleteLater();
        m_webValidator = nullptr;
    }
    emit finished(result);
}

} // namespace SourceProbe
