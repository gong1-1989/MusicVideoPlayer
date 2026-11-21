#ifndef PLAYLISTSNIFFER_H
#define PLAYLISTSNIFFER_H

#include <QByteArray>
#include <QString>
#include <QUrl>

/// HLS 播放列表识别。StreamProxy 和 DownloadTask 必须共用同一套规则，
/// 否则会出现“播放能识别、下载识别不了”或反过来的分叉。
namespace PlaylistSniffer {

/// 响应头/URL 已经能确定是播放列表：
/// - Content-Type 含 mpegurl；
/// - 路径或完整 URL 含 .m3u8（签名地址可能把后缀放在 query）。
bool metadataIndicatesPlaylist(const QUrl &url, const QString &contentType);

/// 正文是否为 m3u8：允许 UTF-8 BOM 和前导空白，但必须以 #EXTM3U 开头。
bool bodyIsPlaylist(const QByteArray &body);

} // namespace PlaylistSniffer

#endif // PLAYLISTSNIFFER_H
