#include "net/MaccmsWebProvider.h"

#include "net/HttpClient.h"
#include "net/NetLimits.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QUrlQuery>

#include <algorithm>

Q_LOGGING_CATEGORY(lcWeb, "mvp.net.webvideo")

namespace {

/// 详情页地址（苹果CMS v10 / 自定义 /video/ 系 / v8）
const char *kDetailPattern =
    R"([^"]*?(?:/voddetail/\d+\.html|/video/\d+\.html|/detail/\d+\.html|/vod/\d+\.html|/movie/\d+\.html|/vod-detail-id-\d+\.html))";

/// 播放页地址（带线路 sid 的三种写法）
const char *kPlayPattern =
    R"([^"]*?(?:/vodplay/\d+-\d+-\d+\.html|/play/\d+/\d+/\d+\.html|/play/\d+-\d+-\d+\.html|/vod-play-id-\d+-sid-\d+-pid-\d+\.html))";

bool looksBinary(const QByteArray &raw)
{
    // 采集站的页面偶尔是 GBK，先看声明再决定用哪套解码器
    const QByteArray head = raw.left(2048).toLower();
    return head.contains("charset=gb") || head.contains("charset=\"gb")
           || head.contains("gb2312") || head.contains("gbk");
}

QString decodeHtml(const QByteArray &raw)
{
    if (raw.isEmpty())
        return QString();
    if (looksBinary(raw)) {
        QStringDecoder decoder("GB18030");
        return decoder.decode(raw);
    }
    return QString::fromUtf8(raw);
}

QString decodeEntities(const QString &raw)
{
    QString text = raw;
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    return text;
}

QString stripTags(const QString &raw)
{
    QString text = raw;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return decodeEntities(text).simplified();
}

/// player_aaaa 里的地址是 JS 字符串：\u56fd 这种转义必须先还原成中文，
/// 否则百分号编码出来的路径是错的（实测解码前请求 404，解码后 200）
QString unescapeJsUrl(const QString &raw)
{
    QString text = raw;
    text.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    text.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    text.replace(QStringLiteral("\\u0026"), QStringLiteral("&"));

    QRegularExpression unicode(QStringLiteral("\\\\u([0-9a-fA-F]{4})"));
    QRegularExpressionMatchIterator it = unicode.globalMatch(text);
    QString result;
    int last = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += text.mid(last, match.capturedStart() - last);
        result += QChar(static_cast<ushort>(match.captured(1).toUInt(nullptr, 16)));
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

/// 交给 QUrl 做百分号编码，中文路径必须编码后才能被 CDN 认出来
QUrl encodeMediaUrl(const QString &decoded)
{
    QUrl url(decoded);
    if (!url.isValid())
        return QUrl();
    if (url.host().isEmpty())
        return QUrl();
    return QUrl(url.toString(QUrl::FullyEncoded));
}

bool looksLikeStream(const QString &url)
{
    const QString lower = url.toLower();
    return lower.contains(QStringLiteral(".m3u8")) || lower.contains(QStringLiteral(".mp4"))
           || lower.contains(QStringLiteral(".flv")) || lower.contains(QStringLiteral(".mkv"))
           || lower.contains(QStringLiteral("m3u8?"));
}

/// 站点把播放地址写成自己的域名占位（encrypt 3 的典型形态），这种拿不到直链
bool isHostPlaceholder(const QString &url)
{
    const QUrl parsed(url);
    if (parsed.host().isEmpty())
        return true;
    const QString path = parsed.path();
    return path.isEmpty() || path == QLatin1String("/");
}

QString pickSid(const QString &href)
{
    const QRegularExpression vodPlay(QStringLiteral("/vodplay/\\d+-(\\d+)-\\d+\\.html"));
    QRegularExpressionMatch match = vodPlay.match(href);
    if (match.hasMatch())
        return match.captured(1);
    const QRegularExpression playDir(QStringLiteral("/play/\\d+/(\\d+)/\\d+\\.html"));
    match = playDir.match(href);
    if (match.hasMatch())
        return match.captured(1);
    const QRegularExpression playFlat(QStringLiteral("/play/\\d+-(\\d+)-\\d+\\.html"));
    match = playFlat.match(href);
    if (match.hasMatch())
        return match.captured(1);
    const QRegularExpression v8(QStringLiteral("/vod-play-id-\\d+-sid-(\\d+)-pid-\\d+\\.html"));
    match = v8.match(href);
    if (match.hasMatch())
        return match.captured(1);
    return QString();
}

/// 只取 href，不抓锚文本：这批站点的列表项和剧集按钮里都是嵌套 div，
/// 锚文本往往不是纯文本，标题统一从详情页取
QVector<QString> extractHrefs(const QString &html, const char *pattern)
{
    QVector<QString> hrefs;
    const QString expression =
        QStringLiteral("<a[^>]*href=\"(%1)\"").arg(QString::fromLatin1(pattern));
    const QRegularExpression re(expression);
    QRegularExpressionMatchIterator it = re.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString href = match.captured(1).trimmed();
        if (!href.isEmpty())
            hrefs.append(href);
    }
    return hrefs;
}

/// 连 href 带锚文本一起取（保持页面顺序）：有的模板把片名放在内层 <span>，
/// 整段取出来再去标签。按钮文字（详情/播放/下载）要滤掉，
/// 否则热门推荐位会被误当成搜索结果。
QVector<QPair<QString, QString>> extractEntries(const QString &html, const char *pattern)
{
    QVector<QPair<QString, QString>> entries;
    const QString expression =
        QStringLiteral("<a[^>]*href=\"(%1)\"[^>]*>([\\s\\S]{0,200}?)</a>")
            .arg(QString::fromLatin1(pattern));
    const QRegularExpression re(expression);
    QRegularExpressionMatchIterator it = re.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString href = match.captured(1).trimmed();
        if (href.isEmpty())
            continue;
        const QString title = stripTags(match.captured(2));
        if (title.isEmpty() || title.size() > 40)
            continue;
        if (title.contains(QStringLiteral("详情")) || title.contains(QStringLiteral("播放"))
            || title.contains(QStringLiteral("下载")) || title.contains(QStringLiteral("查看"))) {
            continue;
        }
        entries.append(qMakePair(href, title));
    }
    return entries;
}

QString firstGroup(const QString &html, const QString &pattern)
{
    const QRegularExpression re(pattern);
    const QRegularExpressionMatch match = re.match(html);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString metaContent(const QString &html, const QString &key)
{
    const QRegularExpression tagRe(
        QStringLiteral("<meta[^>]*%1[^>]*>").arg(key), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch tagMatch = tagRe.match(html);
    if (!tagMatch.hasMatch())
        return QString();
    const QString tag = tagMatch.captured(0);
    const QRegularExpression contentRe(QStringLiteral("content=\"([^\"]*)\""));
    const QRegularExpressionMatch contentMatch = contentRe.match(tag);
    return contentMatch.hasMatch() ? contentMatch.captured(1).trimmed() : QString();
}

/// 详情页标题。部分模板的 <h1> 里还套着 <a>/<span>（如 <h1><a>十万狂花入梦来</a></h1>），
/// 用“纯文本”正则抓不到，所以整段取出来再去标签；再不行退回 og:title / <title>，
/// 并按采集站常见写法清掉《》、站点后缀和“在线观看”这类装饰词。
QString detailTitle(const QString &html)
{
    const QString h1 = stripTags(firstGroup(html, QStringLiteral("<h1[^>]*>([\\s\\S]{0,200}?)</h1>")));
    if (!h1.isEmpty())
        return h1;

    QString candidate = stripTags(metaContent(html, QStringLiteral("property=\"og:title\"")));
    if (candidate.isEmpty())
        candidate = stripTags(
            firstGroup(html, QStringLiteral("<title[^>]*>([\\s\\S]{0,200}?)</title>")));
    if (candidate.isEmpty())
        return QString();

    const QRegularExpression bracket(QStringLiteral("《([^》]{1,60})》"));
    const QRegularExpressionMatch match = bracket.match(candidate);
    if (match.hasMatch())
        return match.captured(1).simplified();

    int cut = candidate.indexOf(QStringLiteral(" - "));
    if (cut <= 0)
        cut = candidate.indexOf(QLatin1Char('_'));
    if (cut > 0)
        candidate.truncate(cut);
    static const QStringList noise = {
        QStringLiteral("电视剧"), QStringLiteral("电影"), QStringLiteral("在线观看"),
        QStringLiteral("高清"), QStringLiteral("完整版"), QStringLiteral("百度云")};
    for (const QString &word : noise)
        candidate.remove(word);
    return candidate.simplified();
}

} // namespace

MaccmsWebProvider::MaccmsWebProvider(const SourceConfig &config, HttpClient *http,
                                     QObject *parent)
    : SearchProviderBase(parent)
    , m_config(config)
    // 网页源的请求链最长（首页→建议→搜索→详情→播放），超时/取消时必须能
    // 单独中止，不能调用共享 HttpClient 的 abortAll() 误伤其它搜索源。
    , m_http(new HttpClient(this))
{
    Q_UNUSED(http)
}

void MaccmsWebProvider::abort()
{
    // 先置 done，abort 同步触发的 finished 回调不会再启动下一跳请求。
    m_done = true;
    if (m_http != nullptr)
        m_http->abortAll();
}

void MaccmsWebProvider::search(const QString &keyword, int limit)
{
    m_keyword = keyword.trimmed();
    // 搜索页只拿候选，详情/播放页按 4 路并发抓取；上限跟设置一致，
    // 但保留 50 条的硬上限，避免一次搜索打出几百个请求。
    m_limit = qBound(1, limit, 50);
    m_keywordEncoded = QString::fromLatin1(QUrl::toPercentEncoding(m_keyword));
    m_done = false;
    m_candidates.clear();
    m_slots.clear();
    m_items.clear();
    m_searchErrors.clear();
    m_firstFailure.clear();
    m_filtered = 0;
    m_nextCursor = 0;
    m_active = 0;
    m_rootLength = 0;

    QString base = m_config.urlTemplate.trimmed();
    if (base.isEmpty()) {
        finish(tr("没有配置站点地址"));
        return;
    }
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    const QUrl parsed(base);
    if (parsed.scheme() != QLatin1String("http") && parsed.scheme() != QLatin1String("https")) {
        finish(tr("站点地址不是 http/https：%1").arg(base));
        return;
    }
    m_root = base;

    if (m_http == nullptr) {
        finish(tr("网络模块未就绪"));
        return;
    }
    fetchRoot();
}

QHash<QString, QString> MaccmsWebProvider::requestHeaders() const
{
    QHash<QString, QString> headers = m_config.headers;
    // 站点根地址在“一键更新”后可能变了，旧 Referer 不能继续沿用
    headers.insert(QStringLiteral("Referer"), m_root);
    if (!m_config.cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), m_config.cookie);
    if (!headers.contains(QStringLiteral("Accept")))
        headers.insert(QStringLiteral("Accept"),
                       QStringLiteral("text/html,application/xhtml+xml,*/*;q=0.8"));
    return headers;
}

QHash<QString, QString> MaccmsWebProvider::mediaHeaders() const
{
    // 播放/下载要按源补 Referer：dmxq 的 m3u8 不带 Referer 直接 403
    QHash<QString, QString> headers = m_config.headers;
    headers.insert(QStringLiteral("Referer"), m_root);
    if (!m_config.cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), m_config.cookie);
    return headers;
}

QUrl MaccmsWebProvider::siteUrl(const QString &relative) const
{
    if (relative.isEmpty())
        return QUrl();
    const QUrl parsed(relative);
    if (parsed.isValid() && !parsed.scheme().isEmpty()
        && (parsed.scheme() == QLatin1String("http") || parsed.scheme() == QLatin1String("https")))
        return parsed;
    QString path = relative;
    if (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    return QUrl(m_root + path);
}

void MaccmsWebProvider::requestHtml(
    const QUrl &url, const std::function<void(bool, const QString &)> &callback)
{
    if (url.isEmpty()) {
        callback(false, QString());
        return;
    }
    QPointer<MaccmsWebProvider> self(this);
    m_http->fetch(url, QByteArray(), requestHeaders(),
                  [self, callback](QNetworkReply *reply) {
                      if (self.isNull()) {
                          // 源已被超时/取消销毁，迟到的响应必须自己收尾，
                          // 否则 reply 会一直挂在共享的 HttpClient 上。
                          if (reply != nullptr) {
                              reply->abort();
                              reply->deleteLater();
                          }
                          return;
                      }
                      if (reply == nullptr) {
                          callback(false, QString());
                          return;
                      }
                      bool tooLarge = false;
                      const QByteArray raw =
                          HttpClient::readAllLimited(reply, NetLimits::kHtmlBytes, &tooLarge);
                      if (tooLarge) {
                          qCWarning(lcWeb) << "页面超过" << NetLimits::kHtmlBytes
                                           << "字节上限："
                                           << HttpClient::redactUrl(reply->url());
                          reply->deleteLater();
                          callback(false, QString());
                          return;
                      }
                      if (!HttpClient::isHttpSuccess(reply) || raw.isEmpty()) {
                          qCDebug(lcWeb) << "请求失败"
                                       << HttpClient::redactUrl(reply->url())
                                       << HttpClient::describeError(reply);
                          reply->deleteLater();
                          callback(false, QString());
                          return;
                      }
                      reply->deleteLater();
                      callback(true, decodeHtml(raw));
                  });
}

void MaccmsWebProvider::fetchRoot()
{
    // 首页只用来测量"回首页"的长度基线，以及确认站点还活着
    QPointer<MaccmsWebProvider> self(this);
    m_http->fetch(QUrl(m_root), QByteArray(), requestHeaders(),
                  [self](QNetworkReply *reply) {
                      if (self.isNull()) {
                          if (reply != nullptr) {
                              reply->abort();
                              reply->deleteLater();
                          }
                          return;
                      }
                      if (reply != nullptr) {
                          bool tooLarge = false;
                          const QByteArray raw = HttpClient::readAllLimited(
                              reply, NetLimits::kHtmlBytes, &tooLarge);
                          if (!tooLarge && HttpClient::isHttpSuccess(reply) && !raw.isEmpty())
                              self->m_rootLength = decodeHtml(raw).size();
                          reply->deleteLater();
                      }
                      self->trySuggest();
                  });
}

void MaccmsWebProvider::trySuggest()
{
    if (m_done)
        return;
    // 苹果CMS 的搜索建议接口：一次请求就能拿到 id/片名/海报，比解析搜索页少一跳，
    // 也不受"搜索页被换成热榜"这类改版影响。没有这个接口的站会返回 404/HTML，退回 HTML 搜索链。
    const QUrl url =
        siteUrl(QStringLiteral("index.php/ajax/suggest?mid=1&wd=") + m_keywordEncoded);
    QPointer<MaccmsWebProvider> self(this);
    m_http->fetch(url, QByteArray(), requestHeaders(),
                  [self](QNetworkReply *reply) {
                      if (self.isNull()) {
                          if (reply != nullptr) {
                              reply->abort();
                              reply->deleteLater();
                          }
                          return;
                      }
                      QByteArray raw;
                      bool ok = false;
                      if (reply != nullptr) {
                          bool tooLarge = false;
                          raw = HttpClient::readAllLimited(reply, NetLimits::kJsonBytes,
                                                           &tooLarge);
                          ok = !tooLarge && HttpClient::isHttpSuccess(reply);
                          reply->deleteLater();
                      }
                      QJsonParseError parseError{};
                      const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
                      const QJsonArray list =
                          (ok && parseError.error == QJsonParseError::NoError
                           && document.isObject())
                              ? document.object().value(QStringLiteral("list")).toArray()
                              : QJsonArray();
                      if (list.isEmpty()) {
                          self->trySearch(0);
                          return;
                      }

                      for (const QJsonValue &value : list) {
                          if (self->m_candidates.size() >= self->m_limit)
                              break;
                          const QJsonObject item = value.toObject();
                          const QString id =
                              item.value(QStringLiteral("id")).toVariant().toString().trimmed();
                          if (id.isEmpty())
                              continue;
                          Candidate candidate;
                          // 苹果CMS 默认详情路径；路径不同的站会在下面整体退回 HTML 搜索
                          candidate.detailUrl =
                              self->siteUrl(QStringLiteral("voddetail/%1.html").arg(id));
                          candidate.title =
                              stripTags(item.value(QStringLiteral("name")).toString());
                          const QString pic =
                              decodeEntities(item.value(QStringLiteral("pic")).toString().trimmed());
                          if (!pic.isEmpty())
                              candidate.poster = self->siteUrl(pic).toString();
                          self->m_candidates.append(candidate);
                      }
                      if (self->m_candidates.isEmpty()) {
                          self->trySearch(0);
                          return;
                      }
                      self->m_suggestUsed = true;
                      qCInfo(lcWeb) << "suggest 命中" << self->m_candidates.size() << "条"
                                 << self->m_root;
                      self->m_nextCursor = 0;
                      self->m_active = 0;
                      self->startWorkers();
                  });
}

void MaccmsWebProvider::trySearch(int strategy)
{
    if (m_done)
        return;
    QPointer<MaccmsWebProvider> self(this);

    auto fail = [self, strategy](const QString &reason) {
        if (self.isNull())
            return;
        self->m_searchErrors.append(QStringLiteral("策略%1：%2").arg(strategy + 1).arg(reason));
        self->trySearch(strategy + 1);
    };

    if (strategy > 4) {
        nextStrategyOrFinish();
        return;
    }
    if (strategy == 0)
        m_htmlSearchTried = true;

    switch (strategy) {
    case 0: // 苹果CMS v10 标准搜索
        requestHtml(siteUrl(QStringLiteral("vodsearch/-------------.html?wd=") + m_keywordEncoded),
                    [self, fail](bool ok, const QString &html) {
                        if (self.isNull())
                            return;
                        if (!ok) {
                            fail(tr("请求失败"));
                            return;
                        }
                        self->afterSearch(html, 0);
                    });
        break;
    case 1: // 自定义模板（/video/ 系）
        requestHtml(siteUrl(QStringLiteral("search.html?wd=") + m_keywordEncoded),
                    [self, fail](bool ok, const QString &html) {
                        if (self.isNull())
                            return;
                        if (!ok) {
                            fail(tr("请求失败"));
                            return;
                        }
                        self->afterSearch(html, 1);
                    });
        break;
    case 2: // 另一种路径形态
        requestHtml(siteUrl(QStringLiteral("search/-------------.html?wd=") + m_keywordEncoded),
                    [self, fail](bool ok, const QString &html) {
                        if (self.isNull())
                            return;
                        if (!ok) {
                            fail(tr("请求失败"));
                            return;
                        }
                        self->afterSearch(html, 2);
                    });
        break;
    case 3: { // 苹果CMS v8 表单搜索
        QNetworkReply *reply =
            m_http->postForm(siteUrl(QStringLiteral("index.php?m=vod-search")),
                             QByteArray("wd=") + m_keywordEncoded.toUtf8(), requestHeaders());
        if (reply == nullptr) {
            fail(tr("请求未发出"));
            break;
        }
        connect(reply, &QNetworkReply::finished, this, [self, reply, fail] {
            reply->deleteLater();
            if (self.isNull())
                return;
            bool tooLarge = false;
            const QByteArray raw =
                HttpClient::readAllLimited(reply, NetLimits::kHtmlBytes, &tooLarge);
            if (tooLarge) {
                fail(tr("搜索结果超过 4 MB 上限"));
                return;
            }
            if (!HttpClient::isHttpSuccess(reply) || raw.isEmpty()) {
                fail(tr("请求失败"));
                return;
            }
            self->afterSearch(decodeHtml(raw), 3);
        });
        break;
    }
    default: { // 帝国CMS 通用搜索
        QNetworkReply *reply = m_http->postForm(
            siteUrl(QStringLiteral("e/search/index.php")),
            QByteArray("show=title&tempid=1&keyboard=") + m_keywordEncoded.toUtf8(),
            requestHeaders());
        if (reply == nullptr) {
            fail(tr("请求未发出"));
            break;
        }
        connect(reply, &QNetworkReply::finished, this, [self, reply, fail] {
            reply->deleteLater();
            if (self.isNull())
                return;
            bool tooLarge = false;
            const QByteArray raw =
                HttpClient::readAllLimited(reply, NetLimits::kHtmlBytes, &tooLarge);
            if (tooLarge) {
                fail(tr("搜索结果超过 4 MB 上限"));
                return;
            }
            if (!HttpClient::isHttpSuccess(reply) || raw.isEmpty()) {
                fail(tr("请求失败"));
                return;
            }
            self->afterSearch(decodeHtml(raw), 4);
        });
        break;
    }
    }
}

void MaccmsWebProvider::afterSearch(const QString &html, int strategy)
{
    if (m_done)
        return;
    if (html.size() < 500) {
        m_searchErrors.append(tr("策略%1 返回内容过短").arg(strategy + 1));
        trySearch(strategy + 1);
        return;
    }
    // 假阳性主力来源：搜索地址无效时站点直接回首页（长度几乎与首页一致，且不回显关键词）
    if (m_rootLength > 2000
        && qAbs(html.size() - m_rootLength) <= qMax(200, m_rootLength / 50)
        && !html.contains(m_keyword)) {
        m_searchErrors.append(tr("策略%1 返回的是首页内容（搜索入口被禁用或地址不对）")
                                  .arg(strategy + 1));
        trySearch(strategy + 1);
        return;
    }

    const QVector<QString> detailHrefs = extractHrefs(html, kDetailPattern);
    if (detailHrefs.isEmpty()) {
        m_searchErrors.append(tr("策略%1 没有解析到影片详情链接").arg(strategy + 1));
        trySearch(strategy + 1);
        return;
    }

    // 有的站点搜索页实际渲染的是“热门/推荐”列表（链接一堆、片名一个都不匹配），
    // 关键词只回显在 <title> 里，长度校验骗不过它。真搜索的头三条一定与关键词相关，
    // 热榜/推荐位不会，所以只看前三条做相关性判定。
    const QVector<QPair<QString, QString>> entries = extractEntries(html, kDetailPattern);
    QHash<QString, QString> titles;
    for (const QPair<QString, QString> &entry : entries) {
        if (!titles.contains(entry.first))
            titles.insert(entry.first, entry.second);
    }
    const QString normalizedKeyword = QString(m_keyword).remove(QRegularExpression(
        QStringLiteral("\\s")));
    if (!entries.isEmpty() && !normalizedKeyword.isEmpty()) {
        bool relevant = false;
        int checked = 0;
        for (const QPair<QString, QString> &entry : entries) {
            if (checked >= 3)
                break;
            ++checked;
            const QString title = QString(entry.second).remove(QRegularExpression(
                QStringLiteral("\\s")));
            if (title.contains(normalizedKeyword, Qt::CaseInsensitive)) {
                relevant = true;
                break;
            }
        }
        if (!relevant) {
            m_searchErrors.append(
                tr("策略%1 返回的是站点推荐位，不是「%2」的搜索结果（搜索可能已关闭）")
                    .arg(strategy + 1)
                    .arg(m_keyword));
            trySearch(strategy + 1);
            return;
        }
    }

    QSet<QString> seen;
    for (const QString &href : detailHrefs) {
        const QUrl detail = siteUrl(href);
        if (detail.isEmpty())
            continue;
        const QString key = detail.toString();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        Candidate candidate;
        candidate.detailUrl = detail;
        candidate.title = titles.value(href); // 详情页 <h1> 取到的话以详情页为准
        m_candidates.append(candidate);
        if (m_candidates.size() >= m_limit)
            break;
    }

    qCInfo(lcWeb) << "网页源搜索命中" << m_candidates.size() << "条" << m_root;
    m_nextCursor = 0;
    m_active = 0;
    startWorkers();
}

void MaccmsWebProvider::nextStrategyOrFinish()
{
    if (m_items.isEmpty() && m_candidates.isEmpty()) {
        const QString detail = m_searchErrors.isEmpty()
                                   ? QString()
                                   : QStringLiteral("（%1）").arg(m_searchErrors.join(QStringLiteral("；")));
        finish(tr("没有搜到结果，站点可能改版或未开放搜索%1").arg(detail));
        return;
    }
    startWorkers();
}

void MaccmsWebProvider::startWorkers()
{
    if (m_done)
        return;

    // 每个候选要打 1 次详情 + 1~3 次播放页；串行时 6 条就要七八秒。
    // 这里最多 4 条同时抓，既让设置里的 10/20/30/50 生效，也不把请求数放大成串行等待。
    static constexpr int kConcurrency = 4;
    while (m_active < kConcurrency && m_nextCursor < m_candidates.size()
           && m_slots.size() < m_limit) {
        auto job = QSharedPointer<Job>::create();
        job->index = m_nextCursor++;
        job->candidate = m_candidates.at(job->index);
        ++m_active;
        processJob(job);
    }

    if (m_active == 0
        && (m_nextCursor >= m_candidates.size() || m_slots.size() >= m_limit)) {
        // suggest 给了 id 但详情页全打不开：说明这个站不是 /voddetail/<id>.html 这套路径，
        // 退回 HTML 搜索链再试一遍（只看详情路径，不代表站点没有内容）
        if (m_slots.isEmpty() && m_suggestUsed && !m_htmlSearchTried) {
            m_suggestUsed = false;
            m_candidates.clear();
            m_slots.clear();
            m_nextCursor = 0;
            m_active = 0;
            m_firstFailure.clear();
            trySearch(0);
            return;
        }
        if (m_slots.isEmpty() && !m_firstFailure.isEmpty())
            finish(m_firstFailure);
        else
            finish(QString());
    }
}

void MaccmsWebProvider::processJob(const QSharedPointer<Job> &job)
{
    if (m_done || job.isNull())
        return;

    QPointer<MaccmsWebProvider> self(this);
    requestHtml(job->candidate.detailUrl, [self, job](bool ok, const QString &html) {
        if (self.isNull())
            return;
        if (!ok) {
            self->finishJob(job, false, MediaItem(), tr("详情页打不开"));
            return;
        }

        const QString pageTitle = detailTitle(html);
        if (!pageTitle.isEmpty())
            job->candidate.title = pageTitle;

        const QString poster = metaContent(html, QStringLiteral("property=\"og:image\""));
        if (!poster.isEmpty())
            job->candidate.poster = decodeEntities(poster);
        if (job->candidate.poster.isEmpty()) {
            // 多数模板没有 og:image，海报挂在 module-info-poster 块里
            const int posterPos = html.indexOf(QStringLiteral("module-info-poster"));
            if (posterPos >= 0) {
                const QString chunk = html.mid(posterPos, 900);
                QString source = firstGroup(chunk, QStringLiteral("src=\"([^\"]+)\""));
                if (source.isEmpty())
                    source = firstGroup(chunk, QStringLiteral("data-original=\"([^\"]+)\""));
                if (!source.isEmpty())
                    job->candidate.poster = decodeEntities(source);
            }
        }
        if (job->candidate.poster.isEmpty()) {
            const QString dataSrc =
                firstGroup(html, QStringLiteral("<img[^>]*data-original=\"([^\"]+)\""));
            if (!dataSrc.isEmpty())
                job->candidate.poster = decodeEntities(dataSrc);
        }
        const QString description = metaContent(html, QStringLiteral("name=\"description\""));
        if (!description.isEmpty())
            job->candidate.description = stripTags(description);

        job->candidate.year = firstGroup(html, QStringLiteral("年份[:：]\\s*(\\d{4})"));
        if (job->candidate.year.isEmpty())
            job->candidate.year = firstGroup(html, QStringLiteral(">((?:19|20)\\d{2})<"));
        job->candidate.area = firstGroup(html, QStringLiteral("地区[:：]\\s*([^<\\s]{1,12})"));
        job->candidate.genre = firstGroup(html, QStringLiteral("类型[:：]\\s*([^<]{1,24})"));

        // 详情页里通常是整部剧的播放链接：按线路各取第一集
        const QVector<QString> plays = extractHrefs(html, kPlayPattern);
        QStringList sidOrder;
        QHash<QString, QString> bySid;
        int loose = 0;
        for (const QString &play : plays) {
            const QString sid = pickSid(play);
            const QString key = sid.isEmpty() ? QStringLiteral("#%1").arg(loose) : sid;
            if (bySid.contains(key))
                continue;
            bySid.insert(key, play);
            sidOrder.append(key);
            ++loose;
            if (sidOrder.size() >= 4)
                break;
        }
        job->candidate.playLines.clear();
        for (const QString &key : sidOrder)
            job->candidate.playLines.append(bySid.value(key));

        if (job->candidate.playLines.isEmpty()) {
            self->finishJob(job, false, MediaItem(), tr("详情页里没有播放入口"));
            return;
        }
        qCInfo(lcWeb) << "详情解析" << job->candidate.title << "线路"
                   << job->candidate.playLines.size();
        job->lineIndex = 0;
        self->processPlay(job);
    });
}

void MaccmsWebProvider::processPlay(const QSharedPointer<Job> &job)
{
    if (m_done || job.isNull())
        return;
    const int lineIndex = job->lineIndex;
    if (lineIndex >= job->candidate.playLines.size() || lineIndex >= 3) {
        finishJob(job, false, MediaItem(),
                  job->firstFailure.isEmpty() ? tr("所有线路都取不到可播地址")
                                             : job->firstFailure);
        return;
    }

    const QUrl playUrl = siteUrl(job->candidate.playLines.at(lineIndex));
    QPointer<MaccmsWebProvider> self(this);
    requestHtml(playUrl, [self, job, lineIndex](bool ok, const QString &html) {
        if (self.isNull())
            return;

        auto retryOrFail = [self, job, lineIndex](const QString &reason) {
            if (job->firstFailure.isEmpty())
                job->firstFailure = reason;
            job->lineIndex = lineIndex + 1;
            if (job->lineIndex >= job->candidate.playLines.size() || job->lineIndex >= 3)
                self->finishJob(job, false, MediaItem(), job->firstFailure);
            else
                self->processPlay(job);
        };

        if (!ok) {
            retryOrFail(tr("播放页打不开"));
            return;
        }
        if (html.contains(QStringLiteral("weihu.html"))
            || html.contains(QStringLiteral("内容审核中"))) {
            self->finishJob(job, false, MediaItem(), tr("片源已下架（站点维护或审核中）"));
            return;
        }

        // 播放参数就藏在 player_aaaa 这一个变量里
        const QRegularExpression playerRe(QStringLiteral("player_aaaa\\s*=\\s*(\\{.*?\\})\\s*[;<]"),
                                          QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch playerMatch = playerRe.match(html);

        int encrypt = 0;
        QString url;
        QString from;
        QString vodName;

        if (playerMatch.hasMatch()) {
            const QByteArray json = playerMatch.captured(1).toUtf8();
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QJsonObject object = document.object();
                encrypt = object.value(QStringLiteral("encrypt")).toInt(0);
                url = object.value(QStringLiteral("url")).toString();
                from = object.value(QStringLiteral("from")).toString();
                vodName = object.value(QStringLiteral("vod_data"))
                              .toObject()
                              .value(QStringLiteral("vod_name"))
                              .toString();
            }
        }
        if (url.isEmpty()) {
            // JSON 里带转义或单引号时退回正则取字段
            url = firstGroup(html, QStringLiteral("\"url\":\"((?:[^\"\\\\]|\\\\.)*)\""));
            if (url.isEmpty())
                url = firstGroup(html, QStringLiteral("\"url\":\"([^\"]*)\""));
            from = firstGroup(html, QStringLiteral("\"from\":\"([^\"]*)\""));
            encrypt = firstGroup(html, QStringLiteral("\"encrypt\":\"?(\\d+)")).toInt();
            vodName = firstGroup(html, QStringLiteral("\"vod_name\":\"([^\"]*)\""));
        }

        if (url.isEmpty()) {
            if (html.contains(QStringLiteral("<iframe"))) {
                self->finishJob(job, false, MediaItem(),
                                tr("该线路是第三方解析（iframe），真实地址要外部解析器"));
                return;
            }
            self->finishJob(job, false, MediaItem(), tr("播放参数为空，片源已失效"));
            return;
        }

        if (!vodName.isEmpty())
            job->candidate.title = stripTags(vodName);

        if (encrypt != 0) {
            retryOrFail(tr("播放地址已加密（encrypt=%1，线路 %2），需逆向站点播放器，暂不支持")
                            .arg(encrypt)
                            .arg(from.isEmpty() ? QStringLiteral("未知") : from));
            return;
        }

        const QString decoded = unescapeJsUrl(url);
        if (isHostPlaceholder(decoded)) {
            retryOrFail(tr("该线路只给出站点域名占位（线路 %1），真实地址由播放器动态解密，暂不支持")
                            .arg(from.isEmpty() ? QStringLiteral("未知") : from));
            return;
        }
        if (!looksLikeStream(decoded)) {
            retryOrFail(tr("播放参数不是直链（%1），可能要二次解析").arg(decoded.left(60)));
            return;
        }

        const QUrl media = encodeMediaUrl(decoded);
        if (media.isEmpty() || !media.isValid()) {
            retryOrFail(tr("播放地址无法解析"));
            return;
        }

        MediaItem item;
        item.type = MediaType::Video;
        item.title = job->candidate.title;
        item.url = media;
        item.webUrl = job->candidate.detailUrl;
        item.sourceId = self->m_config.id;
        item.online = true;
        item.headers = self->mediaHeaders();
        item.posterUrl = self->siteUrl(job->candidate.poster);
        item.year = job->candidate.year;
        item.area = job->candidate.area;
        item.genre = job->candidate.genre;
        item.description = job->candidate.description;

        qCInfo(lcWeb) << "取到直链" << item.title << HttpClient::redactUrl(media);
        self->finishJob(job, true, item);
    });
}

void MaccmsWebProvider::finishJob(const QSharedPointer<Job> &job, bool ok, const MediaItem &item,
                                 const QString &reason)
{
    if (m_done || job.isNull())
        return;

    if (m_active > 0)
        --m_active;

    if (ok) {
        if (m_slots.size() < m_limit)
            m_slots.append(qMakePair(job->index, item));
    } else {
        ++m_filtered;
        if (m_firstFailure.isEmpty())
            m_firstFailure = reason;
        qCInfo(lcWeb) << "跳过" << job->candidate.title << reason;
    }
    startWorkers();
}

void MaccmsWebProvider::finish(const QString &error)
{
    if (m_done)
        return;
    m_done = true;

    // 并发完成顺序不等于搜索页顺序；按候选序号排回搜索页原始顺序。
    std::sort(m_slots.begin(), m_slots.end(),
              [](const QPair<int, MediaItem> &left, const QPair<int, MediaItem> &right) {
                  return left.first < right.first;
              });
    m_items.clear();
    m_items.reserve(m_slots.size());
    for (const QPair<int, MediaItem> &slot : m_slots)
        m_items.append(slot.second);

    qCInfo(lcWeb) << "网页源" << m_config.id << "结果" << m_items.size() << "条，跳过"
                  << m_filtered << "条" << (error.isEmpty() ? QStringLiteral("成功") : error);
    emit finished(m_config.id, m_items, m_filtered, error);
}
