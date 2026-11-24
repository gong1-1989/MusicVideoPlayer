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
class QFile;
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

    /// 从播放缓存复用的 HLS 已完成分片。下载任务会先把这段前缀复制到暂存区，
    /// 再用 fingerprint 校验播放列表；不一致时 DownloadTask 会丢弃并从头下载。
    struct CacheSeed
    {
        QString sourcePath;
        qint64 bytes = 0;
        int segmentIndex = 0;
        QString fingerprint;
        QString cacheBodyPath;
    };

    QString enqueue(const MediaItem &item, const QUrl &url,
                    const QHash<QString, QString> &headers,
                    const QString &filePath, bool hls, qint64 maxBytes,
                    QString *queuedPath = nullptr, bool *alreadyQueued = nullptr,
                    const CacheSeed *seed = nullptr);
    /// 命中完整播放缓存时，从无后缀缓存异步复制出带正确后缀的下载文件。
    /// 不占用下载并发槽位，但同样走 `.mvpdownload` 临时文件与完成信号。
    QString importFromCache(const MediaItem &item, const QString &cachePath,
                            const QString &filePath, qint64 maxBytes = 0,
                            QString *queuedPath = nullptr,
                            bool *alreadyQueued = nullptr);
    /// 在下载队列范围内分配不冲突的目标名；先由界面调用显示文件名，
    /// enqueue 内部还会再校验一次，避免调用方绕过队列。
    QString reserveFilePath(const QString &directory, const QString &fileName) const;
    const QVector<DownloadJob> &jobs() const { return m_jobs; }

    int activeCount() const;
    bool isEmpty() const { return m_jobs.isEmpty(); }

    void resumePending();
    void shutdownForExit();

    bool removeJob(const QString &id);
    /// 暂停正在下载/转封装/暂存复制的任务：保留半成品，之后可继续。
    bool pauseJob(const QString &id);
    /// 继续已暂停任务；不重置续传字节。
    bool resumeJob(const QString &id);
    /// allowIncomplete=true 是用户确认后的“按当前分片强制下载”，
    /// 仅对缺少 #EXT-X-ENDLIST 且未声明 VOD 的失败任务生效。
    bool retryJob(const QString &id, bool allowIncomplete = false);
    void clearCompleted();

signals:
    void jobsChanged();
    void jobCompleted(const MediaItem &item, const QString &localPath);
    void statusMessage(const QString &message);

private:
    int indexOf(const QString &id) const;
    QString reserveFilePath(const QString &directory, const QString &fileName,
                            const QString &ignoreId) const;
    bool isPathReserved(const QString &path, const QString &ignoreId) const;
    bool isPathOwnedByAnyJob(const QString &path) const;
    void load();
    void saveNow();
    void scheduleSave();
    void scheduleNotify();
    void scheduleRemoveFile(const QString &path, int attempt = 0);
    /// 清理下载完成后标记了 cleanupCachePath 的播放缓存；被播放器占用时
    /// 定时重试，程序重启后也会继续。只删 media-cache，不碰歌词缓存。
    void cleanupPendingCaches();
    /// 迁移遗留：旧版本把暂存分片放在下载目录下的 `.download-cache`，
    /// 新版本统一放到应用数据目录；启动时清掉旧目录，避免用户误以为
    /// 下载目录里还在持续产生缓存。
    void cleanLegacyDownloadCaches();
    void pumpQueue();
    void startJob(const QString &id);
    bool startCacheCopy(const QString &id, const QString &sourcePath);
    void startRemux(const QString &id);
    void copyCacheChunks();
    void cancelCacheCopy(const QString &id, bool removePartial);
    /// 下载任务统一暂存路径；HLS 默认用 `ts`，音乐直链用最终后缀。
    QString stagingPathForJob(const DownloadJob &job,
                              const QString &suffix = QStringLiteral("ts")) const;
    /// 下载开始前把播放缓存前缀分块复制到暂存区，避免大文件阻塞 UI。
    bool startSeedCopy(const QString &id);
    void copySeedChunks();
    void cancelSeedCopy(const QString &id, bool removePartial);
    /// 暂存文件发布到下载目录：同盘直接 rename；跨盘时启动分块复制，
    /// 复制期间 finishJob 会暂缓收尾，避免大文件拷贝卡住界面。
    bool publishStagedFile(const QString &id, const QString &sourcePath,
                           QString *publishedPath);
    bool startPublishCopy(const QString &id, const QString &sourcePath,
                          const QString &finalPath);
    void copyPublishChunks();
    void cancelPublishCopy(const QString &id, bool removePartial);
    /// 上次退出时暂存区可能已有完整 TS 或 MP4，恢复时应先转封装/发布而不是重下。
    void startStagedCompletion(const QString &id);
    void finishJob(const QString &id, const QString &path, bool success,
                   const QString &message);

    HttpClient *m_http = nullptr;
    QVector<DownloadJob> m_jobs;
    QHash<QString, QPointer<DownloadTask>> m_tasks;
    QHash<QString, MediaRemux *> m_remuxes;
    QHash<QString, QString> m_remuxOutputs;
    QHash<QString, QFile *> m_cacheCopySources;
    QHash<QString, QFile *> m_cacheCopyTargets;
    QHash<QString, QFile *> m_seedSources;
    QHash<QString, QFile *> m_seedTargets;
    QHash<QString, qint64> m_seedRemaining;
    QHash<QString, QFile *> m_publishSources;
    QHash<QString, QFile *> m_publishTargets;
    QHash<QString, QString> m_publishFinalPaths;
    QHash<QString, QString> m_publishStagingDirs;
    QHash<QString, QString> m_pendingPublishMessages;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_notifyTimer = nullptr;
    QTimer *m_cacheCopyTimer = nullptr;
    QTimer *m_seedTimer = nullptr;
    QTimer *m_publishTimer = nullptr;
    QTimer *m_cacheCleanupTimer = nullptr;
    QString m_statePath;
    bool m_shuttingDown = false;
};

#endif // DOWNLOADMANAGER_H
