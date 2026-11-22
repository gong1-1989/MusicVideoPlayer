#ifndef HLSCACHEBUILDER_H
#define HLSCACHEBUILDER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QPointer>
#include <QString>

class DownloadTask;
class HttpClient;
class MediaRemux;

/// 播放中的 HLS 影视后台整段缓存。
///
/// 不暴露成用户下载任务：只跟随当前播放条目，顺序拉取全部分片、合并 TS，
/// 再用 MediaRemux 无损转封装成 `media-cache/<条目名>/media`（正文无后缀），
/// 旁车 JSON 记录 `.mp4`。完整缓存命中时不再重复下载。
/// 加密 HLS、没有 #EXT-X-ENDLIST 且非 VOD 的列表不生成缓存，避免半截文件
/// 被当成可离线播放的完整视频。
class HlsCacheBuilder : public QObject
{
    Q_OBJECT

public:
    explicit HlsCacheBuilder(HttpClient *http, QObject *parent = nullptr);

    /// 开始缓存当前条目；非在线/非影视/非 HLS/已有完整缓存时直接返回 false。
    bool start(const MediaItem &item);
    /// 切换播放条目或退出：中止任务并清掉未完成的分片/TS，保留已完成正文。
    void cancel();
    /// 删除播放列表条目：中止并移除这个条目的未完成与最终正文。
    void cancelAndRemove();
    bool isActive() const { return m_active; }
    MediaItem currentItem() const { return m_item; }

signals:
    void completed(const MediaItem &item, const QString &cachePath);
    void failed(const MediaItem &item, const QString &message);

private:
    void onTsReady(const QString &tsPath);
    void onRemuxFinished(bool success, const QString &outputPath, const QString &error);
    void cleanupPartial(bool removeBody);
    void finishFailed(const QString &message);

    HttpClient *m_http = nullptr;
    MediaItem m_item;
    QString m_bodyPath;
    QString m_tsPath;
    bool m_active = false;
    QPointer<DownloadTask> m_task;
    QPointer<MediaRemux> m_remux;
};

#endif // HLSCACHEBUILDER_H
