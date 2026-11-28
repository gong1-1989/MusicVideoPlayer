#ifndef PLAYLISTSTORE_H
#define PLAYLISTSTORE_H

#include "media/MediaItem.h"

#include <QString>
#include <QVector>

/// 播放列表持久化：跨会话保留音乐/视频列表。
/// 存成独立 JSON（不复用 QSettings 注册表），列表变长后也不会把注册表撑大。
namespace PlaylistStore {

/// 一次读取的结果。读取失败不能与“文件不存在/空列表”混为一谈：
/// storageBlocked 为 true 时，调用方必须禁止保存，否则空模型会覆盖原文件。
struct LoadResult
{
    QVector<MediaItem> music;
    QVector<MediaItem> video;
    bool fileMissing = false;
    bool recoveredFromBackup = false;
    bool storageBlocked = false;
    /// 只有“内容确实损坏且已成功备份”时才允许用户确认后清空重建；
    /// 占用/权限/超限仍可能保住有效数据，不能提供破坏性出口。
    bool canResetAfterCorruption = false;
    QString message;
    QString corruptBackupPath;
};

QString filePath();
QString backupFilePath();

/// 一次读取两个分区；主文件不可读时先备份，再回退到最近一次成功保存的快照。
LoadResult loadAll();

QVector<MediaItem> load(MediaType type);

/// 一次写入两类列表，避免两次读改写互相覆盖；成功后刷新滚动快照。
bool saveAll(const QVector<MediaItem> &music, const QVector<MediaItem> &video);

/// 用户确认损坏内容不再尝试恢复后，把现有主文件/快照改名备份，
/// 再写入空列表。任一步失败都不会覆盖尚未备份的文件，返回 false。
/// @param backedUpPaths 可选，返回本次新生成的备份路径（换行分隔）。
bool resetAfterCorruption(QString *backedUpPaths = nullptr);

} // namespace PlaylistStore

#endif // PLAYLISTSTORE_H
