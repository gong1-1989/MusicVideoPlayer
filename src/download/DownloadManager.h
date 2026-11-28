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
    /// 三类后台搬运共用一套状态：播放缓存复制、种子前缀复制、跨盘发布。
    enum class ChunkCopyKind {
        Cache,   ///< 播放缓存整文件复制到下载暂存区
        Seed,    ///< 播放缓存前缀复制到下载暂存区
        Publish  ///< 暂存文件跨盘复制到下载目录
    };

    struct ChunkCopy
    {
        ChunkCopyKind kind = ChunkCopyKind::Cache;
        QFile *source = nullptr;
        QFile *target = nullptr;
        /// Seed 需要精确搬完的字节数；Cache/Publish 读到 EOF 为止。
        qint64 remaining = -1;
        QString finalPath;      ///< Publish 的最终目标
        QString stagingDir;     ///< Publish 完成后可删除的暂存目录
        QString pendingMessage; ///< Publish 期间 finishJob 暂存的收尾文案
    };
    int indexOf(const QString &id) const;
    QString reserveFilePath(const QString &directory, const QString &fileName,
                            const QString &ignoreId) const;
    bool isPathReserved(const QString &path, const QString &ignoreId) const;
    bool isPathOwnedByAnyJob(const QString &path) const;
    void load();
    void saveNow();
    void scheduleSave();
    void scheduleNotify();
    /// 每 500ms 检查一次任务速度，并用相邻真实字节采样更新单任务速度；
    /// 速度只是运行时数据，不进入持久化状态文件。
    void sampleTransferSpeeds();
    /// 用一次真实字节采样更新任务速度；返回速度是否变化。
    bool noteTransferSample(DownloadJob &job, qint64 bytes, qint64 nowMs);
    void scheduleRemoveFile(const QString &path, int attempt = 0);
    /// 清理单条下载记录挂着的播放缓存/歌词缓存；返回 false 表示仍有缓存
    /// 被占用，需要保留记录并定时重试。
    bool cleanupJobCaches(DownloadJob &job, bool *changed = nullptr);
    /// 清理所有已完成记录挂着的缓存；被播放器占用时定时重试，程序重启后
    /// 也会继续。只删应用自己的播放/歌词缓存，不碰用户下载文件。
    void cleanupPendingCaches();
    /// 状态文件超限时是否可以安全丢弃这条记录。
    /// @note 带暂存/半成品/提交文件或缓存清理标记的记录不能丢，否则
    ///       对应文件会失去唯一管理入口。
    bool canTrimForStateLimit(const DownloadJob &job) const;
    /// 升级迁移：把旧版本放在下载目录 `.download-cache` 里的暂存文件搬到
    /// 应用数据目录，避免重启后继续在下载目录重建缓存；搬不到时保留原
    /// 路径，宁可暂时留旧缓存也不能丢续传进度。
    void migrateLegacyStagingPaths();
    /// 校验暂存文件是否还在；被用户手动清理时同步清零续传点和分片进度，
    /// 避免界面显示“59/649 分片、0%”这种前后不一致的状态。
    void reconcileStagingState();
    /// 迁移遗留：旧版本把暂存分片放在下载目录下的 `.download-cache`，
    /// 新版本统一放到应用数据目录；启动时清掉旧目录，避免用户误以为
    /// 下载目录里还在持续产生缓存。
    void cleanLegacyDownloadCaches();
    void pumpQueue();
    void startJob(const QString &id);
    bool startCacheCopy(const QString &id, const QString &sourcePath);
    void startRemux(const QString &id);
    /// 三套复制共用的分块搬运循环，按 kind 走各自的完成/失败分支。
    void copyChunks();
    void cancelCacheCopy(const QString &id, bool removePartial);
    /// 下载任务统一暂存路径；HLS 默认用 `ts`，音乐直链用最终后缀。
    QString stagingPathForJob(const DownloadJob &job,
                              const QString &suffix = QStringLiteral("ts")) const;
    /// 下载开始前把播放缓存前缀分块复制到暂存区，避免大文件阻塞 UI。
    bool startSeedCopy(const QString &id);
    void cancelSeedCopy(const QString &id, bool removePartial);
    /// 暂存文件发布到下载目录：同盘直接 rename；跨盘时启动分块复制，
    /// 复制期间 finishJob 会暂缓收尾，避免大文件拷贝卡住界面。
    bool publishStagedFile(const QString &id, const QString &sourcePath,
                           QString *publishedPath);
    bool startPublishCopy(const QString &id, const QString &sourcePath,
                          const QString &finalPath);
    void cancelPublishCopy(const QString &id, bool removePartial);
    /// 共用取消入口；只处理与 kind 匹配的活动复制，避免误停同任务另一阶段。
    void cancelChunkCopy(const QString &id, ChunkCopyKind kind, bool removePartial);
    /// 启动共用计时器；已在运行时不重复 start。
    void ensureChunkCopyTimer();
    /// 上次退出时暂存区可能已有完整 TS 或 MP4，恢复时应先转封装/发布而不是重下。
    void startStagedCompletion(const QString &id);
    void finishJob(const QString &id, const QString &path, bool success,
                   const QString &message);

    HttpClient *m_http = nullptr;
    QVector<DownloadJob> m_jobs;
    QHash<QString, QPointer<DownloadTask>> m_tasks;
    QHash<QString, MediaRemux *> m_remuxes;
    QHash<QString, QString> m_remuxOutputs;
    QHash<QString, ChunkCopy> m_chunkCopies;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_notifyTimer = nullptr;
    QTimer *m_speedTimer = nullptr;
    /// 单任务速度采样状态：上次采样时间/累计字节，用于计算相邻采样的增量。
    struct SpeedState
    {
        qint64 lastMs = 0;
        qint64 lastBytes = 0;
    };
    QHash<QString, SpeedState> m_speedStates;
    QTimer *m_chunkCopyTimer = nullptr;
    QTimer *m_cacheCleanupTimer = nullptr;
    QString m_statePath;
    bool m_shuttingDown = false;
    /// 状态文件超过上限且无法安全裁剪时只提示一次，避免每次自动保存都刷状态栏。
    bool m_stateLimitWarned = false;
};

#endif // DOWNLOADMANAGER_H
