/// @brief StreamProxy 的缓存统计、播放列表记录与条目释放实现。
/// @note 这些成员函数只操作缓存表和播放地址快照，与请求转发/预取状态机
///       没有直接耦合；单独放在一个翻译单元，避免 StreamProxy.cpp 继续膨胀。
#include "stream/StreamProxy.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QNetworkReply>
#include <QStringList>
#include <QTcpSocket>

Q_LOGGING_CATEGORY(lcProxyAdmin, "mvp.stream")
/// @brief 返回当前缓存条目数、内存/磁盘占用等统计。
StreamProxy::CacheStats StreamProxy::cacheStats() const
{
    CacheStats stats;
    stats.entries = int(m_cache.size());
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        stats.bytes += StreamCacheStore::bytes(*it.value());
    stats.activePrefetches = m_prefetchActive;
    stats.rangePrefetches = int(m_rangePrefetchKeys.size());
    return stats;
}

/// @brief 返回最近一次播放使用的影视播放列表地址。
QUrl StreamProxy::lastPlaybackVideoPlaylist() const
{
    QMutexLocker locker(&m_playbackPlaylistMutex);
    return m_lastPlaybackVideoPlaylist;
}

/// @brief 返回最近一次播放使用的音乐播放列表地址。
QUrl StreamProxy::lastPlaybackAudioPlaylist() const
{
    QMutexLocker locker(&m_playbackPlaylistMutex);
    return m_lastPlaybackAudioPlaylist;
}

/// @brief 释放指定路径的磁盘缓存占用（删除缓存文件并移除元数据）。
void StreamProxy::releaseCacheFile(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString target = QFileInfo(path).absoluteFilePath();
    QStringList keys;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
        if (!it.value().isNull() && !it.value()->filePath.isEmpty()) {
            const QString entryPath = QFileInfo(it.value()->filePath).absoluteFilePath();
            if (entryPath == target)
                keys.append(it.key());
        }
    }
    for (const QString &key : keys)
        dropCacheEntry(key);
    if (!keys.isEmpty()) {
        qCInfo(lcProxyAdmin) << "已释放流转缓存文件句柄" << target << keys.size() << "个条目";
    }
}

/// @brief 丢弃单条缓存：中止在途请求、关闭文件并移除条目。
void StreamProxy::dropCacheEntry(const QString &key)
{
    const QSharedPointer<CacheEntry> entry = m_cache.take(key);
    if (entry.isNull())
        return;

    const bool wasPrefetch = entry->prefetch;
    bool wasTrackedPrefetch = false;
    if (entry->reply != nullptr) {
        const auto prefetchIt = m_prefetchReplies.find(key);
        if (prefetchIt != m_prefetchReplies.end() && prefetchIt.value() == entry->reply) {
            m_prefetchReplies.erase(prefetchIt);
            m_prefetchStartedMs.remove(key);
            wasTrackedPrefetch = true;
        }
        abortReplySilently(entry->reply);
        entry->reply = nullptr;
    }
    for (const QPointer<QNetworkReply> &reply : std::as_const(entry->rangeReplies)) {
        if (!reply.isNull())
            abortReplySilently(reply.data());
    }
    entry->rangeReplies.clear();
    entry->rangePending.clear();
    entry->rangeParts.clear();
    entry->rangePartEnds.clear();
    for (const QPointer<QTcpSocket> &client : std::as_const(entry->clients)) {
        if (!client.isNull() && client->state() == QAbstractSocket::ConnectedState)
            client->disconnectFromHost();
    }
    entry->clients.clear();
    // 磁盘条目只关句柄并落元数据，保留文件供下次播放命中；内存条目才清正文。
    closeCacheFile(*entry);
    entry->data.clear();
    qCDebug(lcProxyAdmin) << "流转缓存淘汰" << key.left(32);
    // 预取条目被缓存淘汰时也必须归还并发槽位，否则后续 seek 预取会一直
    // 以为并发还满着，m_rangePrefetchKeys 残留还会阻止同 key 重新预取。
    if (wasPrefetch)
        finishRangePrefetch(key, entry);
    else if (wasTrackedPrefetch) {
        m_prefetchActive = qMax(0, m_prefetchActive - 1);
        startNextPrefetch();
    }
}

