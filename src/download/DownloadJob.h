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
    QString filePath;
    /// 缓存复制任务的源文件；持久化后重启可继续复制，而不是退回网络下载。
    QString cacheCopySourcePath;
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
    QString partialPath() const;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &object, DownloadJob *job, QString *error);
};

#endif // DOWNLOADJOB_H
