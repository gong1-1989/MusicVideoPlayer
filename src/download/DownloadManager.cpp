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

    load();
    QTimer::singleShot(0, this, &DownloadManager::resumePending);
}

DownloadManager::~DownloadManager()
{
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
        if (!it.value().isNull())
            it.value()->stopForExit();
    }
    for (auto *remux : m_remuxes)
        remux->cancel();
    // 最近完成/暂停的状态可能还在 900 ms 合并窗口里；析构时兜底落盘。
    if (!m_shuttingDown)
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
            && existing.item.type == item.type
            && mediaIdentity(existing.item) == mediaIdentity(item)) {
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

int DownloadManager::activeCount() const
{
    int count = 0;
    for (const DownloadJob &job : m_jobs) {
        if (job.status == DownloadJob::Status::Downloading)
            ++count;
    }
    return count;
}

bool DownloadManager::hasActiveTasks() const
{
    return activeCount() > 0;
}

void DownloadManager::resumePending()
{
    if (m_shuttingDown)
        return;

    bool any = false;
    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Paused)
            continue;
        any = true;
        job.message = tr("等待恢复上次未完成的下载");
    }
    if (any) {
        pumpQueue();
        scheduleNotify();
        scheduleSave();
    }
}

void DownloadManager::shutdownForExit()
{
    m_shuttingDown = true;

    for (DownloadJob &job : m_jobs) {
        if (job.status != DownloadJob::Status::Downloading)
            continue;

        job.status = DownloadJob::Status::Paused;
        job.message = tr("程序退出，已暂停；下次启动会自动续传");

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
    return true;
}

bool DownloadManager::retryJob(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    DownloadJob &job = m_jobs[index];
    if (job.status == DownloadJob::Status::Downloading
        || job.status == DownloadJob::Status::Completed) {
        return false;
    }

    job.status = DownloadJob::Status::Paused;
    job.message = tr("等待重试");
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
        if (job.status == DownloadJob::Status::Downloading) {
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

    qCInfo(lcDownloadManager) << "已载入" << m_jobs.size() << "条下载记录";
}

void DownloadManager::saveNow()
{
    if (m_shuttingDown)
        m_saveTimer->stop();

    QDir().mkpath(QFileInfo(m_statePath).absolutePath());

    QJsonArray array;
    for (const DownloadJob &job : m_jobs)
        array.append(job.toJson());

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), array);
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (payload.size() > kMaxStateFileBytes) {
        qCWarning(lcDownloadManager) << "下载状态超过上限，放弃本次保存" << payload.size()
                                     << kMaxStateFileBytes;
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
    if (!file.commit())
        qCWarning(lcDownloadManager) << "下载状态文件保存失败" << file.errorString();
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
        // 新任务已经占用这个路径时不能再按旧任务的所有权删除文件。
        if (isPathReserved(path, QString())) {
            qCInfo(lcDownloadManager) << "延迟删除跳过：路径已被新任务占用" << path;
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
    if (!QFileInfo::exists(partial)) {
        // 临时文件没了，上面的续传点也失效，必须从零开始。
        job.bytesReceived = 0;
        job.segmentIndex = 0;
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
    task->setResumeState(job.bytesReceived, job.segmentIndex, job.hls);

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

    connect(task, &DownloadTask::progress, this,
            [this, id](qint64 received, qint64 total) {
                const int i = indexOf(id);
                if (i < 0)
                    return;
                DownloadJob &j = m_jobs[i];
                if (j.hls) {
                    j.segmentIndex = int(received);
                    j.segmentCount = int(total);
                    j.message = tr("正在下载 %1/%2 分片").arg(received).arg(total);
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
                if (j.hls) {
                    j.status = DownloadJob::Status::Downloading;
                    j.message = tr("正在无损转成 MP4");
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
    auto *remux = new MediaRemux(this);
    m_remuxes.insert(id, remux);
    connect(remux, &MediaRemux::finished, this,
            [this, id, remux, output](bool success, const QString &actual,
                                      const QString &error) {
                m_remuxes.remove(id);
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

void DownloadManager::finishJob(const QString &id, const QString &path, bool success,
                                const QString &message)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    DownloadJob &job = m_jobs[index];
    job.filePath = path;
    job.bytesReceived = QFileInfo(path).size();
    if (job.bytesTotal <= 0)
        job.bytesTotal = job.bytesReceived;
    job.status = success ? DownloadJob::Status::Completed
                         : DownloadJob::Status::Failed;
    if (success)
        job.committed = true;
    job.message = message;
    job.completedAt = QDateTime::currentDateTime();
    scheduleSave();
    scheduleNotify();

    if (success) {
        emit statusMessage(message);
        emit jobCompleted(job.item, path);
    }
    pumpQueue();
}
