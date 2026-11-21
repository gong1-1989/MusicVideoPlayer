#include "stream/StreamTokenRegistry.h"

#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>
#include <utility>

void StreamTokenRegistry::clear()
{
    m_tokens.clear();
}

QUrl StreamTokenRegistry::registerFor(const QUrl &upstream,
                                      const QHash<QString, QString> &headers, int port)
{
    if (port <= 0 || !upstream.isValid() || upstream.isEmpty())
        return QUrl();

    prune();

    const QString token = QString::number(QRandomGenerator::global()->generate64(), 16);
    Upstream entry;
    entry.url = upstream;
    // token 可能对应播放列表，也可能对应 CDN 分片。按目标 host 过滤敏感头：
    // 播放列表在源站时保留 Cookie，分片换到 CDN 时自动剥掉。
    entry.headers = HeaderPolicy::sanitizedForTarget(upstream, headers);
    entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    m_tokens.insert(token, entry);

    // 末尾的斜杠很关键：m3u8 里的相对分片会按“目录”解析，
    // 少了它 /stream/<token>/000001.ts 会被播放器解析成 /stream/000001.ts。
    return QUrl(QStringLiteral("http://127.0.0.1:%1/stream/%2/").arg(port).arg(token));
}

bool StreamTokenRegistry::lookup(const QString &token, Upstream *upstream) const
{
    const auto it = m_tokens.constFind(token);
    if (it == m_tokens.cend())
        return false;
    if (upstream != nullptr)
        *upstream = it.value();
    return true;
}

void StreamTokenRegistry::prune()
{
    if (m_tokens.size() < 512)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 expireBefore = now - 6LL * 60 * 60 * 1000;
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->createdAtMs > 0 && it->createdAtMs < expireBefore)
            it = m_tokens.erase(it);
        else
            ++it;
    }

    // 极端情况下（一个超长播放列表反复刷新）仍超过上限，丢最老的四分之一。
    if (m_tokens.size() > 4096) {
        QList<QPair<qint64, QString>> byAge;
        byAge.reserve(m_tokens.size());
        for (auto it = m_tokens.cbegin(); it != m_tokens.cend(); ++it)
            byAge.append({it->createdAtMs, it.key()});
        std::sort(byAge.begin(), byAge.end(),
                  [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                      return a.first < b.first;
                  });
        const int removeCount = m_tokens.size() / 4;
        for (int i = 0; i < removeCount && i < byAge.size(); ++i)
            m_tokens.remove(byAge.at(i).second);
    }
}
