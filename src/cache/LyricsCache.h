#ifndef LYRICS_CACHE_H
#define LYRICS_CACHE_H

#include "media/MediaItem.h"

#include <QByteArray>
#include <QString>

/// 在线歌词磁盘缓存。正文保存歌词接口的原始响应体（JSON 包裹或纯 LRC），
/// 正文不带后缀，真实格式记录在紧邻的 `<正文>.json` 旁车元数据里。
/// 与播放缓存分开放在 `Data/lyrics-cache`，删除播放列表条目时一起清理。
namespace LyricsCache {

QString rootDir();
bool isEligible(const MediaItem &item);
/// 命中时返回原始响应体；调用方仍用 LyricsService 的同一套解析逻辑，
/// 因此 JSON 包裹和纯文本 LRC 两种上游格式都不需要复制解析代码。
bool load(const MediaItem &item, QByteArray *payload);
/// 身份、请求地址和旁车元数据都只取 item.lyricUrl。
/// 旧接口额外接收 lyricUrl，调用方一旦传入不同地址，缓存键与 load() 就会错位。
bool store(const MediaItem &item, const QByteArray &payload);
bool removeForItem(const MediaItem &item);

} // namespace LyricsCache

#endif // LYRICS_CACHE_H
