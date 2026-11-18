#ifndef PLAYLISTSTORE_H
#define PLAYLISTSTORE_H

#include "media/MediaItem.h"

#include <QString>
#include <QVector>

/// 播放列表持久化：跨会话保留音乐/视频列表。
/// 存成独立 JSON（不复用 QSettings 注册表），列表变长后也不会把注册表撑大。
namespace PlaylistStore {

QString filePath();

QVector<MediaItem> load(MediaType type);
void save(MediaType type, const QVector<MediaItem> &items);

/// 一次写入两类列表，避免两次读改写互相覆盖
void saveAll(const QVector<MediaItem> &music, const QVector<MediaItem> &video);

} // namespace PlaylistStore

#endif // PLAYLISTSTORE_H
