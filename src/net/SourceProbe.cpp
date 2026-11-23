#include "net/SourceProbe.h"

#include "net/ConfigSearchProvider.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"

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
constexpr int kBudgetMsAuto = 32000;    // 自动识别（两种类型都试）的时限
constexpr int kRequestTimeoutMs = 6000; // 单次请求超时
// 内置路径已有 10 条，首页还可能抓到 /from/xxx/ 等非标准接口。
// 上限必须给首页线索留出余量，否则后面的内置路径根本没机会被尝试。
constexpr int kMaxAttempts = 16;

QString renderUrl(const QString &tpl, const QString &keyword, int limit)
{
    QString text = tpl;
    text.replace(QStringLiteral("{keyword}"),
                 QString::fromUtf8(QUrl::toPercentEncoding(keyword)));
    text.replace(QStringLiteral("{limit}"), QString::number(limit));
    return text;
}

/// 把首页里抓到的接口地址补成“能带关键词搜索”的模板。
/// 已经含 {keyword} 的原样返回；已有 ac= 参数时只补 wd，避免重复 ac。
QString appendProbeQuery(QString location)
{
    location = location.trimmed();
    if (location.isEmpty())
        return {};
    location.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    const int fragment = location.indexOf(QLatin1Char('#'));
    if (fragment >= 0)
        location.truncate(fragment);
    if (location.contains(QLatin1String("{keyword}")))
        return location;

    QString separator;
    if (location.endsWith(QLatin1Char('?')) || location.endsWith(QLatin1Char('&')))
        separator.clear();
    else if (location.contains(QLatin1Char('?')))
        separator = QStringLiteral("&");
    else
        separator = QStringLiteral("?");

    if (location.contains(QLatin1String("wd="), Qt::CaseInsensitive)) {
        // 用户可能直接粘贴了一条带示例关键词的搜索地址（?ac=detail&wd=电影）。
        // 把固定值换成占位符，比直接丢掉这条地址更有用；否则试搜永远无法验证搜索词。
        QRegularExpression wdRe(QStringLiteral("([?&])wd=[^&]*"),
                                QRegularExpression::CaseInsensitiveOption);
        location.replace(wdRe, QStringLiteral("\\1wd={keyword}"));
        return location;
    }

    if (location.contains(QLatin1String("/index.php/ajax/data"))) {
        if (!location.contains(QLatin1String("mid=")))
            location += separator + QStringLiteral("mid=1");
        if (!location.contains(QLatin1String("limit=")))
            location += QStringLiteral("&limit={limit}");
        return location + QStringLiteral("&wd={keyword}");
    }
    if (location.contains(QLatin1String("ac="), Qt::CaseInsensitive))
        return location + separator + QStringLiteral("wd={keyword}");
    return location + separator + QStringLiteral("ac=detail&wd={keyword}");
}

bool looksLikeApiPath(const QString &path)
{
    const QString lower = path.toLower();
    return lower.contains(QLatin1String("provide/vod"))
           || lower.contains(QLatin1String("/inc/api"))
           || lower.contains(QLatin1String("ajax/data"))
           || lower.contains(QLatin1String("api.php"));
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
                       "|/index\\.php/ajax/data)"),
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
    QSet<QString> titles;
    titles.reserve(second.size());
    for (const MediaItem &item : second)
        titles.insert(item.title.trimmed());
    for (const MediaItem &item : first) {
        if (!titles.contains(item.title.trimmed()))
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

/// 取前几条对象的字段样本，用来猜哪个字段是标题、播放地址……
QVector<FieldSample> collectSamples(const QJsonArray &items)
{
    QVector<FieldSample> samples;
    QHash<QString, int> index;
    const int take = qMin(items.size(), 5);
    for (int i = 0; i < take; ++i) {
        if (!items.at(i).isObject())
            continue;
        const QJsonObject object = items.at(i).toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            int slot = index.value(it.key(), -1);
            if (slot < 0) {
                FieldSample sample;
                sample.key = it.key();
                samples.append(sample);
                slot = samples.size() - 1;
                index.insert(it.key(), slot);
            }
            const QJsonValue value = it.value();
            if (value.isString())
                samples[slot].values.append(value.toString());
            else if (value.isDouble())
                samples[slot].values.append(QString::number(value.toDouble()));
        }
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
           || lower.contains(QLatin1String(".ogg"));
}

struct StreamPick
{
    QString key;
    QString kind;   // direct / episodes
};

/// 播放地址字段：优先认带 ".m3u8/.mp3/..." 或 "名称$地址#..." 形态的字段，
/// 这样不会把详情页链接误当成播放地址。
StreamPick pickStream(const QVector<FieldSample> &samples)
{
    StreamPick best;
    int bestScore = 0;
    for (const FieldSample &sample : samples) {
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
            } else if (looksPlayable(text)) {
                score = qMax(score, 2);
                if (kind.isEmpty())
                    kind = QStringLiteral("direct");
            }
        }
        // 同分时优先字段名里带 play 的，避免抓到 "picUrl" 之类
        if (score > bestScore
            || (score == bestScore && score > 0
                && sample.key.toLower().contains(QLatin1String("play")))) {
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
            || key.contains(QLatin1String("previewurl"))) {
            musicScore += 2;
        }

        const QStringList values = sample.values.mid(0, 3);
        for (const QString &value : values) {
            const QString text = value.toLower();
            if (text.contains(QLatin1String(".m3u8")) || text.contains(QLatin1String(".mp4"))
                || text.contains(QLatin1String(".flv")) || text.contains(QLatin1String(".mkv"))) {
                videoScore += 3;
            }
            if (text.contains(QLatin1String(".mp3")) || text.contains(QLatin1String(".m4a"))
                || text.contains(QLatin1String(".flac")) || text.contains(QLatin1String(".aac"))
                || text.contains(QLatin1String(".ogg")) || text.contains(QLatin1String(".wav"))) {
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

void scanArrays(const QJsonValue &node, const QString &path, int depth, QJsonArray *best,
                QString *bestPath)
{
    if (depth > 6)
        return;
    if (node.isArray()) {
        const QJsonArray array = node.toArray();
        if (!array.isEmpty() && array.first().isObject() && array.size() > best->size()) {
            *best = array;
            *bestPath = path;
        }
        // 结果数组内部一般没有更值钱的数组，不再往里钻
        return;
    }
    if (node.isObject()) {
        const QJsonObject object = node.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString child = path.isEmpty() ? it.key() : path + QLatin1Char('.') + it.key();
            scanArrays(it.value(), child, depth + 1, best, bestPath);
        }
    }
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
    return {QStringLiteral("/wp-json/wp/v2/search?search={keyword}&per_page={limit}"),
            QStringLiteral("/wp-json/wp/v2/posts?search={keyword}&per_page={limit}"),
            QStringLiteral("/api/search?keyword={keyword}&limit={limit}"),
            QStringLiteral("/ajax/search?keyword={keyword}&limit={limit}"),
            QStringLiteral("/api.php/provide/vod/?ac=detail&wd={keyword}")};
}

Prober::Prober(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

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

void Prober::start(const QUrl &site, MediaType hintType, const QStringList &extraPaths,
                   bool autoDetect)
{
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
    m_validationFirstItems.clear();
    // 失败归类计数必须每次探测清零，否则同一个 Prober 复用会把上一站的失败累积过来
    m_homeOk = false;
    m_connFail = 0;
    m_denied = 0;
    m_unauthorized = 0;
    m_notFound = 0;
    m_notJson = 0;
    m_xml = 0;
    m_jsonNoItems = 0;
    m_jsonNoFields = 0;
    m_clock.start();

    m_site = site;
    if (!m_site.isValid()
        || (m_site.scheme() != QLatin1String("http")
            && m_site.scheme() != QLatin1String("https"))) {
        fail(tr("网址无效，必须以 http:// 或 https:// 开头"));
        return;
    }
    if (m_site.path().isEmpty())
        m_site.setPath(QStringLiteral("/"));
    m_inputPath = m_site.path();
    m_origin = originOf(m_site);
    fetchHome();
}

void Prober::cancel()
{
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
}

bool Prober::budgetLeft() const
{
    // 自动识别要试两种类型的候选，给的时间略多
    return m_clock.elapsed() < (m_auto ? kBudgetMsAuto : kBudgetMs);
}

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

void Prober::fetchHome()
{
    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), m_origin.toString() + QLatin1Char('/'));

    // 首页只用来拿站点标题和真实域名，打不开也照样去试接口
    m_http->fetch(m_site, QByteArray(), headers, [this](QNetworkReply *reply) {
        if (m_finished) {
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
        QStringList paths;
        QVector<MediaType> pathTypes;
        QSet<QString> seenPaths;
        auto addPath = [&](QString path, MediaType type) {
            path = appendProbeQuery(path);
            if (path.isEmpty() || !path.contains(QLatin1String("{keyword}")))
                return; // 没有关键词占位符的地址不能当搜索源
            if (seenPaths.contains(path))
                return;
            seenPaths.insert(path);
            paths.append(path);
            pathTypes.append(type);
        };

        if (userPath.size() > 1 && looksLikeApiPath(userPath))
            addPath(userPath, m_type);

        for (const QString &hint : m_homeApiHints)
            addPath(hint, m_type);

        for (const QString &extra : m_extraPaths) {
            const QString trimmed = extra.trimmed();
            if (trimmed.isEmpty())
                continue;
            if (trimmed.startsWith(QLatin1String("http")))
                addPath(trimmed, m_type);
            else
                addPath(trimmed.startsWith(QLatin1Char('/')) ? trimmed
                                                             : QLatin1Char('/') + trimmed,
                        m_type);
        }

        const MediaType otherType =
            m_type == MediaType::Video ? MediaType::Music : MediaType::Video;
        for (const QString &path : defaultPaths(m_type))
            addPath(path, m_type);
        if (m_auto) {
            for (const QString &path : defaultPaths(otherType))
                addPath(path, otherType);
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
        // 必须按“域名优先”而不是“路径优先”排，否则一个 301 站点就会
        // 把一半尝试次数浪费在旧域名上。
        const bool hasRedirect = m_redirectOrigin.isValid() && !m_redirectOrigin.host().isEmpty()
                              && m_redirectOrigin != m_origin;
        for (int i = 0; i < paths.size(); ++i) {
            if (paths.at(i).startsWith(QLatin1String("http"), Qt::CaseInsensitive))
                appendCandidate(paths.at(i), pathTypes.value(i, m_type));
        }
        const QVector<QUrl> origins = hasRedirect ? QVector<QUrl>{m_origin, m_redirectOrigin}
                                                  : QVector<QUrl>{m_origin};
        for (const QUrl &origin : origins) {
            for (int i = 0; i < paths.size(); ++i) {
                const QString path = paths.at(i);
                if (path.startsWith(QLatin1String("http"), Qt::CaseInsensitive))
                    continue;
                const QString relative = path.startsWith(QLatin1Char('/'))
                                             ? path
                                             : QLatin1Char('/') + path;
                appendCandidate(origin.toString() + relative, pathTypes.value(i, m_type));
            }
        }

        probeNextCandidate();
    }, NetLimits::kHtmlBytes);
}

void Prober::probeNextCandidate()
{
    if (m_finished)
        return;
    const int baseAttempts = m_auto ? 20 : kMaxAttempts;
    // 首页线索和用户自定义路径是额外候选，不能占用内置路径的预算。
    const int maxAttempts = baseAttempts + m_extraPaths.size() + m_homeApiHints.size();
    if (m_candidateIndex >= m_candidates.size() || m_candidateIndex >= maxAttempts) {
        fail(diagnoseFailure());
        return;
    }
    if (!budgetLeft()) {
        fail(tr("探测超时：%1 个候选里没有一个在时限内返回").arg(m_candidateIndex));
        return;
    }

    const int candidateIndex = m_candidateIndex++;
    const QString tpl = m_candidates.at(candidateIndex);
    const MediaType candidateType = m_candidateTypes.value(candidateIndex, m_type);
    const QString keyword = candidateType == MediaType::Video ? tr("电影") : tr("音乐");
    const QString probeUrl = renderUrl(tpl, keyword, 10);

    QHash<QString, QString> headers;
    headers.insert(QStringLiteral("Referer"), m_origin.toString() + QLatin1Char('/'));

    QNetworkReply *reply = m_http->get(QUrl(probeUrl), QByteArray(), headers);
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

        QJsonArray items;
        QString itemsPath;
        scanArrays(root, QString(), 0, &items, &itemsPath);
        if (items.isEmpty()) {
            ++m_jsonNoItems;
            recordAttempt(probeUrl, status, tr("JSON 里没有结果数组"));
            probeNextCandidate();
            return;
        }

        const QVector<FieldSample> samples = collectSamples(items);
        const QString titleKey =
            pickKey(samples, {QStringLiteral("vod_name"), QStringLiteral("trackname"),
                              QStringLiteral("songname"), QStringLiteral("song"),
                              QStringLiteral("title"), QStringLiteral("name")}, {}, true);
        const StreamPick stream = pickStream(samples);
        if (titleKey.isEmpty() || stream.key.isEmpty()) {
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
        config.streamKind = stream.kind;
        config.headers.insert(QStringLiteral("Referer"),
                              m_origin.toString() + QLatin1Char('/'));
        config.fields.insert(QStringLiteral("title"), titleKey);
        config.fields.insert(QStringLiteral("streamUrl"), stream.key);

        QSet<QString> used{titleKey, stream.key};
        const QString artistKey =
            pickKey(samples, {QStringLiteral("artistname"), QStringLiteral("artist"),
                              QStringLiteral("singer"), QStringLiteral("author"),
                              QStringLiteral("vod_actor"), QStringLiteral("performer")}, used, true);
        if (!artistKey.isEmpty()) {
            config.fields.insert(QStringLiteral("artist"), artistKey);
            used.insert(artistKey);
        }

        const QString posterKey =
            pickKey(samples, {QStringLiteral("vod_pic"), QStringLiteral("pic"),
                              QStringLiteral("cover"), QStringLiteral("poster"),
                              QStringLiteral("thumb"), QStringLiteral("image")}, used);
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
                              QStringLiteral("web_url"), QStringLiteral("link")}, used);
        if (!webKey.isEmpty())
            config.fields.insert(QStringLiteral("webUrl"), webKey);

        recordAttempt(probeUrl, status, tr("接口可用，正在试搜校验"));
        validate(config, keyword);
    });
}

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

void Prober::startValidation(const SourceConfig &config, const QString &keyword, int pass)
{
    qCInfo(lcProbe) << "校验配置" << config.id << config.name << "第" << pass << "轮";

    ConfigSearchProvider *provider = new ConfigSearchProvider(config, m_http, this);
    m_validator = provider;
    connect(provider, &SearchProviderBase::finished, this,
            [this, config, pass](const QString &sourceId, const QVector<MediaItem> &items,
                                 int filtered, const QString &error) {
                Q_UNUSED(sourceId)
                Q_UNUSED(filtered)
                if (m_finished)
                    return;
                if (m_validator != nullptr) {
                    m_validator->deleteLater();
                    m_validator = nullptr;
                }

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
                        config.mediaType == MediaType::Video ? tr("电视剧") : tr("歌曲");
                    startValidation(config, secondKeyword, 2);
                    return;
                }

                if (error.isEmpty() && !items.isEmpty()
                    && sameResultTitles(m_validationFirstItems, items)) {
                    recordAttempt(config.urlTemplate, 0,
                                  tr("接口忽略关键词，只返回固定列表，不能作为搜索源"));
                    m_validationFirstItems.clear();
                    probeNextCandidate();
                    return;
                }
                // 第二次请求失败或没有结果时，说明首轮确实按关键词返回过数据，
                // 不能用一次网络抖动否定整个源。
                if (error.isEmpty() && items.isEmpty())
                    qCInfo(lcProbe) << config.id << "二次关键词没有命中，按首轮结果接受";
                acceptConfig(config, m_validationFirstItems);
            });
    provider->search(keyword, 10);
}

void Prober::acceptConfig(const SourceConfig &config, const QVector<MediaItem> &items)
{
    Result result = m_result;
    result.ok = true;
    result.mediaType = config.mediaType;
    result.autoDetected = config.mediaType != m_result.hintType;
    result.siteName = config.name;
    result.siteUrl = m_site;
    result.config = config;
    result.sampleCount = items.size();
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

QString Prober::diagnoseFailure() const
{
    if (m_denied > 0)
        return tr("站点拒绝程序访问（HTTP 401/403，命中 %1 次）：拦的是访问方式，不是接口路径，换地址也没用，只能用浏览器打开")
            .arg(m_denied);
    if (m_unauthorized > 0)
        return tr("接口返回“IP 未授权/已到期”（%1 次）：这不是路径问题，需要在资源站后台授权当前 IP 或更换有授权的域名")
            .arg(m_unauthorized);
    if (m_xml > 0)
        return tr("接口返回 XML（%1 次）：当前探查器只解析 JSON；如果站点同时提供 /at/json/，请改用 JSON 路径，否则需要手动配置外部解析器")
            .arg(m_xml);
    if (m_connFail > 0 && !m_homeOk)
        return tr("站点连不上（%1 次连接失败）：本机网络到该域名不通，或域名已失效")
            .arg(m_connFail);
    if (m_jsonNoItems > 0 && m_notFound == 0 && m_notJson == 0)
        return tr("接口返回 JSON 但没有结果数组（%1 次）：可能只支持分类/列表参数，或关键词参数名不是 wd")
            .arg(m_jsonNoItems);
    if (m_jsonNoFields > 0 && m_notFound == 0)
        return tr("接口有结果数组但猜不出标题/播放地址字段（%1 次）：该站点字段命名不标准，需要手动配置字段映射")
            .arg(m_jsonNoFields);
    if (m_notJson > 0 && m_notFound == 0)
        return tr("地址能访问但返回的是网页（%1 次），不是 JSON 接口：请直接填接口地址，例如 https://域名/api.php/provide/vod/")
            .arg(m_notJson);
    if (m_homeOk)
        return tr("首页能打开，但 %1 条候选路径全部 404：这个站很可能只有网页、没有采集接口，需要它的接口地址（形如 https://域名/api.php/provide/vod/）")
            .arg(m_notFound);
    return tr("没有找到可用的接口地址，这个网站可能不是标准 JSON 接口，需要手动配置");
}

void Prober::fail(const QString &error)
{    Result result = m_result;
    result.ok = false;
    result.mediaType = m_result.hintType;
    result.siteName = m_siteName;
    result.siteUrl = m_site;
    result.error = error;
    done(result);
}

void Prober::done(const Result &result)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_validator != nullptr) {
        m_validator->deleteLater();
        m_validator = nullptr;
    }
    emit finished(result);
}

} // namespace SourceProbe
