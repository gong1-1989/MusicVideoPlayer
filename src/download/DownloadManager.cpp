/// @file DownloadManager.cpp
/// @brief 下载队列管理：任务持久化、并发调度、路径预留、缓存导入与失败恢复。
#include "download/DownloadManager.h"

#include "cache/MediaCache.h"
#include "cache/LyricsCache.h"
#include "download/DownloadPaths.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "net/HttpClient.h"
#include "settings/AppSettings.h"
#include "storage/StoragePaths.h"
#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <utility>

Q_LOGGING_CATEGORY(lcDownloadManager, "mvp.download.manager")

namespace {

const int kMaxCompletedJobs = 100;
/// 下载状态只是队列元数据，正常远小于播放列表；给手工编辑/异常膨胀设上限，
/// 避免启动时 readAll() 把几十 MB 内容整体读进内存。
constexpr qint64 kMaxStateFileBytes = 16LL * 1024 * 1024;
/// 批量点下载时不要把所有任务同时拉起来：HLS 单任务还可能再开多路 Range，
/// 2 路并发已经足够跑满普通带宽，也能避免大量连接和缓存同时上涨。
constexpr int kMaxConcurrentDownloads = 2;

bool sameQueueIdentity(const MediaItem &a, const MediaItem &b)
{
    // mediaIdentity 为了跨源去重会丢掉在线直链的查询串；下载队列不能只看它，
    // 否则同一站点所有 `outer/url?id=...` 歌曲都会被当成同一条任务。
    return a.type == b.type && mediaIdentity(a) == mediaIdentity(b)
           && a.title == b.title && a.artist == b.artist && a.url == b.url;
}

void backupCorruptState(const QString &statePath)
{
    const QString backup = statePath + QStringLiteral(".corrupt");
    QFile::remove(backup);
    if (!QFile::rename(statePath, backup))
        qCWarning(lcDownloadManager) << "下载状态文件损坏，且无法备份到" << backup;
    else
        qCWarning(lcDownloadManager) << "下载状态文件损坏，已备份到" << backup;
}

} // namespace

DownloadManager::DownloadManager(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_statePath(StoragePaths::dataFile(QStringLiteral("downloads.json")))
{
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(900);
    connect(m_saveTimer, &QTimer::timeout, this, &DownloadManager::saveNow);

    m_notifyTimer = new QTimer(this);
    m_notifyTimer->setSingleShot(true);
    m_notifyTimer->setInterval(280);
    connect(m_notifyTimer, &QTimer::timeout, this, [this] {
        emit jobsChanged();
    });

    m_speedTimer = new QTimer(this);
    m_speedTimer->setInterval(500);
    connect(m_speedTimer, &QTimer::timeout, this, &DownloadManager::sampleTransferSpeeds);
    m_speedTimer->start();

    m_cacheCopyTimer = new QTimer(this);
    // 缓存复制每轮只搬固定大小，主线程不会因为大文件整段复制而卡住。
    m_cacheCopyTimer->setInterval(0);
    connect(m_cacheCopyTimer, &QTimer::timeout, this, &DownloadManager::copyCacheChunks);

    m_seedTimer = new QTimer(this);
    // 播放缓存前缀同样分块复制；直接把几 GB 分片拷进暂存会卡死界面。
    m_seedTimer->setInterval(0);
    connect(m_seedTimer, &QTimer::timeout, this, &DownloadManager::copySeedChunks);

    m_publishTimer = new QTimer(this);
    // 跨盘发布同样按块搬运；直接 QFile::copy 会让界面在大文件收尾时卡住。
    m_publishTimer->setInterval(0);
    connect(m_publishTimer, &QTimer::timeout, this, &DownloadManager::copyPublishChunks);

    m_cacheCleanupTimer = new QTimer(this);
    m_cacheCleanupTimer->setInterval(10000);
    connect(m_cacheCleanupTimer, &QTimer::timeout,
            this, &DownloadManager::cleanupPendingCaches);

    load();
    // 旧版本的暂存分片落在下载目录里；先清掉遗留目录，
    // 让下载目录只剩用户可见的成品文件。
    QTimer::singleShot(0, this, &DownloadManager::cleanLegacyDownloadCaches);
    // 上次退出时播放器仍占用缓存，删除会失败；这次启动先补清理。
    QTimer::singleShot(0, this, &DownloadManager::cleanupPendingCaches);
    QTimer::singleShot(0, this, &DownloadManager::resumePending);
}

DownloadManager::~DownloadManager()
{
    // 先置退出标记，避免 stopForExit 同步发出的 finished 回调继续排队/保存。
    m_shuttingDown = true;
    // 不能边遍历 m_tasks 边让 finished 回调 remove 同一容器；先整体摘出。
    const QHash<QString, QPointer<DownloadTask>> tasks = m_tasks;
    m_tasks.clear();
    for (const QPointer<DownloadTask> &task : tasks) {
        if (!task.isNull())
            task->stopForExit();
    }
    for (auto *remux : m_remuxes)
        remux->cancel();
    m_remuxes.clear();
    m_remuxOutputs.clear();
    const QStringList seedIds = m_seedSources.keys();
    for (const QString &id : seedIds)
        cancelSeedCopy(id, false);
    const QStringList publishIds = m_publishSources.keys();
    for (const QString &id : publishIds)
        cancelPublishCopy(id, true);
    if (m_cacheCopyTimer != nullptr)
        m_cacheCopyTimer->stop();
    if (m_seedTimer != nullptr)
        m_seedTimer->stop();
    if (m_publishTimer != nullptr)
        m_publishTimer->stop();
    if (m_cacheCleanupTimer != nullptr)
        m_cacheCleanupTimer->stop();
    const QStringList copyIds = m_cacheCopySources.keys();
    for (const QString &id : copyIds)
        cancelCacheCopy(id, true);
    // 最近完成/暂停的状态可能还在 900 ms 合并窗口里；析构时兜底落盘。
    saveNow();
}

QString DownloadManager::reserveFilePath(const QString &directory,
                                         const QString &fileName) const
{
    return reserveFilePath(directory, fileName, QString());
}

QString DownloadManager::reserveFilePath(const QString &directory,
                                         const QString &fileName,
                                         const QString &ignoreId) const
{
    if (fileName.isEmpty())
        return QString();
    return DownloadPaths::uniqueFilePath(
        directory, fileName,
        [this, &ignoreId](const QString &candidate) {
            return isPathReserved(candidate, ignoreId);
        });
}

/// @brief 判断路径是否已被未完成任务预留（可忽略指定任务）。
bool DownloadManager::isPathReserved(const QString &path, const QString &ignoreId) const
{
    if (path.isEmpty())
        return false;
    // 正在转码的输出路径还没有写进 job.filePath，也必须占位，
    // 否则另一个下载可能选中同名 .mp4，转码完成时发生冲突。
    for (auto it = m_remuxOutputs.cbegin(); it != m_remuxOutputs.cend(); ++it) {
        if (!ignoreId.isEmpty() && it.key() == ignoreId)
            continue;
        if (DownloadPaths::pathsReferToSameFile(path, it.value()))
            return true;
    }
    for (const DownloadJob &job : m_jobs) {
        if (!ignoreId.isEmpty() && job.id == ignoreId)
            continue;
        // 已结束记录不再占用目标名：用户删掉最终文件后重新下载，
        // 不应该因为旧历史里还有同名记录而被迫加 (1)。
        if (job.status == DownloadJob::Status::Completed
            || job.status == DownloadJob::Status::Failed) {
            continue;
        }
        if (DownloadPaths::pathsReferToSameFile(path, job.filePath)
            || DownloadPaths::pathsReferToSameFile(path, job.workPath())
            || DownloadPaths::pathsReferToSameFile(path, job.partialPath())) {
            return true;
        }
    }
    return false;
}

/// @brief 判断路径是否被任意任务（含已完成）占用。
bool DownloadManager::isPathOwnedByAnyJob(const QString &path) const
{
    if (path.isEmpty())
        return false;
    // 延迟删除专用：Completed/Failed 也算占用，否则旧任务的 25ms 定时器
    // 可能把新任务刚完成的最终文件删掉。
    for (const DownloadJob &job : m_jobs) {
        if (DownloadPaths::pathsReferToSameFile(path, job.filePath)
            || DownloadPaths::pathsReferToSameFile(path, job.workPath())
            || DownloadPaths::pathsReferToSameFile(path, job.partialPath())) {
            return true;
        }
    }
    for (const QString &output : m_remuxOutputs) {
        if (DownloadPaths::pathsReferToSameFile(path, output))
            return true;
    }
    return false;
}

/// @brief 新建下载任务并入队。
/// @param item 媒体条目。
/// @param url 下载地址。
/// @param kind 下载类型（直链/HLS 等）。
/// @return 新任务 id；重复任务或路径冲突时返回空。
QString DownloadManager::enqueue(const MediaItem &item, const QUrl &url,
                                 const QHash<QString, QString> &headers,
                                 const QString &filePath, bool hls, qint64 maxBytes,
                                 QString *queuedPath, bool *alreadyQueued,
                                 const CacheSeed *seed)
{
    if (alreadyQueued != nullptr)
        *alreadyQueued = false;
    for (const DownloadJob &existing : m_jobs) {
        if (existing.status != DownloadJob::Status::Completed
            && existing.status != DownloadJob::Status::Failed
            && sameQueueIdentity(existing.item, item)) {
            if (queuedPath != nullptr)
                *queuedPath = existing.filePath;
            if (alreadyQueued != nullptr)
                *alreadyQueued = true;
            emit statusMessage(tr("该条目已在下载队列中"));
            return existing.id;
        }
    }

    DownloadJob job;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.item = item;
    job.url = url;
    job.headers = headers;
    // 即使调用方绕过 DownloadController，也不能让两个活动任务共用一个
    // 最终文件或同名的 .mvpdownload。
    const QFileInfo targetInfo(filePath);
    job.filePath = reserveFilePath(targetInfo.absolutePath(), targetInfo.fileName());
    if (job.filePath.isEmpty()) {
        emit statusMessage(tr("无法为下载分配唯一文件名，请检查下载目录"));
        return QString();
    }
    job.hls = hls;
    job.maxBytes = maxBytes;
    job.status = DownloadJob::Status::Paused;
    job.message = tr("等待下载");
    job.createdAt = QDateTime::currentDateTime();

    // HLS 无论音乐还是影视，最终都以 MP4 入库：分片先后落在暂存区，
    // 完成后由 FFmpeg 无损转封装；没有 FFmpeg 时才回退发布 TS。
    if (hls && !job.filePath.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
        const QFileInfo info(job.filePath);
        const QString reserved = reserveFilePath(
            info.absolutePath(), info.completeBaseName() + QStringLiteral(".mp4"));
        if (reserved.isEmpty()) {
            emit statusMessage(tr("无法为下载分配唯一文件名，请检查下载目录"));
            return QString();
        }
        job.filePath = reserved;
    }

    // 所有下载统一进暂存区：音乐/影视、直链/HLS 都不在下载目录里产生半成品，
    // 成功后才发布到 job.filePath。暂存目录按任务 id 隔离，退出后可直接续传。
    const QString stagingSuffix =
        hls ? QStringLiteral("ts") : QFileInfo(job.filePath).suffix();
    job.stagingPath = stagingPathForJob(job, stagingSuffix);
    if (seed != nullptr && seed->bytes > 0 && !seed->sourcePath.isEmpty()) {
        job.seedSourcePath = seed->sourcePath;
        job.seedBytes = seed->bytes;
        job.seedSegmentIndex = seed->segmentIndex;
        job.seedFingerprint = seed->fingerprint;
        job.seedCacheBodyPath = seed->cacheBodyPath;
        job.message = tr("准备复用播放缓存已完成分片");
    }

    m_jobs.append(job);
    if (queuedPath != nullptr)
        *queuedPath = job.filePath;
    scheduleSave();
    scheduleNotify();
    pumpQueue();
    return job.id;
}

/// @brief 把已完整落盘的播放缓存直接登记为下载完成，避免重复下载。
/// @return 导入任务的 id；缓存不可用或不归本条目时返回空。
QString DownloadManager::importFromCache(const MediaItem &item, const QString &cachePath,
                                         const QString &filePath, qint64 maxBytes,
                                         QString *queuedPath, bool *alreadyQueued)
{
    if (alreadyQueued != nullptr)
        *alreadyQueued = false;
    for (const DownloadJob &existing : m_jobs) {
        if (existing.status != DownloadJob::Status::Completed
            && existing.status != DownloadJob::Status::Failed
            && sameQueueIdentity(existing.item, item)) {
            if (queuedPath != nullptr)
                *queuedPath = existing.filePath;
            if (alreadyQueued != nullptr)
                *alreadyQueued = true;
            emit statusMessage(tr("该条目已在下载队列中"));
            return existing.id;
        }
    }
    if (cachePath.isEmpty() || !QFileInfo(cachePath).isFile()
        || QFileInfo(cachePath).size() <= 0) {
        return QString();
    }
    const qint64 sourceSize = QFileInfo(cachePath).size();
    const qint64 limit = maxBytes > 0 ? maxBytes : NetLimits::kDownloadBytes;
    if (sourceSize > limit) {
        emit statusMessage(tr("播放缓存超过单文件下载上限，改为网络下载"));
        return QString();
    }
    // 缓存复制不经过 pumpQueue，必须在入口自己检查并发，否则批量下载
    // 会同时打开任意数量的缓存文件和目标文件。
    if (activeCount() >= kMaxConcurrentDownloads) {
        emit statusMessage(tr("下载任务已满，请稍后再试"));
        return QString();
    }

    DownloadJob job;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.item = item;
    job.url = preferredPlaybackUrl(item);
    job.headers = item.headers;
    const bool hlsJob = DownloadTask::looksLikeHls(preferredPlaybackUrl(item));
    QString targetPath = filePath;
    if (hlsJob
        && !targetPath.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
        // HLS 完整播放缓存正文本身就是 MP4；不能因为源地址是 m3u8 就把
        // MP4 字节复制成 .ts 扩展名。音乐 HLS 同样按 MP4 入库。
        const QFileInfo info(targetPath);
        targetPath = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                     + QStringLiteral(".mp4");
    }
    const QFileInfo targetInfo(targetPath);
    job.filePath = reserveFilePath(targetInfo.absolutePath(), targetInfo.fileName());
    if (job.filePath.isEmpty()) {
        emit statusMessage(tr("无法为下载分配唯一文件名，请检查下载目录"));
        return QString();
    }
    // 完整缓存也要先落到应用暂存区，再统一发布到下载目录；这样下载目录里
    // 不会出现任务中间态，也能在退出后按同一套暂存状态恢复。
    const QString stagingSuffix =
        hlsJob ? QStringLiteral("mp4") : QFileInfo(job.filePath).suffix();
    job.stagingPath = stagingPathForJob(job, stagingSuffix);
    job.cacheCopySourcePath = cachePath;
    job.bytesTotal = sourceSize;
    job.bytesReceived = 0;
    job.maxBytes = limit;
    job.status = DownloadJob::Status::Downloading;
    job.message = tr("正在从播放缓存生成下载文件");
    job.createdAt = QDateTime::currentDateTime();

    m_jobs.append(job);
    if (!startCacheCopy(job.id, cachePath)) {
        const int index = indexOf(job.id);
        if (index >= 0) {
            const QString targetPath = m_jobs.at(index).filePath;
            finishJob(job.id, targetPath, false, tr("无法从播放缓存生成下载文件"));
        }
        return QString();
    }
    if (queuedPath != nullptr)
        *queuedPath = job.filePath;
    scheduleSave();
    scheduleNotify();
    return job.id;
}

/// @brief 返回当前在途（排队/下载/转码）任务数。
int DownloadManager::activeCount() const
{
    int count = 0;
    for (const DownloadJob &job : m_jobs) {
        if (job.status == DownloadJob::Status::Downloading)
            ++count;
    }
    return count;
}

/// @brief 启动时恢复上次未完成任务，并重新填充下载队列。
void DownloadManager::resumePending()
{
    if (m_shuttingDown)
        return;

    bool any = false;
    bool changed = false;
    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Paused)
            continue;
        if (job.pausedByUser) {
            // 用户主动暂停的任务不能在下次启动时偷偷续传，必须等“继续下载”。
            if (job.message.isEmpty())
                job.message = tr("已暂停，点击继续下载");
            continue;
        }
        if (job.snapshot) {
            job.status = DownloadJob::Status::Failed;
            job.message = tr("快照任务不自动续传；请重新下载");
            changed = true;
            continue;
        }
        any = true;
        job.message = tr("等待恢复上次未完成的下载");
    }
    if (changed)
        scheduleSave();
    if (any) {
        pumpQueue();
        scheduleNotify();
        scheduleSave();
    }
}

/// @brief 退出前停止全部任务，保留可续传状态并同步落盘。
void DownloadManager::shutdownForExit()
{
    m_shuttingDown = true;
    const QStringList copyIds = m_cacheCopySources.keys();
    for (const QString &id : copyIds)
        cancelCacheCopy(id, false);
    const QStringList seedIds = m_seedSources.keys();
    for (const QString &id : seedIds)
        cancelSeedCopy(id, false);
    const QStringList publishIds = m_publishSources.keys();
    for (const QString &id : publishIds)
        cancelPublishCopy(id, true);

    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Downloading)
            continue;

        job.status = DownloadJob::Status::Paused;
        if (job.committed && !job.stagingPath.isEmpty()) {
            job.message = tr("程序退出，已暂停；下次启动继续转封装或写入下载目录");
        } else if (!job.cacheCopySourcePath.isEmpty()) {
            job.message = tr("程序退出，已暂停；下次启动继续从播放缓存生成");
        } else if (!job.seedSourcePath.isEmpty()) {
            job.message = tr("程序退出，已暂停；下次启动继续复用播放缓存");
        } else {
            job.message = tr("程序退出，已暂停；下次启动会自动续传");
        }

        auto taskIt = m_tasks.find(job.id);
        if (taskIt != m_tasks.end() && !taskIt.value().isNull()) {
            DownloadTask *task = taskIt.value().data();
            m_tasks.erase(taskIt);
            task->stopForExit();
        }

        auto remuxIt = m_remuxes.find(job.id);
        if (remuxIt != m_remuxes.end()) {
            MediaRemux *remux = remuxIt.value();
            m_remuxes.erase(remuxIt);
            m_remuxOutputs.remove(job.id);
            remux->cancel();
            remux->deleteLater();
            job.message = tr("程序退出，下次启动继续转成 MP4");
        }
    }

    m_saveTimer->stop();
    m_notifyTimer->stop();
    saveNow();
}

/// @brief 删除任务及其半成品文件。
/// @return 删除成功或任务不存在返回 true。
/// @note 正在下载的任务先取消，避免文件句柄仍被占用。
bool DownloadManager::removeJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;

    DownloadJob job = m_jobs.at(index);

    // 缓存复制任务没有 DownloadTask，必须单独关句柄并清掉半成品。
    cancelCacheCopy(id, true);
    cancelSeedCopy(id, true);
    cancelPublishCopy(id, true);

    // 先断信号、从表里摘掉，再取消：cancel() 会同步发 failed/finished，
    // 虽然已 disconnect，这个顺序也能避免以后加回调时重入 m_tasks/m_jobs。
    if (QPointer<DownloadTask> task = m_tasks.take(id); !task.isNull()) {
        task->disconnect(this);
        task->cancel();
    }

    bool removingRemux = false;
    QString remuxTempPath;
    auto remuxIt = m_remuxes.find(id);
    if (remuxIt != m_remuxes.end()) {
        MediaRemux *remux = remuxIt.value();
        m_remuxes.erase(remuxIt);
        m_remuxOutputs.remove(id);
        remuxTempPath = remux->tempFilePath();
        remux->disconnect(this);
        remux->cancel();
        remux->deleteLater();
        removingRemux = true;
    }

    if (job.status != DownloadJob::Status::Completed) {
        // 只清当前任务自己的半成品。最终文件可能已经被同名任务或用户占用，
        // 这里不再按 job 记录无条件删除最终路径。
        const bool staged = !job.stagingPath.isEmpty();
        QStringList leftovers = {job.partialPath()};
        if (staged) {
            // 暂存任务只删自己的暂存文件；job.filePath 是用户下载目录里的
            // 最终目标，未发布前绝不能碰。
            leftovers.append(job.workPath());
            const QString stagingDir = job.stagingDirectory();
            if (!stagingDir.isEmpty()) {
                QTimer::singleShot(500, this, [stagingDir] {
                    QDir(stagingDir).removeRecursively();
                });
            }
        } else if (removingRemux || (job.hls && job.committed)) {
            // HLS 已完成合并、正在转 MP4 时，TS 是当前任务自己产出的最终文件；
            // 其他状态下最终文件不归本任务，不能碰。
            // 转码失败后 m_remuxes 里已经没有该任务，但仍要看 committed：
            // 否则用户删除失败记录后，任务自己产出的 TS 会永久留在下载目录。
            leftovers.append(job.filePath);
        }
        if (!remuxTempPath.isEmpty())
            leftovers.append(remuxTempPath);
        for (const QString &path : std::as_const(leftovers))
            scheduleRemoveFile(path);
    }

    m_jobs.removeAt(index);
    scheduleSave();
    scheduleNotify();
    emit statusMessage(tr("已移除下载记录"));
    // 移除活动任务后并发槽位已经释放；队列里的 Paused 任务必须马上补位。
    // 用排队调用避免从 finishJob/removeJob 栈内同步重入 startJob。
    QTimer::singleShot(0, this, &DownloadManager::pumpQueue);
    return true;
}

/// @brief 暂停当前任务：中止网络/复制/转封装，但保留暂存半成品供继续。
bool DownloadManager::pauseJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return false;
    DownloadJob &job = m_jobs[index];
    if (job.status != DownloadJob::Status::Downloading)
        return false;

    // 三种中间阶段都可能正在占用 UI 背后的 I/O：播放缓存种子复制、
    // 完整缓存复制、FFmpeg 转封装。先保留已写入部分并停掉它们。
    cancelSeedCopy(id, false);
    cancelCacheCopy(id, false);
    // 正在跨盘发布时，源文件仍在暂存区；删掉下载目录里的临时副本，
    // 继续后由 startStagedCompletion 重新发布。
    cancelPublishCopy(id, true);

    if (m_remuxes.contains(id)) {
        MediaRemux *remux = m_remuxes.take(id);
        m_remuxOutputs.remove(id);
        if (remux != nullptr) {
            remux->cancel();
            remux->deleteLater();
        }
    }

    // DownloadTask::stopForExit() 会补发最后一次 checkpoint 再关闭文件，
    // 不发 failed；直接复用它可以保证暂停后的字节数与磁盘一致。
    if (QPointer<DownloadTask> task = m_tasks.take(id); task != nullptr)
        task->stopForExit();

    job.status = DownloadJob::Status::Paused;
    job.pausedByUser = true;
    job.message = tr("已暂停，点击继续下载");
    scheduleSave();
    scheduleNotify();
    QTimer::singleShot(0, this, &DownloadManager::pumpQueue);
    return true;
}

/// @brief 继续已暂停任务；保留 bytesReceived/分片序号，不从头下载。
bool DownloadManager::resumeJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return false;
    DownloadJob &job = m_jobs[index];
    if (job.status != DownloadJob::Status::Paused)
        return false;
    job.pausedByUser = false;
    job.message = tr("等待继续下载");
    scheduleSave();
    scheduleNotify();
    pumpQueue();
    return true;
}

/// @brief 重试失败任务；HLS 无 ENDLIST 时可用 allowIncomplete 快照下载。
/// @return 已重新入队返回 true。
bool DownloadManager::retryJob(const QString &id, bool allowIncomplete)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    DownloadJob &job = m_jobs[index];
    if (job.status == DownloadJob::Status::Downloading
        || job.status == DownloadJob::Status::Completed) {
        return false;
    }
    if (allowIncomplete && !job.needsSnapshotConsent)
        return false;

    if (allowIncomplete) {
        // 只有用户在下载管理器里明确确认过，才允许无 ENDLIST 的直播/未知列表
        // 按当前分片下载；这不是普通“重试”的默认行为。
        job.allowIncomplete = true;
        job.needsSnapshotConsent = false;
    }
    if (job.snapshot) {
        // 快照列表可能在两次启动之间已经变化，旧分片序号不能复用。
        job.bytesReceived = 0;
        job.segmentIndex = 0;
        job.segmentCount = 0;
    }
    job.status = DownloadJob::Status::Paused;
    job.pausedByUser = false;
    job.message = allowIncomplete ? tr("已授权按当前分片强制下载，等待开始")
                                  : tr("等待重试");
    pumpQueue();
    scheduleSave();
    scheduleNotify();
    return true;
}

/// @brief 清除已完成/已取消任务记录（不删用户文件）。
void DownloadManager::clearCompleted()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs.at(i).status == DownloadJob::Status::Completed)
            m_jobs.removeAt(i);
    }
    scheduleSave();
    emit jobsChanged();
}

/// @brief 按 id 查任务下标，不存在返回 -1。
int DownloadManager::indexOf(const QString &id) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).id == id)
            return i;
    }
    return -1;
}

/// @brief 从磁盘加载任务列表并校正失效状态（文件丢失/路径变化）。
void DownloadManager::load()
{
    QFile file(m_statePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        if (file.exists())
            qCWarning(lcDownloadManager) << "下载状态文件打不开" << m_statePath;
        return;
    }
    if (file.size() > kMaxStateFileBytes) {
        qCWarning(lcDownloadManager) << "下载状态文件超过上限，拒绝读取" << m_statePath
                                     << file.size() << kMaxStateFileBytes;
        file.close();
        backupCorruptState(m_statePath);
        return;
    }

    const QByteArray payload = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        qCWarning(lcDownloadManager) << "下载状态文件读取失败" << m_statePath
                                     << file.errorString();
        file.close();
        backupCorruptState(m_statePath);
        return;
    }
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcDownloadManager) << "下载状态文件损坏" << parseError.errorString();
        // 临时文件还在，但续传点/历史会丢；先保留原文件，允许用户手工恢复。
        backupCorruptState(m_statePath);
        return;
    }

    // jobs 不是数组说明状态文件结构已损坏；直接 toArray() 会得到空列表，
    // 随后一次保存就会把用户所有下载记录静默覆盖掉。
    const QJsonValue jobsValue = document.object().value(QStringLiteral("jobs"));
    if (!jobsValue.isArray()) {
        qCWarning(lcDownloadManager) << "下载状态文件缺少 jobs 数组";
        backupCorruptState(m_statePath);
        return;
    }
    const QJsonArray array = jobsValue.toArray();
    QVector<DownloadJob> loaded;
    QSet<QString> seenIds;
    bool changedInLoad = false;
    for (const QJsonValue &value : array) {
        DownloadJob job;
        QString error;
        if (!DownloadJob::fromJson(value.toObject(), &job, &error)) {
            qCWarning(lcDownloadManager) << "跳过无效下载记录" << error;
            continue;
        }
        // 手工编辑状态文件可能留下重复 id，后续按 id 查找会命中不确定记录。
        if (job.id.isEmpty() || seenIds.contains(job.id)) {
            qCWarning(lcDownloadManager) << "跳过重复/空 id 下载记录" << job.id;
            continue;
        }
        seenIds.insert(job.id);
        if (job.snapshot
            && (job.status == DownloadJob::Status::Downloading
                || job.status == DownloadJob::Status::Paused)) {
            // 快照列表可能在两次启动之间变化，旧的 HLS 分片序号不能续传。
            // 不自动恢复，保留失败记录让用户重新下载或移除。
            job.status = DownloadJob::Status::Failed;
            // 已经合并出 TS、只是转码阶段退出的记录必须保留 committed，
            // 否则这颗 TS 既不能被重试接着转码，也会在用户移除记录后
            // 永久留在下载目录；未合并完成的记录 committed 本来就是 false。
            job.message = job.committed
                ? tr("快照分片已合并，转码被中断；可重试继续转码，或移除记录")
                : tr("快照任务在退出时中断，不自动续传；请重新下载");
            changedInLoad = true;
        } else if (job.status == DownloadJob::Status::Downloading) {
            job.status = DownloadJob::Status::Paused;
            job.message = tr("上次退出时暂停，正在自动续传");
            // 修正后的状态要落盘，否则下次启动仍读到 Downloading，重复做一次
            // “正在下载→暂停”的修正，状态文件长期与实际运行状态不一致。
            changedInLoad = true;
        }
        loaded.append(job);
    }

    // 状态数组按加入顺序追加，最新记录在尾部；超过上限时从头部删最早的
    // 已完成记录，而不是保留最早 100 条、把刚完成的反而裁掉。
    int completed = 0;
    for (const DownloadJob &job : std::as_const(loaded)) {
        if (job.status == DownloadJob::Status::Completed)
            ++completed;
    }
    int completedToDrop = completed - kMaxCompletedJobs;
    for (int i = 0; i < loaded.size() && completedToDrop > 0; ) {
        if (loaded.at(i).status == DownloadJob::Status::Completed) {
            loaded.removeAt(i);
            --completedToDrop;
        } else {
            ++i;
        }
    }
    m_jobs = loaded;

    // 状态文件可被手工编辑；活动任务共用同一目标路径会让两个任务互相覆盖。
    // 载入后给后者重新分配，并丢弃它旧的续传点。
    for (int i = 0; i < m_jobs.size(); ++i) {
        DownloadJob &job = m_jobs[i];
        if (job.status == DownloadJob::Status::Completed
            || job.status == DownloadJob::Status::Failed) {
            continue;
        }
        for (int j = 0; j < i; ++j) {
            const DownloadJob &previous = m_jobs.at(j);
            if (previous.status == DownloadJob::Status::Completed
                || previous.status == DownloadJob::Status::Failed) {
                continue;
            }
            if (!DownloadPaths::pathsReferToSameFile(job.filePath, previous.filePath))
                continue;
            const QFileInfo info(job.filePath);
            const QString reserved =
                reserveFilePath(info.absolutePath(), info.fileName(), job.id);
            if (!reserved.isEmpty() && reserved != job.filePath) {
                // 旧路径仍属于前面那个活动任务（本循环只和前序未完成任务比较），
                // 它的 .mvpdownload 不能删；当前任务改到新路径后从 0 开始，
                // 旧续传点留给原任务继续使用，删掉会让原任务丢失进度。
                job.filePath = reserved;
                job.bytesReceived = 0;
                job.segmentIndex = 0;
                // committed 是针对旧路径的提交标记；目标已改名，不能继续
                // 把新路径当成自己产出过文件。
                job.committed = false;
                job.message = tr("状态文件路径冲突，已重新分配目标文件");
                // 冲突修正必须落盘，否则每次启动都会重新分配一次，任务永远
                // 停在新名字上并反复丢失续传点。
                changedInLoad = true;
            }
            break;
        }
    }

    // 先迁移旧版放在下载目录里的暂存文件，再校验暂存文件是否还存在。
    // 顺序不能反：直接清理会把旧任务的续传字节一起删掉。
    migrateLegacyStagingPaths();
    reconcileStagingState();

    if (changedInLoad)
        scheduleSave();
    qCInfo(lcDownloadManager) << "已载入" << m_jobs.size() << "条下载记录";
}

/// @brief 立即同步保存任务列表（原子替换，失败保留旧文件）。
void DownloadManager::saveNow()
{
    if (m_shuttingDown)
        m_saveTimer->stop();

    QDir().mkpath(QFileInfo(m_statePath).absolutePath());

    auto buildPayload = [this]() {
        QJsonArray array;
        for (const DownloadJob &job : m_jobs)
            array.append(job.toJson());
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("jobs"), array);
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    };

    QByteArray payload = buildPayload();
    bool trimmed = false;
    // 状态文件异常膨胀时先裁掉最老的 Failed/Paused 历史，而不是整份放弃保存；
    // 否则新任务状态永远写不进去，旧记录还会一直留在磁盘上。
    while (payload.size() > kMaxStateFileBytes) {
        int victim = -1;
        for (int i = 0; i < m_jobs.size(); ++i) {
            if (m_jobs.at(i).status == DownloadJob::Status::Failed) {
                victim = i;
                break;
            }
        }
        if (victim < 0) {
            for (int i = 0; i < m_jobs.size(); ++i) {
                if (m_jobs.at(i).status == DownloadJob::Status::Paused) {
                    victim = i;
                    break;
                }
            }
        }
        if (victim < 0)
            break;
        m_jobs.removeAt(victim);
        trimmed = true;
        payload = buildPayload();
    }

    if (payload.size() > kMaxStateFileBytes) {
        qCWarning(lcDownloadManager) << "下载状态超过上限，且没有可裁剪的历史"
                                     << payload.size() << kMaxStateFileBytes;
        return;
    }

    QSaveFile file(m_statePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcDownloadManager) << "下载状态文件无法写入" << file.errorString();
        return;
    }
    const qint64 written = file.write(payload);
    if (written != payload.size()) {
        // 磁盘满/配额不足时 QSaveFile 可能短写；绝不能把截断的 JSON 提交成新状态。
        qCWarning(lcDownloadManager) << "下载状态文件写入不完整，已放弃提交"
                                     << written << payload.size() << file.errorString();
        file.cancelWriting();
        return;
    }
    if (!file.commit()) {
        qCWarning(lcDownloadManager) << "下载状态文件保存失败" << file.errorString();
        return;
    }
    if (trimmed && !m_shuttingDown)
        emit jobsChanged();
}

/// @brief 把旧版下载目录里的 `.download-cache` 暂存文件迁到应用数据目录。
/// @note 同盘 rename；跨盘才复制，并校验大小。迁移失败时保留旧路径，
///       不能为了“清干净下载目录”把用户几百 MB/几 GB 的续传进度删掉。
void DownloadManager::migrateLegacyStagingPaths()
{
    auto moveFile = [](const QString &from, const QString &to) {
        if (!QFileInfo(from).isFile())
            return true; // 没有源文件不算失败
        if (QFileInfo(to).exists() && !QFile::remove(to))
            return false;
        if (QFile::rename(from, to))
            return true;
        if (QFile::copy(from, to)
            && QFileInfo(to).size() == QFileInfo(from).size()
            && QFile::remove(from)) {
            return true;
        }
        QFile::remove(to);
        return false;
    };

    bool changed = false;
    for (DownloadJob &job : m_jobs) {
        if (job.stagingPath.isEmpty())
            continue;
        const QString path = QDir::fromNativeSeparators(job.stagingPath);
        if (!path.contains(QLatin1String("/.download-cache/")))
            continue;

        const QFileInfo oldInfo(job.stagingPath);
        if (oldInfo.absolutePath().isEmpty()) {
            job.stagingPath.clear();
            changed = true;
            continue;
        }
        const QString oldDir = oldInfo.absolutePath();
        const QString oldBase = oldInfo.filePath();
        const QString oldPartial = oldBase + QStringLiteral(".mvpdownload");
        const QString oldMp4 = oldInfo.absolutePath() + QLatin1Char('/')
                               + oldInfo.completeBaseName() + QStringLiteral(".mp4");

        const QString newBase = stagingPathForJob(job, oldInfo.suffix());
        if (newBase.isEmpty())
            continue;
        const QFileInfo newInfo(newBase);
        const QString newDir = newInfo.absolutePath();
        const QString newPartial = newBase + QStringLiteral(".mvpdownload");
        const QString newMp4 = newInfo.absolutePath() + QLatin1Char('/')
                               + newInfo.completeBaseName() + QStringLiteral(".mp4");
        if (!QDir().mkpath(newDir)) {
            qCWarning(lcDownloadManager) << "旧下载缓存迁移目录创建失败" << newDir;
            continue;
        }

        const bool hasBase = QFileInfo(oldBase).isFile();
        const bool hasPartial = QFileInfo(oldPartial).isFile();
        const bool hasMp4 = QFileInfo(oldMp4).isFile();
        if (!hasBase && !hasPartial && !hasMp4) {
            // 用户手动删掉了旧缓存：只改路径并清零，后面按全新任务下载。
            job.stagingPath = newBase;
            job.committed = false;
            job.bytesReceived = 0;
            job.segmentIndex = 0;
            job.segmentCount = 0;
            job.message = tr("旧下载缓存已被清理，将重新下载");
            qCInfo(lcDownloadManager) << "旧下载缓存不存在，已重置任务" << job.id;
            changed = true;
            continue;
        }

        bool ok = true;
        if (hasBase && !moveFile(oldBase, newBase)) {
            qCWarning(lcDownloadManager) << "旧下载缓存迁移失败" << oldBase;
            ok = false;
        }
        if (hasMp4 && !moveFile(oldMp4, newMp4)) {
            qCWarning(lcDownloadManager) << "旧转封装缓存迁移失败" << oldMp4;
            ok = false;
        }
        if (hasPartial && !hasBase && !moveFile(oldPartial, newPartial)) {
            qCWarning(lcDownloadManager) << "旧半成品缓存迁移失败" << oldPartial;
            ok = false;
        }

        if (!ok) {
            // 至少保留原 stagingPath，下次仍能从旧位置续传。
            qCWarning(lcDownloadManager) << "保留旧下载缓存路径等待下次迁移" << job.id;
            continue;
        }

        // 迁移成功：优先接已经转封装好的 MP4，其次接完整 TS/半成品。
        job.stagingPath = QFileInfo(newMp4).isFile() ? newMp4 : newBase;
        // 完整文件已经搬到新位置后，旧目录里残留的 partial 没有意义。
        if (hasBase)
            QFile::remove(oldPartial);
        QDir(oldDir).removeRecursively();
        changed = true;
        qCInfo(lcDownloadManager) << "已迁移旧下载缓存" << oldDir << "->" << newDir;
    }
    if (changed)
        scheduleSave();
}

/// @brief 暂存文件被外部删除时同步清零续传点，避免显示与实际进度脱节。
void DownloadManager::reconcileStagingState()
{
    bool changed = false;
    for (DownloadJob &job : m_jobs) {
        if (job.stagingPath.isEmpty()
            || job.status == DownloadJob::Status::Completed)
            continue;
        // 缓存种子/完整缓存复制还没开始写暂存文件时，不能把进度误判为丢失。
        if (!job.seedSourcePath.isEmpty() || !job.cacheCopySourcePath.isEmpty())
            continue;

        const QFileInfo baseInfo(job.stagingPath);
        const QString partial = job.stagingPath + QStringLiteral(".mvpdownload");
        const QString stagedMp4 = baseInfo.absolutePath() + QLatin1Char('/')
                                  + baseInfo.completeBaseName() + QStringLiteral(".mp4");
        const bool hasStaged = baseInfo.isFile() || QFileInfo(partial).isFile()
                               || QFileInfo(stagedMp4).isFile();
        if (hasStaged)
            continue;
        if (job.bytesReceived <= 0 && job.segmentIndex <= 0 && !job.committed)
            continue;

        job.committed = false;
        job.bytesReceived = 0;
        job.segmentIndex = 0;
        job.segmentCount = 0;
        job.message = tr("下载缓存已被清理，将从头下载");
        changed = true;
        qCInfo(lcDownloadManager) << "暂存文件不存在，已重置下载进度" << job.id;
    }
    if (changed)
        scheduleSave();
}

/// @brief 清掉旧版本遗留在下载目录里的 `.download-cache` 暂存目录。
/// @note 只删除本应用固定命名的子目录，不扫描/删除用户下载的文件本身。
void DownloadManager::cleanLegacyDownloadCaches()
{
    const QString root = AppSettings::downloadDir();
    if (root.trimmed().isEmpty())
        return;

    QStringList parents;
    parents << root
            << DownloadPaths::mediaDirectory(root, false)
            << DownloadPaths::mediaDirectory(root, true);
    // 队列里可能还保留着用户以前改过下载目录时的任务路径。
    for (const DownloadJob &job : m_jobs)
        parents << QFileInfo(job.filePath).absolutePath();

    // 升级前已经开始的任务，stagingPath 仍然指向下载目录里的旧暂存区；
    // 直接整棵删除会丢掉续传进度。这里只清掉没有被任何任务引用的目录，
    // 旧任务完成后会在发布阶段自行删除自己的暂存目录。
    QSet<QString> preserved;
    for (const DownloadJob &job : m_jobs) {
        if (!job.stagingPath.isEmpty())
            preserved.insert(QFileInfo(job.stagingPath).absolutePath());
    }

    parents.removeDuplicates();
    for (const QString &parent : parents) {
        const QString legacy = QDir(parent).filePath(QStringLiteral(".download-cache"));
        if (!QFileInfo::exists(legacy))
            continue;
        const QDir legacyDir(legacy);
        const QStringList children =
            legacyDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        bool removedAny = false;
        for (const QString &child : children) {
            const QString childPath = legacyDir.filePath(child);
            if (preserved.contains(QDir::cleanPath(childPath)))
                continue;
            if (!QDir(childPath).removeRecursively()) {
                qCWarning(lcDownloadManager) << "旧版下载暂存目录清理失败" << childPath;
                continue;
            }
            removedAny = true;
            qCInfo(lcDownloadManager) << "已清理旧版下载暂存目录" << childPath;
        }
        if (removedAny && legacyDir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot))
            QDir().rmdir(legacy);
    }
}

/// @brief 清理已完成任务遗留的播放缓存，避免缓存与下载各存一份。
void DownloadManager::cleanupPendingCaches()
{
    bool changed = false;
    bool pending = false;

    for (DownloadJob &job : m_jobs) {
        if (job.cleanupCachePath.isEmpty() && !job.cleanupMediaAndLyrics)
            continue;

        if (!job.cleanupCachePath.isEmpty()) {
            const QString path = job.cleanupCachePath;
            // HLS 影视的正文是无后缀 MP4，退出时可能还没有正文，只剩
            // media.ts/.mvpdownload 和续传状态。不能因为正文不存在就直接跳过，
            // 否则下载完成后这些半成品会永久留在 media-cache。
            if (MediaCache::removeCachePath(path)) {
                job.cleanupCachePath.clear();
                changed = true;
                qCInfo(lcDownloadManager) << "已删除下载对应的播放缓存" << path;
            } else {
                // Windows 上播放器持有正文句柄时删除会失败；保留精确路径，
                // 后面按条目身份补扫并配合定时重试。
                pending = true;
            }
        }

        if (job.cleanupMediaAndLyrics) {
            // 按条目身份补扫一遍：普通网络下载没有 cacheCopySourcePath /
            // seedCacheBodyPath，但播放时可能已经生成了完整或半成品缓存；
            // 只依赖“本次是否复用缓存”会漏掉这种场景。歌词缓存与播放缓存
            // 分属两个目录，必须分别删除。
            const bool mediaOk = MediaCache::removeForItem(job.item);
            const bool lyricsOk = LyricsCache::removeForItem(job.item);
            if (mediaOk && lyricsOk) {
                job.cleanupMediaAndLyrics = false;
                changed = true;
                qCInfo(lcDownloadManager) << "已清理下载条目对应的播放缓存与歌词缓存"
                                          << job.item.displayName();
            } else {
                pending = true;
            }
        }
    }

    if (changed) {
        scheduleSave();
        if (!m_shuttingDown)
            emit jobsChanged();
    }

    if (pending && !m_shuttingDown) {
        if (!m_cacheCleanupTimer->isActive())
            m_cacheCleanupTimer->start();
    } else {
        m_cacheCleanupTimer->stop();
    }
}

/// @brief 合并短时间内的多次变更，延迟一次落盘。
void DownloadManager::scheduleSave()
{
    if (!m_shuttingDown)
        m_saveTimer->start();
}

/// @brief 合并短时间内的状态变化，延迟一次界面通知。
void DownloadManager::scheduleNotify()
{
    if (!m_notifyTimer->isActive())
        m_notifyTimer->start();
}

/// @brief 每 500ms 刷新一次各任务速度；没有新字节时逐步归零。
/// @note 只处理正在下载的任务。暂停、完成、失败立即清零，避免界面继续显示
///       退出前的旧速度。
void DownloadManager::sampleTransferSpeeds()
{
    if (m_shuttingDown)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    QSet<QString> active;
    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Downloading) {
            m_speedStates.remove(job.id);
            if (job.bytesPerSecond != 0) {
                job.bytesPerSecond = 0;
                changed = true;
            }
            continue;
        }
        active.insert(job.id);
        if (noteTransferSample(job, job.bytesReceived, now))
            changed = true;
    }

    for (auto it = m_speedStates.begin(); it != m_speedStates.end();) {
        if (!active.contains(it.key()))
            it = m_speedStates.erase(it);
        else
            ++it;
    }

    if (changed)
        emit jobsChanged();
}

/// @brief 用累计字节和真实时间差估算速度，避免 HLS 整段落盘造成假尖峰。
/// @return 任务速度字段是否发生变化。
bool DownloadManager::noteTransferSample(DownloadJob &job, qint64 bytes, qint64 nowMs)
{
    SpeedState &state = m_speedStates[job.id];
    if (state.lastMs == 0 || bytes < state.lastBytes) {
        // 首次采样，或任务重置/续传点回退；只记录基线，不产生速度。
        state.lastMs = nowMs;
        state.lastBytes = bytes;
        if (job.bytesPerSecond != 0) {
            job.bytesPerSecond = 0;
            return true;
        }
        return false;
    }

    const qint64 elapsedMs = nowMs - state.lastMs;
    if (bytes == state.lastBytes) {
        // 连续 5 秒没有任何新写入，认为传输已停住；保留太久会把旧速度
        // 当成实时速度展示。
        if (elapsedMs >= 5000 && job.bytesPerSecond != 0) {
            job.bytesPerSecond = 0;
            state.lastMs = nowMs;
            return true;
        }
        return false;
    }

    // 网络 readyRead 可能在几毫秒内连续触发；不足 250ms 的采样先累积，
    // 否则速度会在小分块之间剧烈跳动。
    if (elapsedMs < 250)
        return false;

    const double instant = double(bytes - state.lastBytes) * 1000.0 / double(elapsedMs);
    const qint64 next = job.bytesPerSecond > 0
        ? qint64(double(job.bytesPerSecond) * 0.6 + instant * 0.4)
        : qint64(instant);
    state.lastMs = nowMs;
    state.lastBytes = bytes;
    if (next == job.bytesPerSecond)
        return false;
    job.bytesPerSecond = next;
    return true;
}

/// @brief 延迟删除文件（Windows 上句柄释放可能滞后，按次数重试）。
void DownloadManager::scheduleRemoveFile(const QString &path, int attempt)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    if (QFile::remove(path))
        return;
    if (attempt >= 3) {
        qCWarning(lcDownloadManager) << "移除下载任务时无法删除临时文件" << path;
        return;
    }
    // Windows 上 QFile/QNetworkReply 刚 close 时句柄可能尚未释放。
    // 把重试放到事件循环，避免在 UI 线程里 msleep 造成可见卡顿。
    QTimer::singleShot(25 * (attempt + 1), this, [this, path, attempt] {
        // 延迟窗口内用户可能移除旧任务后立刻重新下载同一目标；
        // 新任务已经占用或已经完成这个路径时都不能再按旧任务的所有权删除。
        if (isPathOwnedByAnyJob(path)) {
            qCInfo(lcDownloadManager) << "延迟删除跳过：路径已属于现有任务" << path;
            return;
        }
        scheduleRemoveFile(path, attempt + 1);
    });
}

/// @brief 在并发上限内启动排队任务。
void DownloadManager::pumpQueue()
{
    if (m_shuttingDown)
        return;

    while (activeCount() < kMaxConcurrentDownloads) {
        QString nextId;
        for (const DownloadJob &job : std::as_const(m_jobs)) {
            if (job.status != DownloadJob::Status::Paused || job.pausedByUser)
                continue;
            if (m_tasks.contains(job.id) || m_remuxes.contains(job.id))
                continue;
            nextId = job.id;
            break;
        }
        if (nextId.isEmpty())
            return;
        // 先占住“正在下载”状态，再启动；这样暂停后遗留的排队 startJob
        // 看到 Paused 会直接退出，不会再偷偷把任务拉起来。
        const int nextIndex = indexOf(nextId);
        if (nextIndex >= 0)
            m_jobs[nextIndex].status = DownloadJob::Status::Downloading;
        startJob(nextId);
        // startJob 可能因为文件已写完而立即 Completed，也可能转入 remux；
        // 继续循环直到达到并发上限为止。
    }
}

/// @brief 所有下载任务的暂存路径；统一放在应用数据目录，和下载目录分开。
/// @note 暂存目录按任务 id 隔离；发布时若跨盘不能 rename，会走异步复制，
///       避免把大文件复制卡在界面线程。
QString DownloadManager::stagingPathForJob(const DownloadJob &job,
                                           const QString &suffix) const
{
    const QFileInfo finalInfo(job.filePath);
    if (job.id.isEmpty() || finalInfo.absolutePath().isEmpty())
        return QString();
    const QString dir = StoragePaths::dataFile(QStringLiteral("download-cache"))
                        + QLatin1Char('/') + job.id;
    QString tail = suffix.trimmed();
    if (!tail.isEmpty() && !tail.startsWith(QLatin1Char('.')))
        tail.prepend(QLatin1Char('.'));
    if (tail == QLatin1String("."))
        tail.clear();
    return dir + QLatin1Char('/') + finalInfo.completeBaseName() + tail;
}

/// @brief 启动单个任务：建 DownloadTask、接进度/检查点/结果信号。
/// @note 启动失败必须落盘 Failed 并通知界面，不能只改内存状态。
void DownloadManager::startJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return;
    if (m_tasks.contains(id) || m_remuxes.contains(id))
        return;

    DownloadJob &job = m_jobs[index];
    if (job.status == DownloadJob::Status::Completed
        || job.status == DownloadJob::Status::Paused)
        return;

    // 旧状态文件里也可能留下两条指向同一目标的任务。真正启动前再校验一次，
    // 给后来者换新名字并从头下载，避免两个任务交错写同一个半成品。
    if (isPathReserved(job.filePath, job.id)) {
        const QFileInfo info(job.filePath);
        const QString reserved =
            reserveFilePath(info.absolutePath(), info.fileName(), job.id);
        if (!reserved.isEmpty() && reserved != job.filePath) {
            qCWarning(lcDownloadManager) << "下载目标与其他任务冲突，重新分配"
                                        << job.filePath << "->" << reserved;
            job.filePath = reserved;
            job.bytesReceived = 0;
            job.segmentIndex = 0;
            job.message = tr("目标文件名冲突，已重新分配并从头下载");
        }
    }

    // 影视 HLS 统一走暂存区：退出时可能停在“已下完待转封装”或“待发布”，
    // 先恢复这两个阶段，不能重新拉整段网络。
    if (!job.stagingPath.isEmpty()) {
        if (job.committed) {
            startStagedCompletion(id);
            return;
        }
        if (!job.seedSourcePath.isEmpty() && job.seedBytes > 0) {
            if (startSeedCopy(id))
                return;
            // 种子源被手动删掉时退回网络；暂存区里已复制的部分仍可由
            // DownloadTask 按记录字节截断后继续使用，不能直接清空。
            job.bytesReceived = job.seedBytes;
            job.segmentIndex = job.seedSegmentIndex;
            job.hlsFingerprint = job.seedFingerprint;
            job.seedSourcePath.clear();
            job.message = tr("播放缓存源已不存在，改为网络继续下载");
            qCWarning(lcDownloadManager) << "播放缓存种子不可用，退回网络" << id;
        }
    }

    // 上次可能已经把最终文件写完，只是状态没来得及落盘。只有本任务确实
    // 成功提交过（committed）且文件大小与记录一致时才认这个文件；否则可能
    // 是用户后来自建/替换的同名文件，不能当成自己的 TS 去转 MP4 再删掉。
    const qint64 committedSize = QFileInfo(job.filePath).size();
    if (job.committed && committedSize > 0
        && (job.bytesReceived <= 0 || committedSize == job.bytesReceived)) {
        job.bytesReceived = QFileInfo(job.filePath).size();
        if (job.hls && !job.filePath.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
            job.status = DownloadJob::Status::Downloading;
            job.message = tr("正在无损转成 MP4");
            startRemux(id);
        } else {
            finishJob(id, job.filePath, true, tr("下载完成"));
        }
        return;
    }

    // 走到这里说明 committed 记录对不上磁盘文件：文件被删、被改大小，或
    // 同名文件已被用户替换。接下来要重新下载/截断写目标路径，旧的提交标记
    // 已经失效；必须清掉，否则这次失败后用户“移除记录”会删掉不属于本任务
    // 的同名文件。
    job.committed = false;

    const QString partial(job.partialPath());
    if (job.snapshot) {
        // 快照任务每次重试都从头下载：列表没有 ENDLIST，分片序号跨次启动
        // 不再可信；openTargetFile() 会在续传点为 0 时截掉旧临时文件。
        job.bytesReceived = 0;
        job.segmentIndex = 0;
        job.segmentCount = 0;
    } else if (!QFileInfo::exists(partial)) {
        // 临时文件没了，上面的续传点也失效，必须从零开始。
        job.bytesReceived = 0;
        job.segmentIndex = 0;
    }

    // 缓存复制任务没有 HTTP 下载语义；重启后必须继续复制缓存源，
    // 否则原 URL 需要播放缓存/签名时会失败，或重复下载整段。
    if (!job.cacheCopySourcePath.isEmpty()) {
        job.status = DownloadJob::Status::Downloading;
        job.message = tr("正在从播放缓存生成下载文件");
        if (startCacheCopy(id, job.cacheCopySourcePath)) {
            scheduleSave();
            scheduleNotify();
            return;
        }
        job.status = DownloadJob::Status::Failed;
        job.message = tr("播放缓存已不存在，无法继续生成下载文件");
        scheduleSave();
        scheduleNotify();
        return;
    }

    job.status = DownloadJob::Status::Downloading;
    job.message = job.hls
        ? (job.segmentIndex > 0
           ? tr("正在校验播放缓存并继续下载")
           : tr("正在解析播放列表"))
        : tr("正在下载");

    // 暂存目录按任务 id 分离，首次启动前必须确保父目录存在，否则
    // DownloadTask 打开半成品会直接失败。
    if (!job.stagingPath.isEmpty()
        && !QDir().mkpath(QFileInfo(job.stagingPath).absolutePath())) {
        job.status = DownloadJob::Status::Failed;
        job.message = tr("无法创建下载暂存目录");
        scheduleSave();
        scheduleNotify();
        emit statusMessage(job.message);
        return;
    }

    DownloadTask *task = DownloadTask::start(m_http, job.url, job.workPath(),
                                             job.headers, this);
    if (task == nullptr) {
        job.status = DownloadJob::Status::Failed;
        job.message = tr("无法启动下载任务");
        // 任务启动失败时也要把 Failed 状态落盘并通知界面，否则会一直停在
        // “正在下载”，重启前也看不到失败原因。
        scheduleSave();
        scheduleNotify();
        emit statusMessage(job.message);
        return;
    }
    task->setMaxBytes(job.maxBytes > 0 ? job.maxBytes : NetLimits::kDownloadBytes);
    task->setAllowIncompletePlaylist(job.allowIncomplete);
    task->setResumeState(job.snapshot ? 0 : job.bytesReceived,
                         job.snapshot ? 0 : job.segmentIndex, job.hls);
    if (job.hls)
        task->setResumeFingerprint(job.hlsFingerprint);

    // 源站地址由 Provider 写入条目；旧记录没有该字段时退回 Referer/Origin。
    const QUrl origin = job.item.sourceOrigin.isValid() && !job.item.sourceOrigin.host().isEmpty()
                            ? job.item.sourceOrigin
                            : HeaderPolicy::originFromHeaders(job.headers);
    task->setOriginUrl(origin);
    task->setPathResolver([this, id](const QString &desired) {
        const QFileInfo info(desired);
        const QString reserved =
            reserveFilePath(info.absolutePath(), info.fileName(), id);
        return reserved.isEmpty() ? desired : reserved;
    });
    m_tasks.insert(id, task);

    connect(task, &DownloadTask::targetPathChanged, this,
            [this, id](const QString &path, bool hls) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                // 这里只会发生在普通链接被识别成 HLS 后改后缀。下载任务
                // 已经把旧半成品连同字节一起改到新路径，续传点和分片序号
                // 仍然有效；此处清零会让进程在下一分片检查点前崩溃时丢掉进度。
                // 真正的“换名从头下载”由 startJob 的冲突分支显式清零。
                if (j.stagingPath.isEmpty())
                    j.filePath = path;
                else
                    j.stagingPath = path;
                j.hls = hls;
                scheduleSave();
                scheduleNotify();
            });

    connect(task, &DownloadTask::snapshotAccepted, this,
            [this, id](const QString &reason) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                j.snapshot = true;
                j.needsSnapshotConsent = false;
                j.message = reason;
                scheduleSave();
                scheduleNotify();
            });

    connect(task, &DownloadTask::snapshotConsentRequired, this,
            [this, id](const QString &) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                j.needsSnapshotConsent = true;
                scheduleSave();
                scheduleNotify();
            });

    connect(task, &DownloadTask::playlistFingerprint, this,
            [this, id](const QString &fingerprint) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                m_jobs[i].hlsFingerprint = fingerprint;
                scheduleSave();
            });

    connect(task, &DownloadTask::progress, this,
            [this, id](qint64 received, qint64 total) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                if (j.hls) {
                    j.segmentIndex = int(received);
                    j.segmentCount = int(total);
                    j.message = j.snapshot
                        ? tr("正在下载 %1/%2 分片（快照，可能不完整）")
                              .arg(received)
                              .arg(total)
                        : tr("正在下载 %1/%2 分片").arg(received).arg(total);
                } else {
                    j.bytesReceived = received;
                    j.bytesTotal = total;
                    j.message = tr("正在下载");
                }
                noteTransferSample(j, j.bytesReceived,
                                   QDateTime::currentMSecsSinceEpoch());
                scheduleNotify();
            });

    connect(task, &DownloadTask::checkpoint, this,
            [this, id](qint64 bytes, int segmentIndex, int segmentCount) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                j.bytesReceived = bytes;
                if (segmentCount >= 0) {
                    j.segmentIndex = segmentIndex;
                    j.segmentCount = segmentCount;
                }
                scheduleSave();
            });

    connect(task, &DownloadTask::succeeded, this,
            [this, id](const QString &path) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                const qint64 downloadedBytes = QFileInfo(path).size();
                const bool staged = !j.stagingPath.isEmpty();
                if (staged) {
                    j.stagingPath = path;
                    j.bytesReceived = downloadedBytes;
                    j.bytesTotal = downloadedBytes;
                    // 暂存文件已经完整；发布可能是跨盘异步复制，期间退出
                    // 必须能按 committed 直接重新发布，不能重新拉网络。
                    j.committed = true;
                    if (j.hls) {
                        if (path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
                            // fMP4 HLS 合并结果已经是 MP4；直接发布到下载目录。
                            QString published;
                            if (publishStagedFile(id, path, &published))
                                finishJob(id, published, true, tr("下载完成（MP4）"));
                            else
                                finishJob(id, j.filePath, false,
                                          tr("缓存已完成，但无法写入下载目录"));
                            return;
                        }
                        // 转封装期间退出可直接恢复，不必重新下载。
                        j.status = DownloadJob::Status::Downloading;
                        j.message = j.snapshot
                            ? tr("正在无损转成 MP4（当前分片快照）")
                            : tr("正在无损转成 MP4");
                        scheduleSave();
                        startRemux(id);
                        return;
                    }
                    QString published;
                    if (publishStagedFile(id, path, &published))
                        finishJob(id, published, true, tr("下载完成"));
                    else
                        finishJob(id, j.filePath, false,
                                  tr("缓存已完成，但无法写入下载目录"));
                    return;
                }

                j.filePath = path;
                j.bytesReceived = downloadedBytes;
                j.bytesTotal = j.bytesReceived;
                if (j.hls) {
                    if (path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
                        // fMP4 HLS 合并结果已经是 MP4，再转一次只会多一次无意义拷贝。
                        finishJob(id, path, true, tr("下载完成（MP4）"));
                        return;
                    }
                    // 进入 remux 前先把自己产出的 TS 记成 committed；
                    // 否则转码期间退出，重启会误判为“还没下载”，重新拉整段 HLS。
                    j.committed = true;
                    j.status = DownloadJob::Status::Downloading;
                    j.message = j.snapshot
                        ? tr("正在无损转成 MP4（当前分片快照）")
                        : tr("正在无损转成 MP4");
                    scheduleSave();
                    startRemux(id);
                } else {
                    finishJob(id, path, true, tr("下载完成"));
                }
            });

    connect(task, &DownloadTask::failed, this,
            [this, id](const QString &message) {
                const int i = indexOf(id);
                if (i < 0 || m_shuttingDown)
                    return;
                DownloadJob &j = m_jobs[i];
                j.status = DownloadJob::Status::Failed;
                j.message = message;
                scheduleSave();
                scheduleNotify();
                emit statusMessage(tr("下载失败：%1").arg(message));
                pumpQueue();
            });

    connect(task, &DownloadTask::finished, this, [this, id] {
        m_tasks.remove(id);
        scheduleSave();
        scheduleNotify();
    });

    scheduleSave();
    scheduleNotify();
}

/// @brief HLS 合并完成后启动 TS→MP4 转封装。
/// @note 暂存任务的转封装也在暂存目录完成，成功后才移入下载目录；失败时
///       仍把有效 TS 发布出去，保持原有“TS 兜底”的用户预期。
void DownloadManager::startRemux(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return;
    DownloadJob &job = m_jobs[index];
    if (m_remuxes.contains(id))
        return;

    const bool staged = !job.stagingPath.isEmpty();
    const QString source = job.workPath();

    if (!MediaRemux::isAvailable()) {
        if (staged) {
            QString published;
            if (publishStagedFile(id, source, &published))
                finishJob(id, published, true,
                          tr("下载完成（TS）：没有找到随程序安装的 FFmpeg"));
            else
                finishJob(id, job.filePath, false,
                          tr("无法把缓存文件写入下载目录"));
        } else {
            finishJob(id, source, true,
                      tr("下载完成（TS）：没有找到随程序安装的 FFmpeg"));
        }
        return;
    }
    // 失败记录重试时源 TS 可能已被用户手动删掉；不再拉起 FFmpeg 空跑，
    // 直接给出可理解的失败原因。
    if (!QFileInfo::exists(source)) {
        finishJob(id, staged ? job.filePath : source, false,
                  tr("无法继续转 MP4：源 TS 文件已不存在"));
        return;
    }

    const QFileInfo tsInfo(source);
    QString output;
    if (staged) {
        output = tsInfo.absolutePath() + QLatin1Char('/')
                 + tsInfo.completeBaseName() + QStringLiteral(".mp4");
        // MediaRemux 拒绝覆盖已有目标；上次退出留下的半成品 .mvpremux
        // 由它自己清，完整 MP4 只会在发布前存在，这里允许重试重新生成。
        QFile::remove(output);
    } else {
        output = reserveFilePath(tsInfo.absolutePath(),
                                 tsInfo.completeBaseName() + QStringLiteral(".mp4"),
                                 job.id);
    }
    if (output.isEmpty()) {
        if (staged) {
            QString published;
            if (publishStagedFile(id, source, &published))
                finishJob(id, published, true,
                          tr("下载完成（TS）：无法分配 MP4 输出文件名"));
            else
                finishJob(id, job.filePath, false,
                          tr("无法把缓存文件写入下载目录"));
        } else {
            finishJob(id, source, true,
                      tr("下载完成（TS）：无法分配 MP4 输出文件名"));
        }
        return;
    }
    // 从失败记录重试时，retryJob 只把状态置为“等待重试”；转码可能持续数分钟，
    // 必须切到“转码中”并落盘通知，否则下载管理器会一直停在等待状态。
    job.status = DownloadJob::Status::Downloading;
    job.message = job.snapshot ? tr("正在无损转成 MP4（当前分片快照）")
                               : tr("正在无损转成 MP4");
    scheduleSave();
    scheduleNotify();
    m_remuxOutputs.insert(id, output);
    auto *remux = new MediaRemux(this);
    m_remuxes.insert(id, remux);
    connect(remux, &MediaRemux::finished, this,
            [this, id, remux, output](bool success, const QString &actual,
                                      const QString &error) {
                m_remuxes.remove(id);
                m_remuxOutputs.remove(id);
                remux->deleteLater();
                const int i = indexOf(id);
                if (i < 0 || m_shuttingDown)
                    return;
                DownloadJob &j = m_jobs[i];
                const bool stagedJob = !j.stagingPath.isEmpty();
                if (success && QFileInfo(actual).exists()) {
                    if (stagedJob) {
                        QString published;
                        if (publishStagedFile(id, actual, &published))
                            finishJob(id, published, true, tr("下载完成（MP4）"));
                        else
                            finishJob(id, j.filePath, false,
                                      tr("MP4 已生成，但无法写入下载目录"));
                        return;
                    }
                    // 只有文件仍与下载记录一致时才删除源 TS；用户可能已经把
                    // 同名 TS 换成自己的文件，不能替用户删。
                    if (QFileInfo(j.filePath).size() == j.bytesReceived)
                        QFile::remove(j.filePath);
                    finishJob(id, actual, true, tr("下载完成（MP4）"));
                } else {
                    const QString fallbackMessage =
                        tr("下载完成（TS）：转 MP4 失败%1")
                            .arg(error.isEmpty() ? QString()
                                                 : QStringLiteral("（") + error
                                                       + QStringLiteral("）"));
                    if (stagedJob) {
                        QString published;
                        if (publishStagedFile(id, j.workPath(), &published))
                            finishJob(id, published, true, fallbackMessage);
                        else
                            finishJob(id, j.filePath, false,
                                      tr("转 MP4 失败，且无法把 TS 写入下载目录"));
                        return;
                    }
                    finishJob(id, j.filePath, true, fallbackMessage);
                }
            });
    remux->remuxToMp4(source, output);
}

/// @brief 恢复“暂存区文件已完整、只差转封装或发布”的影视任务。
void DownloadManager::startStagedCompletion(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return;
    DownloadJob &job = m_jobs[index];
    const QString work = job.stagingPath;
    if (work.isEmpty())
        return;

    if (work.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)
        && QFileInfo(work).size() > 0) {
        QString published;
        if (publishStagedFile(id, work, &published))
            finishJob(id, published, true, tr("下载完成（MP4）"));
        else
            finishJob(id, job.filePath, false, tr("无法把缓存文件写入下载目录"));
        return;
    }

    // 上次转封装可能已经成功，但退出发生在发布前；暂存目录里会有
    // `<base>.mp4`。先发布它，不能重新拉一遍网络。
    const QFileInfo workInfo(work);
    const QString stagedMp4 = workInfo.absolutePath() + QLatin1Char('/')
                              + workInfo.completeBaseName() + QStringLiteral(".mp4");
    if (QFileInfo(stagedMp4).size() > 0) {
        QString published;
        if (publishStagedFile(id, stagedMp4, &published))
            finishJob(id, published, true, tr("下载完成（MP4）"));
        else
            finishJob(id, job.filePath, false, tr("无法把缓存文件写入下载目录"));
        return;
    }

    // 普通直链（音乐/影视）下载到暂存区后不需要转封装，完整即发布。
    if (!job.hls) {
        if (QFileInfo(work).size() > 0) {
            QString published;
            if (publishStagedFile(id, work, &published))
                finishJob(id, published, true, tr("下载完成"));
            else
                finishJob(id, job.filePath, false, tr("无法把下载文件写入下载目录"));
            return;
        }
    } else if (QFileInfo(work).size() > 0) {
        job.status = DownloadJob::Status::Downloading;
        startRemux(id);
        return;
    }

    // 发布已经完成、只是状态没落盘：最终文件存在就按完成处理。
    if (QFileInfo(job.filePath).size() > 0) {
        finishJob(id, job.filePath, true, tr("下载完成"));
        return;
    }

    // 暂存文件被外部清理，只能重新下载。清掉旧字节/分片，避免从错误的
    // 分片序号继续。
    job.committed = false;
    job.bytesReceived = 0;
    job.segmentIndex = 0;
    job.segmentCount = 0;
    job.message = tr("缓存暂存文件已不存在，重新下载");
    scheduleSave();
    QTimer::singleShot(0, this, [this, id] {
        const int i = indexOf(id);
        if (i >= 0 && !m_shuttingDown)
            m_jobs[i].status = DownloadJob::Status::Paused;
        pumpQueue();
    });
}

/// @brief 把暂存区里的完整文件移动到下载目录，成功后清理该任务暂存目录。
bool DownloadManager::publishStagedFile(const QString &id, const QString &sourcePath,
                                         QString *publishedPath)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    DownloadJob &job = m_jobs[index];
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile() || sourceInfo.size() <= 0)
        return false;

    const QFileInfo finalInfo(job.filePath);
    if (finalInfo.absolutePath().isEmpty())
        return false;
    QString suffix = sourceInfo.suffix().toLower();
    if (suffix.isEmpty())
        suffix = QStringLiteral("bin");
    const QString desiredName = finalInfo.completeBaseName() + QLatin1Char('.') + suffix;
    QString finalPath = reserveFilePath(finalInfo.absolutePath(), desiredName, id);
    if (finalPath.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(finalPath).absolutePath());
    bool moved = QFile::rename(sourcePath, finalPath);
    if (!moved) {
        // 暂存区在应用数据目录，下载目录可能在另一个盘；跨盘不能 rename，
        // 改为分块复制到下载目录的 .mvpdownload，再原子改名。大文件复制
        // 期间 finishJob 会暂缓收尾，避免界面线程被一次性 QFile::copy 卡住。
        if (startPublishCopy(id, sourcePath, finalPath)) {
            job.filePath = finalPath;
            if (publishedPath != nullptr)
                *publishedPath = finalPath;
            return true;
        }
        return false;
    }
    if (QFileInfo(finalPath).size() != sourceInfo.size()) {
        QFile::remove(finalPath);
        return false;
    }

    job.filePath = finalPath;
    const QString stagingDir = sourceInfo.absolutePath();
    if (!stagingDir.isEmpty() && QFileInfo(stagingDir).fileName() == job.id)
        QDir(stagingDir).removeRecursively();
    job.stagingPath.clear();
    if (publishedPath != nullptr)
        *publishedPath = finalPath;
    return true;
}

/// @brief 跨盘发布：把暂存文件分块复制到下载目录的临时文件，完成后改名。
bool DownloadManager::startPublishCopy(const QString &id, const QString &sourcePath,
                                       const QString &finalPath)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return false;
    DownloadJob &job = m_jobs[index];
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile() || sourceInfo.size() <= 0)
        return false;

    const QString targetPath = finalPath + QStringLiteral(".mvpdownload");
    cancelPublishCopy(id, true);
    QFile *source = new QFile(sourcePath, this);
    QFile *target = new QFile(targetPath, this);
    if (!source->open(QIODevice::ReadOnly)) {
        source->deleteLater();
        target->deleteLater();
        return false;
    }
    QFile::remove(targetPath);
    if (!target->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        source->close();
        source->deleteLater();
        target->deleteLater();
        return false;
    }

    m_publishSources.insert(id, source);
    m_publishTargets.insert(id, target);
    m_publishFinalPaths.insert(id, finalPath);
    m_publishStagingDirs.insert(id, sourceInfo.absolutePath());
    job.filePath = finalPath;
    job.status = DownloadJob::Status::Downloading;
    job.message = tr("正在写入下载目录（%1）")
                      .arg(sourceInfo.size() / (1024.0 * 1024.0), 0, 'f', 1);
    scheduleSave();
    scheduleNotify();
    if (!m_publishTimer->isActive())
        m_publishTimer->start();
    return true;
}

/// @brief 分块搬运跨盘发布文件；每轮最多 4 MB，完成后再通知 finishJob。
void DownloadManager::copyPublishChunks()
{
    if (m_shuttingDown) {
        m_publishTimer->stop();
        return;
    }
    if (m_publishSources.isEmpty()) {
        m_publishTimer->stop();
        return;
    }

    constexpr qint64 kChunk = 4 * 1024 * 1024;
    const QStringList ids = m_publishSources.keys();
    bool changed = false;
    for (const QString &id : ids) {
        QFile *source = m_publishSources.value(id);
        QFile *target = m_publishTargets.value(id);
        const int index = indexOf(id);
        if (source == nullptr || target == nullptr || index < 0) {
            cancelPublishCopy(id, true);
            continue;
        }
        const QByteArray chunk = source->read(kChunk);
        if (chunk.isEmpty()) {
            if (source->error() != QFileDevice::NoError
                || target->error() != QFileDevice::NoError) {
                const QString error = source->error() != QFileDevice::NoError
                                          ? source->errorString()
                                          : target->errorString();
                const QString finalPath = m_publishFinalPaths.value(id);
                cancelPublishCopy(id, true);
                finishJob(id, finalPath, false,
                          tr("写入下载目录失败：%1").arg(error));
                continue;
            }
            const QString targetPath = target->fileName();
            const QString finalPath = m_publishFinalPaths.value(id);
            const QString sourcePath = source->fileName();
            const QString stagingDir = m_publishStagingDirs.value(id);
            const QString pendingMessage = m_pendingPublishMessages.take(id);
            if (!target->flush()) {
                const QString error = target->errorString();
                cancelPublishCopy(id, true);
                finishJob(id, finalPath, false,
                          tr("刷新下载文件失败：%1").arg(error));
                continue;
            }
            target->close();
            source->close();
            cancelPublishCopy(id, false);

            if (QFileInfo(targetPath).size() != QFileInfo(sourcePath).size()
                || !QFile::rename(targetPath, finalPath)) {
                QFile::remove(targetPath);
                finishJob(id, finalPath, false, tr("无法写入下载目录"));
                continue;
            }
            QFile::remove(sourcePath);
            if (!stagingDir.isEmpty())
                QDir(stagingDir).removeRecursively();
            DownloadJob &job = m_jobs[index];
            job.stagingPath.clear();
            job.filePath = finalPath;
            finishJob(id, finalPath, true,
                      pendingMessage.isEmpty() ? tr("下载完成") : pendingMessage);
            changed = true;
            continue;
        }
        if (target->write(chunk) != chunk.size()) {
            const QString error = target->errorString();
            const QString finalPath = m_publishFinalPaths.value(id);
            cancelPublishCopy(id, true);
            finishJob(id, finalPath, false, tr("写入下载目录失败：%1").arg(error));
            continue;
        }
        changed = true;
    }
    if (changed) {
        scheduleSave();
        scheduleNotify();
    }
    if (m_publishSources.isEmpty())
        m_publishTimer->stop();
}

/// @brief 停止跨盘发布；removePartial 控制是否删下载目录里的临时副本。
void DownloadManager::cancelPublishCopy(const QString &id, bool removePartial)
{
    QFile *source = m_publishSources.take(id);
    QFile *target = m_publishTargets.take(id);
    const QString targetPath = target != nullptr ? target->fileName() : QString();
    m_publishFinalPaths.remove(id);
    m_publishStagingDirs.remove(id);
    m_pendingPublishMessages.remove(id);
    if (source != nullptr) {
        source->close();
        source->deleteLater();
    }
    if (target != nullptr) {
        target->close();
        target->deleteLater();
    }
    if (removePartial && !targetPath.isEmpty())
        QFile::remove(targetPath);
    if (m_publishSources.isEmpty() && m_publishTimer != nullptr)
        m_publishTimer->stop();
}

/// @brief 把播放缓存前缀分块复制到暂存区半成品，完成后再走网络 DownloadTask。
bool DownloadManager::startSeedCopy(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return false;
    DownloadJob &job = m_jobs[index];
    if (job.seedSourcePath.isEmpty() || job.seedBytes <= 0 || job.stagingPath.isEmpty())
        return false;

    QFileInfo sourceInfo(job.seedSourcePath);
    if (!sourceInfo.isFile() || sourceInfo.size() < job.seedBytes)
        return false;
    if (!QDir().mkpath(QFileInfo(job.stagingPath).absolutePath()))
        return false;

    cancelSeedCopy(id, true);
    QFile *source = new QFile(job.seedSourcePath, this);
    QFile *target = new QFile(job.partialPath(), this);
    if (!source->open(QIODevice::ReadOnly)) {
        source->deleteLater();
        target->deleteLater();
        return false;
    }
    QFile::remove(job.partialPath());
    if (!target->open(QIODevice::WriteOnly)) {
        source->close();
        source->deleteLater();
        target->deleteLater();
        return false;
    }

    m_seedSources.insert(id, source);
    m_seedTargets.insert(id, target);
    m_seedRemaining.insert(id, job.seedBytes);
    job.bytesReceived = 0;
    job.status = DownloadJob::Status::Downloading;
    job.message = job.hls
        ? tr("正在复用播放缓存已完成分片（%1 MB）")
              .arg(job.seedBytes / (1024.0 * 1024.0), 0, 'f', 1)
        : tr("正在复用播放缓存已下载部分（%1 MB）")
              .arg(job.seedBytes / (1024.0 * 1024.0), 0, 'f', 1);
    scheduleSave();
    scheduleNotify();
    if (!m_seedTimer->isActive())
        m_seedTimer->start();
    return true;
}

/// @brief 分块复制种子，单轮最多 4 MB，避免大缓存把 UI 卡住。
void DownloadManager::copySeedChunks()
{
    if (m_shuttingDown) {
        m_seedTimer->stop();
        return;
    }
    if (m_seedSources.isEmpty()) {
        m_seedTimer->stop();
        return;
    }

    constexpr qint64 kChunk = 4 * 1024 * 1024;
    const QStringList ids = m_seedSources.keys();
    QStringList finished;
    QStringList failed;
    for (const QString &id : ids) {
        QFile *source = m_seedSources.value(id);
        QFile *target = m_seedTargets.value(id);
        qint64 &remaining = m_seedRemaining[id];
        if (source == nullptr || target == nullptr || !source->isOpen()
            || !target->isOpen()) {
            failed.append(id);
            continue;
        }
        const qint64 want = qMin(kChunk, remaining);
        const QByteArray buffer = source->read(want);
        if (buffer.isEmpty() || buffer.size() != want
            || target->write(buffer) != buffer.size()) {
            failed.append(id);
            continue;
        }
        remaining -= buffer.size();
        const int jobIndex = indexOf(id);
        if (jobIndex >= 0)
            m_jobs[jobIndex].bytesReceived += buffer.size();
        if (remaining <= 0)
            finished.append(id);
    }

    for (const QString &id : failed) {
        const int jobIndex = indexOf(id);
        cancelSeedCopy(id, true);
        if (jobIndex >= 0) {
            DownloadJob &job = m_jobs[jobIndex];
            job.seedSourcePath.clear();
            job.seedBytes = 0;
            job.bytesReceived = 0;
            job.segmentIndex = 0;
            job.message = tr("播放缓存复制失败，改为网络下载");
            scheduleSave();
            scheduleNotify();
        }
        QTimer::singleShot(0, this, [this, id] {
            if (!m_shuttingDown)
                startJob(id);
        });
    }

    for (const QString &id : finished) {
        const int jobIndex = indexOf(id);
        QFile *source = m_seedSources.take(id);
        QFile *target = m_seedTargets.take(id);
        m_seedRemaining.remove(id);
        if (source != nullptr) {
            source->close();
            source->deleteLater();
        }
        if (target != nullptr) {
            target->flush();
            target->close();
            target->deleteLater();
        }
        if (jobIndex >= 0) {
            DownloadJob &job = m_jobs[jobIndex];
            job.bytesReceived = job.seedBytes;
            job.segmentIndex = job.seedSegmentIndex;
            job.hlsFingerprint = job.seedFingerprint;
            job.message = job.hls
                ? tr("已复用播放缓存 %1 个分片，继续网络下载")
                      .arg(job.segmentIndex)
                : tr("已复用播放缓存已下载部分，继续网络下载");
            // 种子已落到暂存区，后续重启直接用暂存字节和指纹续传，
            // 不再重复复制。播放缓存正文路径保留到成功后再清理。
            job.seedSourcePath.clear();
            job.seedBytes = 0;
            job.seedSegmentIndex = 0;
            scheduleSave();
            scheduleNotify();
        }
        QTimer::singleShot(0, this, [this, id] {
            if (!m_shuttingDown)
                startJob(id);
        });
    }

    if (m_seedSources.isEmpty())
        m_seedTimer->stop();
}

/// @brief 停止种子复制；removePartial 控制是否删除暂存半成品。
void DownloadManager::cancelSeedCopy(const QString &id, bool removePartial)
{
    QFile *source = m_seedSources.take(id);
    QFile *target = m_seedTargets.take(id);
    m_seedRemaining.remove(id);
    if (source != nullptr) {
        source->close();
        source->deleteLater();
    }
    if (target != nullptr) {
        target->flush();
        target->close();
        target->deleteLater();
    }
    if (removePartial) {
        const int index = indexOf(id);
        if (index >= 0)
            QFile::remove(m_jobs[index].partialPath());
    }
    if (m_seedSources.isEmpty() && m_seedTimer != nullptr)
        m_seedTimer->stop();
}

/// @brief 把播放缓存拷贝到下载目录，作为已完成文件入库。
/// @return 已开始拷贝返回 true。
bool DownloadManager::startCacheCopy(const QString &id, const QString &sourcePath)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    DownloadJob &job = m_jobs[index];

    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile() || sourceInfo.size() <= 0) {
        emit statusMessage(tr("播放缓存已不存在：%1").arg(sourcePath));
        return false;
    }
    const qint64 limit = job.maxBytes > 0 ? job.maxBytes : NetLimits::kDownloadBytes;
    if (sourceInfo.size() > limit) {
        emit statusMessage(tr("播放缓存超过单文件下载上限"));
        return false;
    }

    const QString partial = job.partialPath();
    if (!QDir().mkpath(QFileInfo(partial).absolutePath())) {
        emit statusMessage(tr("无法创建下载暂存目录：%1")
                               .arg(QFileInfo(partial).absolutePath()));
        return false;
    }
    qint64 offset = QFileInfo(partial).size();
    if (offset > sourceInfo.size()) {
        // 状态文件里的续传字节不可信时从头复制。
        QFile::remove(partial);
        offset = 0;
    }

    QFile *source = new QFile(sourcePath, this);
    if (!source->open(QIODevice::ReadOnly) || !source->seek(offset)) {
        const QString error = source->errorString();
        source->close();
        source->deleteLater();
        emit statusMessage(tr("无法读取播放缓存：%1").arg(error));
        return false;
    }

    QFile *target = new QFile(partial, this);
    const QIODevice::OpenMode mode = offset > 0
        ? (QIODevice::ReadWrite)
        : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!target->open(mode) || !target->resize(offset) || !target->seek(offset)) {
        const QString error = target->errorString();
        target->close();
        target->deleteLater();
        source->close();
        source->deleteLater();
        emit statusMessage(tr("无法续写下载文件：%1").arg(error));
        return false;
    }

    job.bytesReceived = offset;
    job.bytesTotal = sourceInfo.size();
    job.status = DownloadJob::Status::Downloading;
    job.message = tr("正在从播放缓存生成下载文件");
    m_cacheCopySources.insert(id, source);
    m_cacheCopyTargets.insert(id, target);
    m_cacheCopyTimer->start();
    return true;
}

/// @brief 分块拷贝缓存文件，避免一次性读入大文件阻塞界面。
void DownloadManager::copyCacheChunks()
{
    if (m_cacheCopySources.isEmpty()) {
        m_cacheCopyTimer->stop();
        return;
    }

    const QStringList ids = m_cacheCopySources.keys();
    bool changed = false;
    for (const QString &id : ids) {
        QFile *source = m_cacheCopySources.value(id);
        QFile *target = m_cacheCopyTargets.value(id);
        const int index = indexOf(id);
        if (source == nullptr || target == nullptr || index < 0) {
            cancelCacheCopy(id, true);
            continue;
        }

        // 每轮 4MB：一首歌 1-2 轮，一部电影分几十轮，期间 UI 仍可响应。
        const QByteArray chunk = source->read(4LL * 1024 * 1024);
        if (chunk.isEmpty()) {
            if (source->error() != QFileDevice::NoError || target->error() != QFileDevice::NoError) {
                const QString error = source->error() != QFileDevice::NoError
                                          ? source->errorString()
                                          : target->errorString();
                const QString targetPath = m_jobs.at(index).filePath;
                cancelCacheCopy(id, true);
                finishJob(id, targetPath, false, tr("读取播放缓存失败：%1").arg(error));
                continue;
            }
            const QString partial = target->fileName();
            DownloadJob &job = m_jobs[index];
            if (!target->flush()) {
                const QString error = target->errorString();
                target->close();
                source->close();
                cancelCacheCopy(id, true);
                finishJob(id, job.filePath, false, tr("刷新下载文件失败：%1").arg(error));
                continue;
            }
            target->close();
            source->close();
            cancelCacheCopy(id, false);

            if (!job.stagingPath.isEmpty()) {
                // 影视 HLS 的完整播放缓存先落到暂存区，再统一发布到下载目录；
                // 中途退出时下载目录里不会出现 .mvpdownload 半成品。
                const QString stagedPath = job.stagingPath;
                if (!QFile::rename(partial, stagedPath)) {
                    QFile::remove(partial);
                    finishJob(id, job.filePath, false,
                              tr("缓存复制完成，但无法写入暂存文件"));
                    continue;
                }
                QString published;
                if (publishStagedFile(id, stagedPath, &published))
                    finishJob(id, published, true, tr("下载完成（来自播放缓存）"));
                else
                    finishJob(id, job.filePath, false,
                              tr("缓存复制完成，但无法写入下载目录"));
                changed = true;
                continue;
            }

            QString finalPath = job.filePath;
            // 复制期间用户或其他任务可能占用了目标名；先重新分配唯一名，
            // 不能因为一次 rename 冲突就把已经复制好的数据删除。
            if (QFileInfo::exists(finalPath) || isPathReserved(finalPath, job.id)) {
                const QFileInfo info(finalPath);
                const QString reserved =
                    reserveFilePath(info.absolutePath(), info.fileName(), job.id);
                if (!reserved.isEmpty())
                    finalPath = reserved;
                job.filePath = finalPath;
            }
            if (!QFile::rename(partial, finalPath)) {
                // 可能是路径竞争或权限/磁盘错误。再尝试一次唯一名；仍失败才清理。
                const QFileInfo info(finalPath);
                const QString retryPath =
                    reserveFilePath(info.absolutePath(), info.fileName(), job.id);
                if (!retryPath.isEmpty() && retryPath != finalPath
                    && QFile::rename(partial, retryPath)) {
                    finalPath = retryPath;
                    m_jobs[index].filePath = finalPath;
                } else {
                    QFile::remove(partial);
                    finishJob(id, finalPath, false, tr("缓存复制完成，但无法写入目标文件"));
                    continue;
                }
            }
            m_jobs[index].filePath = finalPath;
            finishJob(id, finalPath, true, tr("下载完成（来自播放缓存）"));
            changed = true;
            continue;
        }

        if (target->write(chunk) != chunk.size()) {
            const QString error = target->errorString();
            const QString targetPath = m_jobs.at(index).filePath;
            cancelCacheCopy(id, true);
            finishJob(id, targetPath, false, tr("写入下载文件失败：%1").arg(error));
            continue;
        }
        m_jobs[index].bytesReceived += chunk.size();
        changed = true;
    }
    if (changed) {
        scheduleSave();
        scheduleNotify();
    }
    if (m_cacheCopySources.isEmpty())
        m_cacheCopyTimer->stop();
}

/// @brief 取消缓存拷贝任务，可一并删除已写入的部分文件。
void DownloadManager::cancelCacheCopy(const QString &id, bool removePartial)
{
    QFile *source = m_cacheCopySources.take(id);
    QFile *target = m_cacheCopyTargets.take(id);
    const QString partial = target != nullptr ? target->fileName() : QString();
    if (source != nullptr) {
        source->close();
        source->deleteLater();
    }
    if (target != nullptr) {
        target->close();
        target->deleteLater();
    }
    if (removePartial && !partial.isEmpty())
        QFile::remove(partial);
}

/// @brief 任务收尾：更新状态/路径、落盘并通知界面，随后继续排队。
/// @param success 是否成功；失败时保留半成品供重试。
void DownloadManager::finishJob(const QString &id, const QString &path, bool success,
                                const QString &message)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    DownloadJob &job = m_jobs[index];
    // 跨盘发布还在进行：调用方拿到的只是“发布已开始”，真正完成由
    // copyPublishChunks 在改名成功后回调本函数。这里暂存收尾消息，
    // 不能提前把任务标成 Completed。
    if (success && m_publishSources.contains(id)) {
        m_pendingPublishMessages.insert(id, message);
        scheduleSave();
        scheduleNotify();
        return;
    }
    job.filePath = path;
    const qint64 finalSize = QFileInfo(path).size();
    if (success) {
        job.bytesReceived = finalSize;
        // HLS 任务的记录长度是合并后的 TS，成功转封装后路径已变成 MP4；
        // 完成后统一按最终文件大小收口，否则进度条会停在 TS 大小的比例上。
        job.bytesTotal = job.bytesReceived;
    } else {
        // 失败时 path 通常是最终目标名，真正的半成品在同名 .mvpdownload；
        // 直接按最终路径取大小会把已接收字节清零，丢失续传进度。
        const qint64 partialSize = QFileInfo(path + QStringLiteral(".mvpdownload")).size();
        const qint64 measured = qMax(finalSize, partialSize);
        if (measured > 0)
            job.bytesReceived = measured;
        if (job.bytesTotal <= 0)
            job.bytesTotal = job.bytesReceived;
    }
    job.status = success ? DownloadJob::Status::Completed
                         : DownloadJob::Status::Failed;
    // 先记下缓存源路径：成功分支会清掉 cacheCopySourcePath，但若开启了
    // “下载完成后删除播放缓存”，还需要这个精确路径做清理。
    const QString copiedCachePath = job.cacheCopySourcePath;
    const QString cleanupSource = !copiedCachePath.isEmpty()
        ? copiedCachePath
        : job.seedCacheBodyPath;
    if (success) {
        job.committed = true;
        job.pausedByUser = false;
        job.cacheCopySourcePath.clear();
        job.seedSourcePath.clear();
        job.seedBytes = 0;
        job.seedSegmentIndex = 0;
        job.seedFingerprint.clear();
        if (AppSettings::removePlaybackCacheAfterDownload()) {
            // 不能只在 cleanupSource 非空时安排清理：直接网络下载没有复用
            // 播放缓存，但播放器可能已经留下完整/半成品缓存。成功下载后
            // 统一按条目身份清理播放缓存与歌词缓存。
            job.cleanupMediaAndLyrics = true;
            if (!cleanupSource.isEmpty())
                job.cleanupCachePath = cleanupSource;
        }
        job.seedCacheBodyPath.clear();
    }
    QString finalMessage = message;
    if (success && job.snapshot && !finalMessage.contains(QStringLiteral("快照"))) {
        // 不把无 ENDLIST 的结果伪装成“完整下载”；下载管理器与状态栏都显示快照提示。
        finalMessage += tr("（当前分片快照，可能不完整）");
    }
    job.message = finalMessage;
    job.completedAt = QDateTime::currentDateTime();
    // 先复制信号参数，不能在 emit 后再访问 job：槽函数可能同步 removeJob()。
    const MediaItem completedItem = job.item;
    scheduleSave();
    scheduleNotify();

    if (success) {
        // 命中缓存复制时，下载文件已经完整落盘；此时删除播放缓存不会影响
        // 下载结果。若播放器仍占用缓存，cleanupPendingCaches 会失败并定时
        // 重试，剩余路径写进 downloads.json，下次启动继续清理。
        if (!cleanupSource.isEmpty() || job.cleanupMediaAndLyrics)
            cleanupPendingCaches();
        emit statusMessage(finalMessage);
        emit jobCompleted(completedItem, path);
    }
    // finishJob 可能在 startRemux/pumpQueue 栈内被调用；同步 pump 会让
    // 大量 Paused HLS 任务递归 startJob -> finishJob，改为事件循环排队。
    QTimer::singleShot(0, this, &DownloadManager::pumpQueue);
}
