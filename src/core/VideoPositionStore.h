#ifndef VIDEOPOSITIONSTORE_H
#define VIDEOPOSITIONSTORE_H

#include "media/MediaItem.h"

#include <QString>

/// 视频播放进度存储：只记录 video 类型的条目，键为 MediaItem::mediaIdentity()。
/// 数据写在程序目录 Data/video_positions.json，不参与音乐播放。
namespace VideoPositionStore {

QString filePath();

/// 读取某条视频上次保存的播放位置；没有记录或已接近片尾时返回 0。
qint64 positionFor(const MediaItem &item);

/// 保存播放位置；位置太靠前或已到片尾时删除旧记录，避免下次从片头/片尾恢复。
bool save(const MediaItem &item, qint64 positionMs, qint64 durationMs);

/// 删除某条视频的进度记录。
bool remove(const MediaItem &item);

} // namespace VideoPositionStore

#endif // VIDEOPOSITIONSTORE_H
