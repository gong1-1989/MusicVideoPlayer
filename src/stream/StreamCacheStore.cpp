#include "stream/StreamCacheStore.h"

qint64 StreamCacheStore::bytes(const Entry &entry)
{
    qint64 total = entry.data.size();
    for (auto it = entry.rangeParts.cbegin(); it != entry.rangeParts.cend(); ++it)
        total += it.value().size();
    return total;
}
