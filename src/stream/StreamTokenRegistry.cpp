#include "stream/StreamTokenRegistry.h"

#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace {
constexpr qint64 kIdleExpireMs = 2LL * 60 * 60 * 1000;
/// 刚登记/刚使用的令牌在保留期内绝不淘汰。长 VOD 播放列表（例如 2 秒一片、
/// 两个多小时的正片会有 4000 多个分片）会在一次重写中瞬间登记几千个 token；
/// 如果按容量立刻淘汰，列表里已经写出去的 URL 会指向已删除 token，播放器请求
/// 开头分片时全部 404（FFmpeg 报 Invalid data found when processing input）。
constexpr qint64 kMinRetentionMs = 10LL * 60 * 1000;
constexpr int kPruneSmallEvery = 64;
/// 正常播放列表上限是 10000 个分片，容量必须覆盖一整个长列表，再加主列表/
/// 音轨/字幕等附加 URL，避免一次重写就触发淘汰。
constexpr int kMaxTokens = 16384;
} // namespace

void StreamTokenRegistry::clear()
{
    m_tokens.clear();
    m_tokenByKey.clear();
    m_pruneCounter = 0;
}

QString StreamTokenRegistry::cacheKeyFor(const QUrl &upstream,
                                         const QHash<QString, QString> &headers)
{
    QStringList headerKeys = headers.keys();
    headerKeys.sort();
    QString key = upstream.toString(QUrl::FullyEncoded);
    for (const QString &header : std::as_const(headerKeys))
        key += QLatin1Char('\x1f') + header + QLatin1Char('\x1e') + headers.value(header);
    return key;
}

QUrl StreamTokenRegistry::registerFor(const QUrl &upstream,
                                      const QHash<QString, QString> &headers, int port)
{
    if (port <= 0 || !upstream.isValid() || upstream.isEmpty())
        return QUrl();

    prune();

    // 同一个分片在顺序播放/预取/拖动时会反复登记；按地址+鉴权头复用，
    // 避免每个请求都复制一份 Cookie 并留下大量只读 token。
    const QString key = cacheKeyFor(upstream, headers);
    const QString existing = m_tokenByKey.value(key);
    if (!existing.isEmpty() && m_tokens.contains(existing)) {
        m_tokens[existing].lastUsedMs = QDateTime::currentMSecsSinceEpoch();
        return QUrl(QStringLiteral("http://127.0.0.1:%1/stream/%2/").arg(port).arg(existing));
    }

    const QString token = QString::number(QRandomGenerator::global()->generate64(), 16);
    Upstream entry;
    entry.url = upstream;
    // token 可能对应播放列表，也可能对应 CDN 分片。按目标 host 过滤敏感头：
    // 播放列表在源站时保留 Cookie，分片换到 CDN 时自动剥掉。
    entry.headers = HeaderPolicy::sanitizedForTarget(upstream, headers);
    entry.lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    entry.cacheKey = key;
    m_tokens.insert(token, entry);
    m_tokenByKey.insert(key, token);

    // 末尾的斜杠很关键：m3u8 里的相对分片会按“目录”解析，
    // 少了它 /stream/<token>/000001.ts 会被播放器解析成 /stream/000001.ts。
    return QUrl(QStringLiteral("http://127.0.0.1:%1/stream/%2/").arg(port).arg(token));
}

bool StreamTokenRegistry::lookup(const QString &token, Upstream *upstream)
{
    auto it = m_tokens.find(token);
    if (it == m_tokens.end())
        return false;
    it->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    if (upstream != nullptr)
        *upstream = it.value();
    return true;
}

void StreamTokenRegistry::prune()
{
    // 每登记 64 个 token 才扫一次全表。长播放列表一次会登记几千个 token，
    // 旧实现超过 512 后每次登记都全表扫描，复杂度接近 O(n²)。
    if (++m_pruneCounter < kPruneSmallEvery)
        return;
    m_pruneCounter = 0;

    pruneAt(QDateTime::currentMSecsSinceEpoch());
}

void StreamTokenRegistry::pruneAt(qint64 nowMs)
{
    const qint64 expireBefore = nowMs - kIdleExpireMs;
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->lastUsedMs > 0 && it->lastUsedMs < expireBefore) {
            if (m_tokenByKey.value(it->cacheKey) == it.key())
                m_tokenByKey.remove(it->cacheKey);
            it = m_tokens.erase(it);
        } else {
            ++it;
        }
    }

    // 极端情况下（一个超长播放列表反复刷新）仍超过上限，只丢“已经过保留期”的
    // 最老四分之一。保留期内的 token 可能正被刚返回的播放列表引用，淘汰它们
    // 会让播放器请求开头分片时拿不到上游地址。宁可短暂超限，也不能拆正在用的列表。
    if (m_tokens.size() > kMaxTokens) {
        const qint64 freshAfter = nowMs - kMinRetentionMs;
        QList<QPair<qint64, QString>> byAge;
        byAge.reserve(m_tokens.size());
        for (auto it = m_tokens.cbegin(); it != m_tokens.cend(); ++it) {
            if (it->lastUsedMs <= 0 || it->lastUsedMs < freshAfter)
                byAge.append({it->lastUsedMs, it.key()});
        }
        std::sort(byAge.begin(), byAge.end(),
                  [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                      return a.first < b.first;
                  });
        const int removeCount = m_tokens.size() / 4;
        for (int i = 0; i < removeCount && i < byAge.size(); ++i) {
            const QString token = byAge.at(i).second;
            const auto it = m_tokens.constFind(token);
            if (it != m_tokens.cend() && m_tokenByKey.value(it->cacheKey) == token)
                m_tokenByKey.remove(it->cacheKey);
            m_tokens.remove(token);
        }
    }
}
