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
    qint64 bytesReceived = 0;
    qint64 bytesTotal = -1;
    qint64 maxBytes = 0;
    int segmentIndex = 0;
    int segmentCount = 0;
    bool hls = false;
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
