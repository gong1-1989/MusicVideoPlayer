#include "download/DownloadManager.h"

#include "download/DownloadPaths.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "net/HttpClient.h"
#include "storage/StoragePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUuid>

Q_LOGGING_CATEGORY(lcDownloadManager, "mvp.download.manager")

namespace {

const int kMaxCompletedJobs = 100;

/// Windows 上 QNetworkReply/QFile 刚 close 时句柄可能还没完全释放，
/// 删除失败会留下“已移除任务但临时文件还在”的假象；这里短暂重试几次。
bool removeFileNow(const QString &path)
{
    if (path.isEmpty())
        return true;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!QFileInfo::exists(path))
            return true;
        if (QFile::remove(path))
            return true;
        QThread::msleep(25);
    }
    return !QFileInfo::exists(path);
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
}

QString DownloadManager::enqueue(const MediaItem &item, const QUrl &url,
                                 const QHash<QString, QString> &headers,
                                 const QString &filePath, bool hls, qint64 maxBytes)
{
    for (const DownloadJob &existing : m_jobs) {
        if (existing.status != DownloadJob::Status::Completed
            && existing.status != DownloadJob::Status::Failed
            && existing.item.type == item.type
            && mediaIdentity(existing.item) == mediaIdentity(item)) {
            emit statusMessage(tr("该条目已在下载队列中"));
            return existing.id;
        }
    }

    DownloadJob job;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.item = item;
    job.url = url;
    job.headers = headers;
    job.filePath = filePath;
    job.hls = hls;
    job.maxBytes = maxBytes;
    job.status = DownloadJob::Status::Paused;
    job.message = tr("等待下载");
    job.createdAt = QDateTime::currentDateTime();

    m_jobs.append(job);
    scheduleSave();
    scheduleNotify();
    startJob(job.id);
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
        job.message = tr("正在恢复上次未完成的下载");
        startJob(job.id);
    }
    if (any) {
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

    auto remuxIt = m_remuxes.find(id);
    if (remuxIt != m_remuxes.end()) {
        MediaRemux *remux = remuxIt.value();
        m_remuxes.erase(remuxIt);
        remux->disconnect(this);
        remux->cancel();
        remux->deleteLater();
    }

    if (job.status != DownloadJob::Status::Completed) {
        // 删除进行中的任务时，已下载的部分必须同步消失。
        // task->cancel() 已关文件并删过一遍；这里按 job 记录兜底，
        // 覆盖 HLS 改过后缀、MP4 半成品和状态文件与磁盘不同步的情况。
        const QStringList leftovers = {
            job.partialPath(), job.filePath, uniqueMp4Path(job.filePath),
            job.filePath + QStringLiteral(".ts"),
            job.filePath + QStringLiteral(".ts.mvpdownload")};
        for (const QString &path : leftovers) {
            if (!removeFileNow(path))
                qCWarning(lcDownloadManager) << "移除下载任务时无法删除临时文件" << path;
        }
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
    startJob(job.id);
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

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcDownloadManager) << "下载状态文件损坏" << parseError.errorString();
        return;
    }

    const QJsonArray array = document.object().value(QStringLiteral("jobs")).toArray();
    int completed = 0;
    for (const QJsonValue &value : array) {
        DownloadJob job;
        QString error;
        if (!DownloadJob::fromJson(value.toObject(), &job, &error)) {
            qCWarning(lcDownloadManager) << "跳过无效下载记录" << error;
            continue;
        }
        if (job.status == DownloadJob::Status::Downloading) {
            job.status = DownloadJob::Status::Paused;
            job.message = tr("上次退出时暂停，正在自动续传");
        }
        if (job.status == DownloadJob::Status::Completed) {
            ++completed;
            if (completed > kMaxCompletedJobs)
                continue;
        }
        m_jobs.append(job);
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

    QSaveFile file(m_statePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcDownloadManager) << "下载状态文件无法写入" << file.errorString();
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
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

    // 上次可能已经把最终文件写完，只是状态没来得及落盘。
    if (QFileInfo::exists(job.filePath) && QFileInfo(job.filePath).size() > 0) {
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
    task->setMaxBytes(job.maxBytes > 0 ? job.maxBytes : NetLimits::kDownloadBytes);
    task->setResumeState(job.bytesReceived, job.segmentIndex, job.hls);
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

    const QString output = uniqueMp4Path(job.filePath);
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
                    QFile::remove(j.filePath);
                    finishJob(id, actual, true, tr("下载完成（MP4）"));
                } else {
                    QFile::remove(output);
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
    job.message = message;
    job.completedAt = QDateTime::currentDateTime();
    scheduleSave();
    scheduleNotify();

    if (success) {
        emit statusMessage(message);
        emit jobCompleted(job.item, path);
    }
}

QString DownloadManager::uniqueMp4Path(const QString &tsPath)
{
    const QFileInfo info(tsPath);
    return DownloadPaths::uniqueFilePath(info.absolutePath(),
                                         info.completeBaseName() + QStringLiteral(".mp4"));
}
