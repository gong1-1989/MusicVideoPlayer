#include "cache/HlsCacheBuilder.h"

#include "cache/MediaCache.h"
#include "download/DownloadTask.h"
#include "net/HttpClient.h"
#include "util/HeaderPolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <limits>

Q_LOGGING_CATEGORY(lcHlsCache, "mvp.hls.cache")

HlsCacheBuilder::HlsCacheBuilder(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

bool HlsCacheBuilder::start(const MediaItem &item)
{
    cancel();

    // HLS 影视才需要整段缓存；直链 MP4 已有 StreamProxy 的完整文件落盘。
    if (m_http == nullptr || !item.online || item.type != MediaType::Video
        || !playbackIsHls(item) || !item.isValid()) {
        return false;
    }

    if (!MediaCache::completeFileForItem(item).isEmpty()) {
        qCDebug(lcHlsCache) << "HLS 完整缓存已存在，跳过：" << item.displayName();
        return false;
    }

    m_item = item;
    // 没命中完整缓存时可能残留上一次的半成品；先清掉再开始，避免续传点
    // 和新的播放列表错位（同一集的 m3u8 签名/分片顺序可能已变化）。
    MediaCache::removeForItem(item);
    m_bodyPath = MediaCache::filePathForItem(item);
    m_segmentsDir = QFileInfo(m_bodyPath).absolutePath() + QStringLiteral("/segments");
    if (!QDir().mkpath(m_segmentsDir))
        return false;

    m_active = true;
    m_lastPercent = -1;
    const QUrl url = preferredPlaybackUrl(item);
    m_task = DownloadTask::start(m_http, url, m_bodyPath, item.headers, this);
    if (m_task.isNull()) {
        finishFailed(tr("无法启动 HLS 缓存任务"));
        return false;
    }
    m_task->setMaxBytes(std::numeric_limits<qint64>::max());
    // 逐片原样保留到 segments 目录；DownloadTask 完成后会在 m_bodyPath
    // 生成引用这些分片的本地 m3u8，不再合并为单个 TS，也不转 MP4。
    m_task->setPreserveSegmentsDir(m_segmentsDir);
    connect(m_task, &DownloadTask::progress, this,
            [this](qint64 received, qint64 total) {
                if (!m_active || total <= 0)
                    return;
                const int percent = int(qMin<qint64>(100, received * 100 / total));
                // 每个分片都刷状态栏会压过播放状态；只在跨过 10% 时上报和记日志，
                // 让用户能看出后台缓存确实在推进。
                if (percent < 100 && percent / 10 <= m_lastPercent / 10)
                    return;
                if (percent == m_lastPercent)
                    return;
                m_lastPercent = percent;
                qCInfo(lcHlsCache) << "HLS 缓存进度：" << m_item.displayName()
                                   << percent << "%";
                emit progressChanged(m_item, percent);
            });
    // 自动缓存必须等完整 ENDLIST / VOD；不完整的直播列表按快照缓存会得到
    // 一段看似成功、实际缺结尾的视频，用户下次播放会更困惑。
    m_task->setAllowIncompletePlaylist(false);
    const QUrl origin =
        item.sourceOrigin.isValid() && !item.sourceOrigin.host().isEmpty()
            ? item.sourceOrigin
            : HeaderPolicy::originFromHeaders(item.headers);
    m_task->setOriginUrl(origin);

    connect(m_task, &DownloadTask::succeeded, this,
            &HlsCacheBuilder::onSegmentsReady);
    connect(m_task, &DownloadTask::failed, this,
            [this](const QString &message) { finishFailed(message); });

    qCInfo(lcHlsCache) << "开始后台缓存 HLS 分片：" << item.displayName()
                       << HttpClient::redactUrl(url);
    return true;
}

void HlsCacheBuilder::cancel()
{
    if (m_task != nullptr) {
        DownloadTask *task = m_task.data();
        m_task = nullptr;
        task->disconnect(this);
        task->cancel();
        task->deleteLater();
    }
    if (!m_bodyPath.isEmpty() || !m_segmentsDir.isEmpty())
        cleanupPartial(false);
    m_active = false;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_segmentsDir.clear();
}

void HlsCacheBuilder::cancelAndRemove()
{
    const MediaItem item = m_item;
    if (m_task != nullptr) {
        DownloadTask *task = m_task.data();
        m_task = nullptr;
        task->disconnect(this);
        task->cancel();
        task->deleteLater();
    }
    cleanupPartial(false);
    m_active = false;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_segmentsDir.clear();
    // 先按身份找到实际占用的正文/JSON，再整体删条目目录。不能先删 JSON：
    // 同名条目会解析到 (2)/(3)，JSON 一旦丢了就无法定位真正文。
    if (item.isValid())
        MediaCache::removeForItem(item);
}

void HlsCacheBuilder::onSegmentsReady(const QString &playlistPath)
{
    if (!m_active)
        return;
    if (!playlistPath.isEmpty())
        m_bodyPath = playlistPath;

    // DownloadTask 在 succeeded 之后还会自己 finish/deleteLater；这里只摘掉
    // 观察指针，不主动删除，免得在信号调用栈里销毁发送者。
    m_task = nullptr;

    QFileInfo playlistInfo(m_bodyPath);
    if (!playlistInfo.exists() || playlistInfo.size() <= 0) {
        finishFailed(tr("分片下载完成，但本地 m3u8 正文不存在或为空"));
        return;
    }

    const qint64 segmentBytes = totalSegmentBytes();
    if (segmentBytes <= 0) {
        finishFailed(tr("分片目录里没有有效分片文件"));
        return;
    }

    // total 必须记录“正文文件本身”的大小：MediaCache::completeFileForItem
    // 会用 state.total 和正文实际字节数比对。逐片模式下正文只是几 KB 的
    // 本地 m3u8，真实媒体大小在 segments/ 里；这里若写分片总和，完整缓存
    // 永远无法被识别，下次播放会重复缓存。
    if (!MediaCache::writeState(m_bodyPath, MediaCache::cacheIdentity(m_item),
                                preferredPlaybackUrl(m_item),
                                "application/vnd.apple.mpegurl", playlistInfo.size(),
                                true, QStringLiteral(".m3u8"))) {
        // 正文已经完整，元数据失败不能留下“看似没有缓存”的半套状态。
        QFile::remove(m_bodyPath);
        finishFailed(tr("分片已缓存，但写入缓存元数据失败"));
        return;
    }

    m_active = false;
    const MediaItem item = m_item;
    const QString cachePath = m_bodyPath;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_segmentsDir.clear();
    qCInfo(lcHlsCache) << "HLS 影视分片缓存完成：" << item.displayName()
                       << cachePath << segmentBytes << "字节分片";
    emit completed(item, cachePath);
}

qint64 HlsCacheBuilder::totalSegmentBytes() const
{
    qint64 total = 0;
    const QDir dir(m_segmentsDir);
    const QFileInfoList files =
        dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &info : files) {
        if (info.size() > 0)
            total += info.size();
    }
    return total;
}

void HlsCacheBuilder::cleanupPartial(bool removeBody)
{
    if (!m_bodyPath.isEmpty()) {
        QFile::remove(m_bodyPath);
        QFile::remove(m_bodyPath + QStringLiteral(".mvpdownload"));
        QFile::remove(m_bodyPath + QStringLiteral(".mvpremux"));
        if (removeBody)
            QFile::remove(m_bodyPath + QStringLiteral(".json"));
    }
    if (!m_segmentsDir.isEmpty())
        QDir(m_segmentsDir).removeRecursively();

    if (!m_bodyPath.isEmpty()) {
        const QString folder = QFileInfo(m_bodyPath).absolutePath();
        // 只删空目录；同目录里还有用户其他缓存时 rmdir 会失败，忽略即可。
        QDir().rmdir(folder);
    }
}

void HlsCacheBuilder::finishFailed(const QString &message)
{
    if (!m_active)
        return;
    const MediaItem item = m_item;
    cleanupPartial(false);
    m_active = false;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_segmentsDir.clear();
    qCWarning(lcHlsCache) << "HLS 影视分片缓存失败：" << item.displayName() << message;
    emit failed(item, message);
}
