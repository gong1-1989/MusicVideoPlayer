#ifndef NETWORKADDRESSPOLICY_H
#define NETWORKADDRESSPOLICY_H

#include <QUrl>

/// 远程内容地址的可达性策略。
///
/// 搜索源返回的播放地址属于“远端不可信输入”。公网站点可以返回
/// `http://127.0.0.1:...` / `http://192.168.x.x` 之类的地址，如果客户端直接
/// 请求，就会变成对本机或内网的 SSRF。这里只做无 DNS 的静态判断：
/// 字面 IP、localhost 和常见保留域名直接识别；用户自己配置的本地源仍可播放
/// 本地地址。
namespace NetworkAddressPolicy {

/// 地址是否是本机回环、私网、链路本地、组播/未指定地址。
bool isLocalOrPrivateHost(const QUrl &url);

/// 目标是否允许作为搜索/下载返回的媒体地址。
/// - 非 http(s) 或没有 host：拒绝；
/// - 公网地址：允许；
/// - 本机/内网地址：只有可信源站本身也是本机/内网时才允许（本地源场景）。
/// @param sourceOrigin 可信源站（只要有 scheme+host 即可）。
bool allowsMediaTarget(const QUrl &target, const QUrl &sourceOrigin);

/// 只接受 http/https 且 host 非空的远程媒体地址。
bool isHttpMediaUrl(const QUrl &url);

} // namespace NetworkAddressPolicy

#endif // NETWORKADDRESSPOLICY_H
