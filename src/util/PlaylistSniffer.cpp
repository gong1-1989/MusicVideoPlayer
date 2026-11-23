/// @file PlaylistSniffer.cpp
/// @brief HLS 播放列表识别工具：按元数据与正文魔数判断，避免把普通媒体误当 m3u8。
#include "util/PlaylistSniffer.h"

namespace PlaylistSniffer {

/// @brief 按 Content-Type 或 URL 路径后缀判断是否播放列表。
/// @param url 资源地址。
/// @param contentType 响应类型，可为空。
/// @return 命中 `mpegurl` 类型或路径以 `.m3u8` 结尾时返回 true。
/// @note 只看 path，查询串里出现 `.m3u8` 的 MP4 不能误判；尾部多余斜杠先剥离。
bool metadataIndicatesPlaylist(const QUrl &url, const QString &contentType)
{
    if (contentType.toLower().contains(QLatin1String("mpegurl")))
        return true;
    // 只看 path 后缀。查询串里带 .m3u8（例如 CDN 的签名参数、跳转参数）
    // 的 MP4 不能因此被当成播放列表。部分源会在 playlist 地址末尾多一个
    // 斜杠，先去掉尾部斜杠再判断，否则 `index.m3u8/` 会漏判。
    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path.endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

/// @brief 检查正文是否以 `#EXTM3U` 开头。
/// @param body 原始响应正文。
/// @return 去除 UTF-8 BOM 与前导空白后命中魔数返回 true。
bool bodyIsPlaylist(const QByteArray &body)
{
    QByteArray head = body;
    if (head.startsWith("\xEF\xBB\xBF"))
        head.remove(0, 3);

    int offset = 0;
    while (offset < head.size()
           && (head.at(offset) == ' ' || head.at(offset) == '\t'
               || head.at(offset) == '\r' || head.at(offset) == '\n')) {
        ++offset;
    }
    if (offset >= head.size())
        return false;

    static const QByteArray magic = QByteArrayLiteral("#EXTM3U");
    return head.mid(offset, magic.size()) == magic;
}

} // namespace PlaylistSniffer
