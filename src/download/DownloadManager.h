#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include "download/DownloadJob.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>
#include <QUrl>

class DownloadTask;
class HttpClient;
class MediaRemux;
class QTimer;

/// 下载队列与持久化：
/// - 任务状态写 Data/downloads.json，退出后保留临时文件并在下次启动时自动续传；
/// - 所有任务完成后，如果是视频且列表里存在同一来源，发出 jobCompleted 供播放列表改成本地路径；
/// - 有 FFmpeg 时，HLS 合并出的 TS 会再无损转成 MP4。
class DownloadManager : public QObject
{
    Q_OBJECT

public:
    explicit DownloadManager(HttpClient *http, QObject *parent = nullptr);
    ~DownloadManager() override;

    QString enqueue(const MediaItem &item, const QUrl &url,
                    const QHash<QString, QString> &headers,
                    const QString &filePath, bool hls, qint64 maxBytes);
    /// 在下载队列范围内分配不冲突的目标名；先由界面调用显示文件名，
    /// enqueue 内部还会再校验一次，避免调用方绕过队列。
    QString reserveFilePath(const QString &directory, const QString &fileName) const;
    const QVector<DownloadJob> &jobs() const { return m_jobs; }

    int activeCount() const;
    bool hasActiveTasks() const;
    bool isEmpty() const { return m_jobs.isEmpty(); }

    void resumePending();
    void shutdownForExit();

    bool removeJob(const QString &id);
    bool retryJob(const QString &id);
    void clearCompleted();

    QString stateFilePath() const { return m_statePath; }

signals:
    void jobsChanged();
    void jobCompleted(const MediaItem &item, const QString &localPath);
    void statusMessage(const QString &message);

private:
    int indexOf(const QString &id) const;
    QString reserveFilePath(const QString &directory, const QString &fileName,
                            const QString &ignoreId) const;
    bool isPathReserved(const QString &path, const QString &ignoreId) const;
    void load();
    void saveNow();
    void scheduleSave();
    void scheduleNotify();
    void scheduleRemoveFile(const QString &path, int attempt = 0);
    void pumpQueue();
    void startJob(const QString &id);
    void startRemux(const QString &id);
    void finishJob(const QString &id, const QString &path, bool success,
                   const QString &message);

    HttpClient *m_http = nullptr;
    QVector<DownloadJob> m_jobs;
    QHash<QString, QPointer<DownloadTask>> m_tasks;
    QHash<QString, MediaRemux *> m_remuxes;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_notifyTimer = nullptr;
    QString m_statePath;
    bool m_shuttingDown = false;
};

#endif // DOWNLOADMANAGER_H
