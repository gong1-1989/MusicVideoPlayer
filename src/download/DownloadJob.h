#ifndef DOWNLOADJOB_H
#define DOWNLOADJOB_H

#include "media/MediaItem.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QUrl>

/// 一条可持久化的下载任务记录。下载中的任务写入 Data/downloads.json，
/// 程序退出后保留已下载字节数和 HLS 分片序号，下次启动自动续传。
struct DownloadJob
{
    enum class Status {
        Downloading,
        Paused,
        Completed,
        Failed,
    };

    QString id;
    MediaItem item;
    QUrl url;
    QHash<QString, QString> headers;
    /// 用户最终要拿到的下载路径。影视 HLS 为带 .mp4 的目标；完成前不会在
    /// 下载目录里生成同名半成品。
    QString filePath;
    /// 影视 HLS 的统一暂存路径。下载、合并、转封装都先在这里完成，成功后再
    /// 原子移动到 filePath；非暂存任务为空。跨启动持久化，退出后可接着下。
    QString stagingPath;
    /// 从播放缓存复用已完成分片时，源文件与断点信息；下载开始前先异步复制
    /// 这段前缀到暂存区，再由 DownloadTask 校验播放列表指纹并继续。
    QString seedSourcePath;
    qint64 seedBytes = 0;
    int seedSegmentIndex = 0;
    QString seedFingerprint;
    /// 播放缓存正文路径，下载成功后按设置清理整条缓存。
    QString seedCacheBodyPath;
    /// 缓存复制任务的源文件；持久化后重启可继续复制，而不是退回网络下载。
    QString cacheCopySourcePath;
    /// 已完成下载、但对应播放缓存尚未删除的路径。播放器占用文件时
    /// 删除会失败，记录到任务里，重启或空闲时继续尝试。
    QString cleanupCachePath;
    qint64 bytesReceived = 0;
    qint64 bytesTotal = -1;
    qint64 maxBytes = 0;
    int segmentIndex = 0;
    int segmentCount = 0;
    bool hls = false;
    /// 列表没有 #EXT-X-ENDLIST 时按当前分片下载，结果只是快照。
    /// 上次成功解析出的 HLS 分片结构指纹；续传必须匹配，否则从头下载。
    QString hlsFingerprint;
    bool snapshot = false;
    /// 用户主动暂停。与队列里的“等待下载”共用 Paused 状态，但重启后
    /// 不能自动续传，必须等用户点“继续下载”。
    bool pausedByUser = false;
    /// 用户已明确授权无 ENDLIST 也下载（例如下载管理器里的“强制下载当前分片”）。
    bool allowIncomplete = false;
    /// 上次因缺少 ENDLIST 且未声明 VOD 被拒绝，界面据此提供强制下载入口。
    bool needsSnapshotConsent = false;
    /// 最终文件是否由本任务成功提交。旧状态没有这个字段时按 false 处理，
    /// 不能把用户后来放在目标路径的同名文件当成自己的下载结果。
    bool committed = false;
    Status status = Status::Paused;
    QString message;
    QDateTime createdAt;
    QDateTime completedAt;

    int percent() const;
    QString statusText() const;
    bool isActive() const { return status == Status::Downloading; }
    /// 当前实际写入的工作路径：暂存任务在 stagingPath，其他在 filePath。
    QString workPath() const { return stagingPath.isEmpty() ? filePath : stagingPath; }
    /// 当前任务半成品路径，固定跟在 workPath() 后。
    QString partialPath() const;
    /// 暂存目录；不是暂存任务时为空。
    QString stagingDirectory() const;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &object, DownloadJob *job, QString *error);
};

#endif // DOWNLOADJOB_H
