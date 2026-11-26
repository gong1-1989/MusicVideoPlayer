#ifndef PAGEMEDIAEXTRACTOR_H
#define PAGEMEDIAEXTRACTOR_H

#include <QString>
#include <QUrl>

/// 从网页正文中提取可直接播放的媒体地址。
///
/// 影视源的 `vod_play_url` 经常给的是“网页播放页”，页面本身是 HTML，
/// 真正的 m3u8/MP4 写在播放器配置或 `<video>` 标签里。这里统一做静态解析，
/// 不执行网页脚本；识别不到的页面由调用方给出明确失败原因。
namespace PageMediaExtractor {

/// @param html 已按 UTF-8 解码的页面正文。
/// @param base 页面实际地址，用于把相对路径/协议相对地址补全。
/// @return 可播放的 http(s) 地址；没有命中时返回空 URL。
QUrl extractMediaUrl(const QString &html, const QUrl &base);

} // namespace PageMediaExtractor

#endif // PAGEMEDIAEXTRACTOR_H
