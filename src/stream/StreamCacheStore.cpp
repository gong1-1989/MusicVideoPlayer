#include "stream/StreamCacheStore.h"

qint64 StreamCacheStore::bytes(const Entry &entry)
{
    qint64 stored = entry.data.size();
    for (auto it = entry.rangeParts.cbegin(); it != entry.rangeParts.cend(); ++it)
        stored += it.value().size();
    // 配额按“已缓存 + 已登记未完成”算：pending 占位/在途响应即使还没收到数据，
    // 也必须先占住 total，否则多个占位可以同时突破总配额。
    return qMax(stored, qMax<qint64>(entry.reservedBytes, 0));
}
