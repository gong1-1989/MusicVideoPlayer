/// @file StreamCacheStore.cpp
/// @brief 流转缓存条目占用统计：实际字节与预留额度取较大者，避免并发占位突破配额。
#include "stream/StreamCacheStore.h"

/// @brief 计算单条缓存的配额占用。
/// @param entry 缓存条目。
/// @return 已缓存字节（含分片）与 reservedBytes 中的较大值。
/// @note 在途响应即使还没收到数据，也按登记的 total 预留，防止多个占位同时超配额。
qint64 StreamCacheStore::bytes(const Entry &entry)
{
    qint64 stored = entry.diskBacked ? entry.dataSize : entry.data.size();
    for (auto it = entry.rangeParts.cbegin(); it != entry.rangeParts.cend(); ++it)
        stored += it.value().size();
    // 配额按“已缓存 + 已登记未完成”算：pending 占位/在途响应即使还没收到数据，
    // 也必须先占住 total，否则多个占位可以同时突破总配额。
    return qMax(stored, qMax<qint64>(entry.reservedBytes, 0));
}
