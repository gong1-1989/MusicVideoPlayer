#ifndef SEARCHRANKER_H
#define SEARCHRANKER_H

#include "media/MediaItem.h"

#include <QString>
#include <QVector>

/// 搜索结果相关性排序与同内容优选。
/// 只做纯计算，不依赖网络/UI，便于单元测试；Provider 仍然只负责“拿到什么”，
/// 排序和跨源去重统一放在协调器出口，避免每个源各排一套导致顺序不稳定。
namespace SearchRanker {

/// 计算一条结果与关键词的相关性分数。
/// 规则只使用标题、歌手、简介等已解析字段，不引入外部模型：
/// 标题完全匹配 > 前缀 > 包含；歌手命中次之；查询词拆开后逐项命中补分。
/// 试听/电台条目在音乐搜索里降权，避免完整曲目被片段压到后面。
int relevanceScore(const MediaItem &item, const QString &keyword);

/// 按“音乐组 → 影视组 → 相关性降序 → 稳定源顺序”排序，并按 mediaIdentity 去重。
/// 同 identity 保留相关性最高的一条；分数相同保留先出现的（源配置顺序）。
QVector<MediaItem> rankAndDedupe(const QVector<MediaItem> &items, const QString &keyword);

/// 归一化搜索文本：全半角统一、转小写、去掉空白和标点。
/// 只用于比较，不改变展示字段；中文按整串/子串匹配，英文按单词子串匹配。
QString normalizeForMatch(const QString &text);

} // namespace SearchRanker

#endif // SEARCHRANKER_H
