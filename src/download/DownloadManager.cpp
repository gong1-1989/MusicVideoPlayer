#include "download/DownloadManager.h"

#include "download/DownloadPaths.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "net/HttpClient.h"
#include "storage/StoragePaths.h"
#include "util/HeaderPolicy.h"

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

    m_cacheCopyTimer = new QTimer(this);
    // 缓存复制每轮只搬固定大小，主线程不会因为大文件整段复制而卡住。
    m_cacheCopyTimer->setInterval(0);
    connect(m_cacheCopyTimer, &QTimer::timeout, this, &DownloadManager::copyCacheChunks);

    load();
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
    if (m_cacheCopyTimer != nullptr)
        m_cacheCopyTimer->stop();
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
            || DownloadPaths::pathsReferToSameFile(path, job.partialPath())) {
            return true;
        }
    }
    return false;
}

bool DownloadManager::isPathOwnedByAnyJob(const QString &path) const
{
    if (path.isEmpty())
        return false;
    // 延迟删除专用：Completed/Failed 也算占用，否则旧任务的 25ms 定时器
    // 可能把新任务刚完成的最终文件删掉。
    for (const DownloadJob &job : m_jobs) {
        if (DownloadPaths::pathsReferToSameFile(path, job.filePath)
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

QString DownloadManager::enqueue(const MediaItem &item, const QUrl &url,
                                 const QHash<QString, QString> &headers,
                                 const QString &filePath, bool hls, qint64 maxBytes,
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

    m_jobs.append(job);
    if (queuedPath != nullptr)
        *queuedPath = job.filePath;
    scheduleSave();
    scheduleNotify();
    pumpQueue();
    return job.id;
}

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
    const QFileInfo targetInfo(filePath);
    job.filePath = reserveFilePath(targetInfo.absolutePath(), targetInfo.fileName());
    if (job.filePath.isEmpty()) {
        emit statusMessage(tr("无法为下载分配唯一文件名，请检查下载目录"));
        return QString();
    }
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

int DownloadManager::activeCount() const
{
    int count = 0;
    for (const DownloadJob &job : m_jobs) {
        if (job.status == DownloadJob::Status::Downloading)
            ++count;
    }
    return count;
}

void DownloadManager::resumePending()
{
    if (m_shuttingDown)
        return;

    bool any = false;
    bool changed = false;
    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Paused)
            continue;
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

void DownloadManager::shutdownForExit()
{
    m_shuttingDown = true;
    const QStringList copyIds = m_cacheCopySources.keys();
    for (const QString &id : copyIds)
        cancelCacheCopy(id, false);

    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Downloading)
            continue;

        job.status = DownloadJob::Status::Paused;
        job.message = job.cacheCopySourcePath.isEmpty()
            ? tr("程序退出，已暂停；下次启动会自动续传")
            : tr("程序退出，已暂停；下次启动继续从播放缓存生成");

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

bool DownloadManager::removeJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;

    DownloadJob job = m_jobs.at(index);

    // 缓存复制任务没有 DownloadTask，必须单独关句柄并清掉半成品。
    cancelCacheCopy(id, true);

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
        QStringList leftovers = {job.partialPath()};
        // HLS 已完成合并、正在转 MP4 时，TS 是当前任务自己产出的最终文件；
        // 其他状态下最终文件不归本任务，不能碰。
        if (removingRemux)
            leftovers.append(job.filePath);
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
    job.message = allowIncomplete ? tr("已授权按当前分片强制下载，等待开始")
                                  : tr("等待重试");
    pumpQueue();
    scheduleSave();
    scheduleNotify();
    return true;
}

void DownloadManager::clearCompleted()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs.at(i).status == DownloadJob::Status::Completed)
            m_jobs.removeAt(i);
    }
    scheduleSave();
    emit jobsChanged();
}

int DownloadManager::indexOf(const QString &id) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).id == id)
            return i;
    }
    return -1;
}

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

    const QJsonArray array = document.object().value(QStringLiteral("jobs")).toArray();
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
            job.committed = false;
            job.message = tr("快照任务在退出时中断，不自动续传；请重新下载");
            changedInLoad = true;
        } else if (job.status == DownloadJob::Status::Downloading) {
            job.status = DownloadJob::Status::Paused;
            job.message = tr("上次退出时暂停，正在自动续传");
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
                // 重新分配目标后，旧路径的续传点已无法对应新目标；
                // 不清掉会在下载目录里长期留下一个无主 .mvpdownload。
                const QString oldPartial = job.filePath + QStringLiteral(".mvpdownload");
                if (QFileInfo::exists(oldPartial) && !QFile::remove(oldPartial)) {
                    qCWarning(lcDownloadManager) << "旧续传点清理失败" << oldPartial;
                }
                job.filePath = reserved;
                job.bytesReceived = 0;
                job.segmentIndex = 0;
                job.message = tr("状态文件路径冲突，已重新分配目标文件");
            }
            break;
        }
    }

    if (changedInLoad)
        scheduleSave();
    qCInfo(lcDownloadManager) << "已载入" << m_jobs.size() << "条下载记录";
}

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

void DownloadManager::scheduleSave()
{
    if (!m_shuttingDown)
        m_saveTimer->start();
}

void DownloadManager::scheduleNotify()
{
    if (!m_notifyTimer->isActive())
        m_notifyTimer->start();
}

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

void DownloadManager::pumpQueue()
{
    if (m_shuttingDown)
        return;

    while (activeCount() < kMaxConcurrentDownloads) {
        QString nextId;
        for (const DownloadJob &job : std::as_const(m_jobs)) {
            if (job.status != DownloadJob::Status::Paused)
                continue;
            if (m_tasks.contains(job.id) || m_remuxes.contains(job.id))
                continue;
            nextId = job.id;
            break;
        }
        if (nextId.isEmpty())
            return;
        startJob(nextId);
        // startJob 可能因为文件已写完而立即 Completed，也可能转入 remux；
        // 继续循环直到达到并发上限为止。
    }
}

void DownloadManager::startJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return;
    if (m_tasks.contains(id) || m_remuxes.contains(id))
        return;

    DownloadJob &job = m_jobs[index];
    if (job.status == DownloadJob::Status::Completed)
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
    job.message = job.hls ? tr("正在解析播放列表") : tr("正在下载");

    DownloadTask *task = DownloadTask::start(m_http, job.url, job.filePath,
                                             job.headers, this);
    if (task == nullptr) {
        job.status = DownloadJob::Status::Failed;
        job.message = tr("无法启动下载任务");
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
                j.filePath = path;
                j.hls = hls;
                j.bytesReceived = 0;
                j.segmentIndex = 0;
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
                j.filePath = path;
                j.bytesReceived = QFileInfo(path).size();
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

void DownloadManager::startRemux(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0 || m_shuttingDown)
        return;
    DownloadJob &job = m_jobs[index];
    if (m_remuxes.contains(id))
        return;

    if (!MediaRemux::isAvailable()) {
        finishJob(id, job.filePath, true,
                  tr("下载完成（TS）：没有找到随程序安装的 FFmpeg"));
        return;
    }

    const QFileInfo tsInfo(job.filePath);
    const QString output =
        reserveFilePath(tsInfo.absolutePath(),
                        tsInfo.completeBaseName() + QStringLiteral(".mp4"), job.id);
    if (output.isEmpty()) {
        finishJob(id, job.filePath, true, tr("下载完成（TS）：无法分配 MP4 输出文件名"));
        return;
    }
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
                if (success && QFileInfo(actual).exists()) {
                    // 只有文件仍与下载记录一致时才删除源 TS；用户可能已经把
                    // 同名 TS 换成自己的文件，不能替用户删。
                    if (QFileInfo(j.filePath).size() == j.bytesReceived)
                        QFile::remove(j.filePath);
                    finishJob(id, actual, true, tr("下载完成（MP4）"));
                } else {
                    finishJob(id, j.filePath, true,
                              tr("下载完成（TS）：转 MP4 失败%1")
                                  .arg(error.isEmpty() ? QString()
                                                       : QStringLiteral("（") + error + QStringLiteral("）")));
                }
            });
    remux->remuxToMp4(job.filePath, output);
}

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
            if (!target->flush()) {
                const QString error = target->errorString();
                target->close();
                source->close();
                cancelCacheCopy(id, true);
                finishJob(id, finalPath, false, tr("刷新下载文件失败：%1").arg(error));
                continue;
            }
            target->close();
            source->close();
            cancelCacheCopy(id, false);
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

void DownloadManager::finishJob(const QString &id, const QString &path, bool success,
                                const QString &message)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    DownloadJob &job = m_jobs[index];
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
    if (success) {
        job.committed = true;
        job.cacheCopySourcePath.clear();
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
        emit statusMessage(finalMessage);
        emit jobCompleted(completedItem, path);
    }
    // finishJob 可能在 startRemux/pumpQueue 栈内被调用；同步 pump 会让
    // 大量 Paused HLS 任务递归 startJob -> finishJob，改为事件循环排队。
    QTimer::singleShot(0, this, &DownloadManager::pumpQueue);
}
