/// @file SearchRanker.cpp
/// @brief 搜索结果相关性打分、音乐/影视分组与同内容优选。
#include "net/SearchRanker.h"

#include <QHash>
#include <QStringList>

#include <algorithm>

namespace {

/// 品质只作为同分时的排序依据，不能压过标题/歌手的相关性。
int qualityRank(const QString &quality)
{
    const QString text = quality.toUpper();
    if (text.contains(QLatin1String("4K")))
        return 5;
    if (text.contains(QLatin1String("1080")) || text.contains(QStringLiteral("无损"))
        || text.contains(QLatin1String("HDR")))
        return 4;
    if (text.contains(QLatin1String("720")) || text.contains(QLatin1String("HQ"))
        || text.contains(QStringLiteral("高清")))
        return 3;
    if (text.contains(QLatin1String("标清")))
        return 1;
    return 2;
}

int typeOrder(const MediaItem &item)
{
    return item.type == MediaType::Music ? 0 : 1;
}

struct ScoredItem
{
    MediaItem item;
    int score = 0;
    int order = 0;
};

bool betterThan(const ScoredItem &candidate, const ScoredItem &current)
{
    if (candidate.score != current.score)
        return candidate.score > current.score;
    const int candidateQuality = qualityRank(candidate.item.quality);
    const int currentQuality = qualityRank(current.item.quality);
    if (candidateQuality != currentQuality)
        return candidateQuality > currentQuality;
    return false;
}

} // namespace

/// @brief 归一化搜索文本，消除全半角、大小写、空白和标点差异。
QString SearchRanker::normalizeForMatch(const QString &text)
{
    QString normalized;
    normalized.reserve(text.size());
    for (const QChar ch : text.normalized(QString::NormalizationForm_KC)) {
        if (ch.isLetterOrNumber())
            normalized.append(ch.toLower());
    }
    return normalized;
}

/// @brief 计算关键词与条目的相关性分数。
/// @param item 搜索结果条目。
/// @param keyword 用户原始关键词。
/// @return 非负分数；空关键词返回 0，调用方仍可按类型/源顺序稳定排列。
int SearchRanker::relevanceScore(const MediaItem &item, const QString &keyword)
{
    const QString query = normalizeForMatch(keyword);
    if (query.isEmpty())
        return 0;

    const QString title = normalizeForMatch(item.title);
    const QString artist = normalizeForMatch(item.artist);
    const QString description =
        normalizeForMatch(item.description + QLatin1Char(' ') + item.genre
                          + QLatin1Char(' ') + item.year);

    int score = 0;
    // 标题命中权重最高：用户输入片名/歌名时，标题就是最直接的意图。
    if (title == query)
        score += 240;
    else if (title.startsWith(query))
        score += 180;
    else if (title.contains(query))
        score += 140;

    // 歌手/作者是第二意图；很多音乐结果的标题不含关键词，但歌手匹配仍然相关。
    if (!artist.isEmpty()) {
        if (artist == query)
            score += 90;
        else if (artist.contains(query))
            score += 60;
    }

    // 查询词可能包含“歌手 + 歌名”两个词；逐项在标题/作者里命中后补分，
    // 全部命中再加一次整体分，避免只命中一个常见词的结果排在前面。
    QStringList tokens = keyword.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        tokens.append(keyword);
    bool allTokensMatched = true;
    int tokenCount = 0;
    for (const QString &rawToken : std::as_const(tokens)) {
        const QString token = normalizeForMatch(rawToken);
        if (token.isEmpty() || token == query)
            continue;
        ++tokenCount;
        if (title.contains(token))
            score += 35;
        else if (artist.contains(token))
            score += 25;
        else if (description.contains(token))
            score += 8;
        else
            allTokensMatched = false;
    }
    if (tokenCount > 0 && allTokensMatched)
        score += 35;

    // 简介命中是弱信号；只在标题/歌手都没有命中时提供兜底相关性。
    if (score == 0 && !description.isEmpty() && description.contains(query))
        score += 10;

    // 试听/电台不是用户搜索完整歌曲时的首选，但不能直接过滤；
    // 降权后仍可在没有完整条目时被看到。
    if (item.streamScope == QLatin1String("preview"))
        score -= 30;
    if (item.streamScope == QLatin1String("radio"))
        score -= 40;
    return qMax(0, score);
}

/// @brief 按类型分组、组内相关性排序，并按 mediaIdentity 去重。
/// @param items 各源原始结果，顺序通常就是源配置顺序。
/// @param keyword 用户关键词。
/// @return 音乐组在前、影视组在后；组内分数高在前，同分保持原源顺序。
QVector<MediaItem> SearchRanker::rankAndDedupe(const QVector<MediaItem> &items,
                                               const QString &keyword)
{
    QVector<ScoredItem> unique;
    unique.reserve(items.size());
    QHash<QString, int> bestIndexByIdentity;
    bestIndexByIdentity.reserve(items.size());

    for (int i = 0; i < items.size(); ++i) {
        const MediaItem &item = items.at(i);
        if (!item.isValid())
            continue;
        ScoredItem candidate{item, relevanceScore(item, keyword), i};
        const QString identity = mediaIdentity(item);
        const auto existing = bestIndexByIdentity.constFind(identity);
        if (existing == bestIndexByIdentity.cend()) {
            bestIndexByIdentity.insert(identity, unique.size());
            unique.append(candidate);
            continue;
        }
        // 同一内容可能被多个源返回；保留更相关、品质更好的那条，
        // 不能像普通去重那样“先到先得”，否则首个坏源会挡住后面的好线路。
        if (betterThan(candidate, unique.at(existing.value())))
            unique[existing.value()] = candidate;
    }

    std::stable_sort(unique.begin(), unique.end(), [](const ScoredItem &a, const ScoredItem &b) {
        if (typeOrder(a.item) != typeOrder(b.item))
            return typeOrder(a.item) < typeOrder(b.item);
        if (a.score != b.score)
            return a.score > b.score;
        const int aQuality = qualityRank(a.item.quality);
        const int bQuality = qualityRank(b.item.quality);
        if (aQuality != bQuality)
            return aQuality > bQuality;
        return a.order < b.order;
    });

    QVector<MediaItem> result;
    result.reserve(unique.size());
    for (const ScoredItem &entry : std::as_const(unique))
        result.append(entry.item);
    return result;
}
