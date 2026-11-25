/// @file HlsCacheBuilder.cpp
/// @brief 播放中 HLS 影视后台整段缓存的编排实现。
/// @details 状态机顺序为：主列表 → 等待播放器选码率 → 视频/音频媒体列表 →
///          并行下载轨道 → FFmpeg 转封装 → 写入缓存元数据。每次 start/cancel/
///          失败都会递增 generation，用于丢弃迟到的网络/下载回调。
#include "cache/HlsCacheBuilder.h"

#include "cache/MediaCache.h"
#include "cache/HlsPreloadMigrator.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "stream/HlsPlaylist.h"
#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

Q_LOGGING_CATEGORY(lcHlsCache, "mvp.hls.cache")

namespace {

/// @brief 生成用于日志的变体描述：分辨率 / 码率 / 音频组。
QString variantLabel(const HlsCacheManifest::Variant &variant)
{
    QStringList parts;
    if (!variant.resolution.isEmpty())
        parts.append(variant.resolution);
    if (variant.bandwidth > 0)
        parts.append(QStringLiteral("%1 Mbps").arg(variant.bandwidth / 1000000.0, 0, 'f', 2));
    if (!variant.audioGroup.isEmpty())
        parts.append(QStringLiteral("audio=%1").arg(variant.audioGroup));
    return parts.join(QStringLiteral(" / "));
}

/// @brief 描述单条轨道的下载方式，便于日志区分整段 Range 与分片合并。
QString trackSourceLabel(bool direct)
{
    return direct ? QStringLiteral("单文件 BYTERANGE → MP4 Range")
                  : QStringLiteral("分片列表 → 合并");
}

} // namespace

/// @brief 构造 HLS 后台缓存器。
/// @param http 共享 HTTP 客户端，不拥有；为空时 start() 直接失败。
/// @param parent 父对象，缓存器与子任务都挂在它下面。
HlsCacheBuilder::HlsCacheBuilder(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_variantTimer(new QTimer(this))
{
    m_variantTimer->setInterval(250);
    connect(m_variantTimer, &QTimer::timeout, this, &HlsCacheBuilder::onVariantTick);
}

/// @brief 析构时中止所有网络请求、下载与转码，保留半成品供下次续传。
HlsCacheBuilder::~HlsCacheBuilder()
{
    stopForExit();
    // 进程正常退出时，当前会话已经结束；完成缓存后特意保留的轨道/索引
    // 可以安全回收，避免磁盘上长期留下双份影视文件。
    cleanupRetainedTracks();
}

/// @brief 设置播放器当前实际打开的视频/音频播放列表查询函数。
/// @param video 返回当前视频轨 URL 的回调。
/// @param audio 返回当前音频轨 URL 的回调。
/// @note 用于让后台缓存跟随用户实际选中的码率，而不是盲选最高码率。
void HlsCacheBuilder::setPlaybackTrackResolvers(UrlResolver video, UrlResolver audio)
{
    m_videoResolver = std::move(video);
    m_audioResolver = std::move(audio);
}

/// @brief 设置后台 HLS 分片的本地流转地址解析器。
/// @note 仅在代理已就绪时设置；回调返回空地址表示本次回退直连。分片索引
///       仍记录原始源地址，所有续传指纹也仍按原播放列表计算。
void HlsCacheBuilder::setProxyUrlResolver(ProxyUrlResolver resolver)
{
    m_proxyUrlResolver = std::move(resolver);
}

/// @brief 控制完成后是否延迟清理轨道/索引。
/// @note 主窗口的播放会话需要保留，否则当前播放会在缓存完成瞬间失去本地分片；
///       测试或离线缓存任务没有播放会话，保持默认 false 立即清理中间文件。
void HlsCacheBuilder::setRetainCompletedTrackFiles(bool retain)
{
    m_retainCompletedTrackFiles = retain;
}

/// @brief 开始为指定在线 HLS 影视条目构建完整缓存。
/// @param item 当前播放条目，必须是在线视频且是 HLS。
/// @param sourceUrl 播放器实际使用的上游地址；空时回退到条目的首选地址。
/// @return 已启动返回 true；非 HLS 或已有完整缓存返回 false。
/// @note 已有完整缓存时直接跳过；未完成缓存保留现场，下次从续传点继续。
bool HlsCacheBuilder::start(const MediaItem &item, const QUrl &sourceUrl)
{
    // 上一条目如果已经生成完整 MP4，但当前会话仍可能从轨道分片索引继续读，
    // 只有切换到不同条目时才清理。同一部视频缓存完成后再次 start() 不能把
    // 正在使用的索引删掉，否则播放会立刻退回直连。
    const QString nextBody = item.isValid() ? MediaCache::filePathForItem(item) : QString();
    if (!m_retainedBasePath.isEmpty()
        && (nextBody != m_retainedBasePath
            || MediaCache::completeFileForItem(item).isEmpty())) {
        cleanupRetainedTracks();
    }

    // 切换条目时保留上一条目的半成品，不再用 cancel() 删除它。
    stopForExit();

    // HLS 影视才需要整段缓存；直链 MP4 已有 StreamProxy 的完整文件落盘。
    // 必须看播放器实际地址：主线路失败换到备用 mp4 时，不能继续按条目的
    // 首选 m3u8 去缓存。
    if (m_http == nullptr || !item.online || item.type != MediaType::Video
        || !item.isValid()) {
        return false;
    }
    const QUrl actualSource = sourceUrl.isValid() && !sourceUrl.isEmpty()
                                  ? sourceUrl
                                  : preferredPlaybackUrl(item);
    if (!DownloadTask::looksLikeHls(actualSource))
        return false;

    if (!MediaCache::completeFileForItem(item).isEmpty()) {
        qCDebug(lcHlsCache) << "HLS 完整缓存已存在，跳过：" << item.displayName();
        return false;
    }

    m_item = item;
    m_sourceUrl = actualSource;
    QUrl origin = item.sourceOrigin.isValid() && !item.sourceOrigin.host().isEmpty()
                      ? item.sourceOrigin
                      : HeaderPolicy::originFromHeaders(item.headers);
    // 条目没有记录源站时，用真实播放地址补上；否则公网 HLS 的 m_originUrl
    // 为空，HttpClient 会把“无源站”当成兼容模式，内网分片就失去防护。
    if (origin.isEmpty() || origin.host().isEmpty())
        origin = HeaderPolicy::originOnly(actualSource);
    m_originUrl = origin;
    // 未完成缓存不再提前删除：轨道文件和 `.mvpdownload` 就是续传现场，
    // 是否真正可续由 DownloadTask 的分片指纹校验决定。
    m_bodyPath = MediaCache::filePathForItem(item);
    m_videoPath = m_bodyPath + QStringLiteral(".video");
    m_audioPath = m_bodyPath + QStringLiteral(".audio");
    m_identity = MediaCache::cacheIdentity(item);
    m_videoSegments.clear();
    m_audioSegments.clear();
    m_videoHasByteRange = false;
    m_audioHasByteRange = false;
    if (!QDir().mkpath(QFileInfo(m_bodyPath).absolutePath()))
        return false;
    // 完成前没有旁车 JSON，必须靠这个身份标记让下次启动找回同一目录；
    // 否则非空目录会被当成冲突，改选 " (2)" 后从头下载。
    if (!MediaCache::writePartialIdentity(m_bodyPath, MediaCache::cacheIdentity(item))) {
        qCWarning(lcHlsCache) << "无法写入未完成缓存身份标记，跳过本次后台缓存："
                              << item.displayName();
        return false;
    }

    // 加载同一缓存目录上次留下的分片索引：播放代理据此把已下载字节直接
    // 当作分片响应，断点续传后的前半段也能在本次启动继续命中。
    // 没有可续现场的那条轨道必须清掉旧索引：新任务会从 0 重写轨道文件，
    // 旧偏移继续命中就会把错位字节当成该分片返回。
    const TrackResume videoIndexResume =
        loadTrackResume(m_bodyPath + QStringLiteral(".video"));
    const TrackResume audioIndexResume =
        loadTrackResume(m_bodyPath + QStringLiteral(".audio"));
    HlsPreloadIndex::instance().openJournal(m_bodyPath + QStringLiteral(".preload.jsonl"));
    if (!(videoIndexResume.bytesWritten > 0 && !videoIndexResume.fingerprint.isEmpty())) {
        const QString target = videoIndexResume.targetPath.isEmpty()
                                   ? m_bodyPath + QStringLiteral(".video")
                                   : videoIndexResume.targetPath;
        HlsPreloadIndex::instance().clearTrack(target);
    }
    if (!(audioIndexResume.bytesWritten > 0 && !audioIndexResume.fingerprint.isEmpty())) {
        const QString target = audioIndexResume.targetPath.isEmpty()
                                   ? m_bodyPath + QStringLiteral(".audio")
                                   : audioIndexResume.targetPath;
        HlsPreloadIndex::instance().clearTrack(target);
    }

    m_active = true;
    m_stage = Stage::FetchingMaster;
    ++m_generation;
    m_master = HlsCacheManifest::Master();
    m_videoTrack = HlsCacheManifest::MediaTrack();
    m_audioTrack = HlsCacheManifest::MediaTrack();
    m_videoPlaylistUrl = QUrl();
    m_audioPlaylistUrl = QUrl();
    m_videoSource = QUrl();
    m_audioSource = QUrl();
    m_masterBaseUrl = QUrl();
    m_videoDirect = false;
    m_audioDirect = false;
    m_hasAudio = false;
    m_videoDone = false;
    m_audioDone = false;
    m_videoPct = -1;
    m_audioPct = -1;
    m_videoReceived = 0;
    m_videoTotal = 0;
    m_audioReceived = 0;
    m_audioTotal = 0;
    m_lastOverallPct = -1;
    m_lastProgressMs = 0;

    qCInfo(lcHlsCache) << "开始后台缓存 HLS：" << item.displayName()
                       << HttpClient::redactUrl(m_sourceUrl);
    fetchPlaylist(m_sourceUrl, [this](const QByteArray &body, const QUrl &finalUrl,
                                      const QString &error) {
        onMasterReady(body, finalUrl, error);
    });
    return true;
}

/// @brief 拉取一个播放列表文本，并保证回调只执行一次。
/// @param url 播放列表地址。
/// @param callback 结果回调，参数为正文、最终 URL、错误文本。
/// @note 同一时刻只保留一个播放列表请求；新请求会中止旧请求。generation
///       不匹配或对象已销毁时，迟到的响应会被直接丢弃。
void HlsCacheBuilder::fetchPlaylist(const QUrl &url, const PlaylistCallback &callback)
{
    if (!m_active || m_http == nullptr || !url.isValid())
        return;

    abortPlaylistFetch();
    const qint64 generation = m_generation;
    const QHash<QString, QString> headers =
        HeaderPolicy::sanitizedForTarget(url, m_item.headers, m_originUrl);
    QPointer<HlsCacheBuilder> self(this);
    m_http->fetch(
        url, QByteArray(), headers,
        [self, generation, url, callback](QNetworkReply *reply) {
            if (self.isNull() || generation != self->m_generation) {
                if (reply != nullptr) {
                    reply->abort();
                    reply->deleteLater();
                }
                return;
            }
            if (reply == nullptr) {
                callback(QByteArray(), url, QObject::tr("播放列表请求失败（重定向或协议不允许）"));
                return;
            }

            self->m_playlistReply = reply;
            auto finished = std::make_shared<bool>(false);
            auto deliver = [self, generation, reply, callback, finished] {
                if (*finished)
                    return;
                *finished = true;
                if (self.isNull() || generation != self->m_generation) {
                    reply->abort();
                    reply->deleteLater();
                    return;
                }
                if (self->m_playlistReply == reply)
                    self->m_playlistReply = nullptr;

                const QUrl finalUrl = reply->url();
                if (!HttpClient::isHttpSuccess(reply)) {
                    const QString error = HttpClient::describeError(reply);
                    reply->deleteLater();
                    callback(QByteArray(), finalUrl, error);
                    return;
                }
                bool tooLarge = false;
                const QByteArray body =
                    HttpClient::readAllLimited(reply, NetLimits::kPlaylistBytes, &tooLarge);
                reply->deleteLater();
                if (tooLarge) {
                    callback(QByteArray(), finalUrl, QObject::tr("播放列表超过大小上限"));
                    return;
                }
                if (body.isEmpty()) {
                    callback(QByteArray(), finalUrl, QObject::tr("播放列表为空"));
                    return;
                }
                callback(body, finalUrl, QString());
            };
            QObject::connect(reply, &QNetworkReply::finished, self,
                             [deliver] { deliver(); });
            if (reply->isFinished())
                deliver();
        },
        NetLimits::kPlaylistBytes, m_originUrl);
}

/// @brief 处理主播放列表响应。
/// @note parseMaster 成功且有变体时进入选码率阶段；若确实识别为主列表但
///       没有可用码率，必须直接失败，不能退回去当媒体列表解析。
void HlsCacheBuilder::onMasterReady(const QByteArray &body, const QUrl &finalUrl,
                                    const QString &error)
{
    if (!m_active || m_stage != Stage::FetchingMaster)
        return;
    if (!error.isEmpty()) {
        finishFailed(error);
        return;
    }

    HlsCacheManifest::Master master;
    QString masterError;
    if (HlsCacheManifest::parseMaster(body, finalUrl, &master, &masterError)) {
        if (master.variants.isEmpty()) {
            finishFailed(masterError.isEmpty() ? tr("主播放列表里没有可用码率") : masterError);
            return;
        }
        m_master = master;
        m_masterBaseUrl = finalUrl;
        m_stage = Stage::WaitingVariant;
        beginVariantSelection();
        return;
    }
    // parseMaster 返回 false 有两种情况：不是主列表，或确实是主列表但没有
    // 可用码率。后者带 masterError，必须直接报错，不能退回去当媒体列表解析。
    if (!masterError.isEmpty()) {
        finishFailed(masterError);
        return;
    }

    // 原始地址本身就是媒体列表，没有码率可选，直接按视频轨处理。
    HlsCacheManifest::MediaTrack track;
    QString trackError;
    if (!HlsCacheManifest::parseMediaTrack(body, finalUrl, &track, &trackError)) {
        finishFailed(trackError.isEmpty() ? tr("不是可识别的 HLS 播放列表") : trackError);
        return;
    }
    if (!track.hasEndList && !track.isVod) {
        finishFailed(tr("播放列表没有结束标记，可能是直播流，未生成不完整缓存"));
        return;
    }
    m_videoPlaylistUrl = finalUrl;
    m_videoTrack = track;
    m_videoDirect = track.singleFile;
    m_videoSource = track.singleFile ? track.singleFileUrl : finalUrl;
    m_hasAudio = false;
    qCInfo(lcHlsCache) << "源是媒体列表，直接缓存："
                       << (track.singleFile ? "单文件 BYTERANGE" : "普通分片")
                       << "分片数" << track.segmentCount;
    startDownloads();
}

/// @brief 进入码率选择阶段；没有播放器 URL 回调时直接退回第一个变体。
void HlsCacheBuilder::beginVariantSelection()
{
    if (!m_active || m_stage != Stage::WaitingVariant)
        return;

    if (!m_videoResolver) {
        selectVariant(QUrl(), QString());
        return;
    }

    m_variantStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_variantStableSinceMs = 0;
    m_variantCandidate = QUrl();
    m_variantTimer->start();
}

/// @brief 每 250ms 检查播放器实际打开的变体是否稳定。
/// @note 同一个变体连续稳定 2 秒后开始缓存；8 秒内曾稳定过则采用；10 秒
///       仍无法识别时回退主列表第一个变体，避免后台缓存永远不开始。
void HlsCacheBuilder::onVariantTick()
{
    if (!m_active || m_stage != Stage::WaitingVariant) {
        m_variantTimer->stop();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QUrl candidate = m_videoResolver ? m_videoResolver() : QUrl();
    bool known = false;
    if (candidate.isValid()) {
        for (const auto &variant : std::as_const(m_master.variants)) {
            if (HlsCacheManifest::sameUrlIgnoringQuery(variant.url, candidate)) {
                known = true;
                break;
            }
        }
    }

    if (known) {
        if (!HlsCacheManifest::sameUrlIgnoringQuery(m_variantCandidate, candidate)) {
            m_variantCandidate = candidate;
            m_variantStableSinceMs = now;
        }
        if (now - m_variantStableSinceMs >= 2000 || now - m_variantStartedMs >= 8000) {
            m_variantTimer->stop();
            selectVariant(candidate, audioGroupForVariant(candidate));
            return;
        }
    }

    if (now - m_variantStartedMs >= 10000) {
        m_variantTimer->stop();
        qCWarning(lcHlsCache) << "未能稳定识别播放码率，回退主列表第一个变体";
        selectVariant(QUrl(), QString());
    }
}

/// @brief 查指定视频变体关联的音频组 id。
/// @return 找到匹配变体时返回 audioGroup；未知 URL 返回空。
QString HlsCacheBuilder::audioGroupForVariant(const QUrl &videoUrl) const
{
    if (!videoUrl.isValid())
        return QString();
    for (const auto &variant : m_master.variants) {
        if (HlsCacheManifest::sameUrlIgnoringQuery(variant.url, videoUrl))
            return variant.audioGroup;
    }
    return QString();
}

/// @brief 确定要缓存的视频变体及其独立音轨 URL。
/// @param videoUrl 播放器当前打开的视频播放列表；无效时选第一个变体。
/// @param audioGroup 优先匹配的音频组 id；为空时按播放器音频轨/变体标签推断。
void HlsCacheBuilder::selectVariant(const QUrl &videoUrl, const QString &audioGroup)
{
    if (!m_active || m_master.variants.isEmpty())
        return;

    QUrl trackerAudio = m_audioResolver ? m_audioResolver() : QUrl();
    QString trackerAudioGroup;
    for (const auto &audio : std::as_const(m_master.audioRenditions)) {
        if (trackerAudio.isValid()
            && HlsCacheManifest::sameUrlIgnoringQuery(audio.url, trackerAudio)) {
            trackerAudioGroup = audio.groupId;
            break;
        }
    }

    HlsCacheManifest::Variant chosen;
    bool found = false;
    QString group = audioGroup.isEmpty() ? trackerAudioGroup : audioGroup;
    if (videoUrl.isValid()) {
        for (const auto &variant : std::as_const(m_master.variants)) {
            if (!HlsCacheManifest::sameUrlIgnoringQuery(variant.url, videoUrl))
                continue;
            if (!found || (!group.isEmpty() && variant.audioGroup == group)) {
                chosen = variant;
                found = true;
            }
        }
    }
    if (!found) {
        chosen = m_master.variants.first();
        group = chosen.audioGroup;
        qCWarning(lcHlsCache) << "播放器码率不在主列表，回退第一个变体"
                              << HttpClient::redactUrl(chosen.url);
    }
    m_videoPlaylistUrl = chosen.url;

    if (chosen.audioGroup.isEmpty() && trackerAudioGroup.isEmpty()) {
        m_audioPlaylistUrl = QUrl();
    } else {
        if (chosen.audioGroup.isEmpty())
            chosen.audioGroup = trackerAudioGroup;
        if (chosen.audioGroup.isEmpty())
            chosen.audioGroup = group;
        for (const auto &audio : std::as_const(m_master.audioRenditions)) {
            if (HlsCacheManifest::sameUrlIgnoringQuery(audio.url, trackerAudio)
                && !trackerAudio.isEmpty()) {
                m_audioPlaylistUrl = audio.url;
                break;
            }
            if (!audio.groupId.isEmpty() && audio.groupId == chosen.audioGroup) {
                if (!m_audioPlaylistUrl.isValid() || audio.isDefault)
                    m_audioPlaylistUrl = audio.url;
            }
        }
    }
    m_hasAudio = m_audioPlaylistUrl.isValid() && !m_audioPlaylistUrl.isEmpty();

    qCInfo(lcHlsCache) << "跟随当前播放码率：" << variantLabel(chosen)
                       << HttpClient::redactUrl(m_videoPlaylistUrl);
    if (m_hasAudio) {
        qCInfo(lcHlsCache) << "独立音轨：" << HttpClient::redactUrl(m_audioPlaylistUrl);
    }

    m_stage = Stage::FetchingVideoPlaylist;
    fetchPlaylist(m_videoPlaylistUrl, [this](const QByteArray &body, const QUrl &finalUrl,
                                             const QString &error) {
        onVideoPlaylistReady(body, finalUrl, error);
    });
}

/// @brief 解析视频媒体列表；有独立音轨时继续拉音频列表，否则直接开始下载。
void HlsCacheBuilder::onVideoPlaylistReady(const QByteArray &body, const QUrl &finalUrl,
                                           const QString &error)
{
    if (!m_active || m_stage != Stage::FetchingVideoPlaylist)
        return;
    if (!error.isEmpty()) {
        finishFailed(tr("视频播放列表下载失败：%1").arg(error));
        return;
    }

    HlsCacheManifest::MediaTrack track;
    QString trackError;
    if (!HlsCacheManifest::parseMediaTrack(body, finalUrl, &track, &trackError)) {
        finishFailed(trackError.isEmpty() ? tr("视频播放列表无法解析") : trackError);
        return;
    }
    if (!track.hasEndList && !track.isVod) {
        finishFailed(tr("视频播放列表没有结束标记，可能是直播流，未生成不完整缓存"));
        return;
    }
    if (m_master.encrypted) {
        // SESSION-KEY 只写在主列表；子列表看起来是“全 BYTERANGE 同 URL”，
        // 但每个 Range 仍是独立密文，不能走整段 MP4 Range 直下。
        track.encrypted = true;
        track.singleFile = false;
        track.singleFileUrl = QUrl();
    }
    m_videoTrack = track;
    m_videoDirect = track.singleFile;
    m_videoSource = track.singleFile ? track.singleFileUrl : finalUrl;
    // 记录当前播放列表的真实分片顺序，供旧缓存安全迁移探测长度使用。
    m_videoSegments.clear();
    if (const auto info = HlsPlaylist::parse(body, finalUrl))
        m_videoSegments = info->segments;
    m_videoHasByteRange = body.contains(QByteArrayLiteral("#EXT-X-BYTERANGE:"));
    if (track.encrypted) {
        // DownloadTask 落盘的是解密后的明文分片，而播放列表仍带 EXT-X-KEY；
        // 若播放代理用明文命中索引，FFmpeg 还会再解一次密，必须禁用该索引。
        HlsPreloadIndex::instance().openJournal(QString());
        if (!m_bodyPath.isEmpty())
            QFile::remove(m_bodyPath + QStringLiteral(".preload.jsonl"));
        qCInfo(lcHlsCache) << "加密视频轨不使用分片预加载索引";
    }

    qCInfo(lcHlsCache) << "视频轨：" << trackSourceLabel(m_videoDirect)
                       << "分片数" << track.segmentCount
                       << HttpClient::redactUrl(m_videoSource);

    if (m_hasAudio) {
        m_stage = Stage::FetchingAudioPlaylist;
        fetchPlaylist(m_audioPlaylistUrl, [this](const QByteArray &audioBody,
                                                 const QUrl &audioFinal,
                                                 const QString &audioError) {
            onAudioPlaylistReady(audioBody, audioFinal, audioError);
        });
        return;
    }
    startDownloads();
}

/// @brief 解析独立音轨媒体列表，成功后启动视频/音轨并行下载。
void HlsCacheBuilder::onAudioPlaylistReady(const QByteArray &body, const QUrl &finalUrl,
                                           const QString &error)
{
    if (!m_active || m_stage != Stage::FetchingAudioPlaylist)
        return;
    if (!error.isEmpty()) {
        finishFailed(tr("音轨播放列表下载失败：%1").arg(error));
        return;
    }

    HlsCacheManifest::MediaTrack track;
    QString trackError;
    if (!HlsCacheManifest::parseMediaTrack(body, finalUrl, &track, &trackError)) {
        finishFailed(trackError.isEmpty() ? tr("音轨播放列表无法解析") : trackError);
        return;
    }
    if (!track.hasEndList && !track.isVod) {
        finishFailed(tr("音轨播放列表没有结束标记，未生成不完整缓存"));
        return;
    }
    if (m_master.encrypted) {
        track.encrypted = true;
        track.singleFile = false;
        track.singleFileUrl = QUrl();
    }
    m_audioTrack = track;
    m_audioDirect = track.singleFile;
    m_audioSource = track.singleFile ? track.singleFileUrl : finalUrl;
    m_audioSegments.clear();
    if (const auto info = HlsPlaylist::parse(body, finalUrl))
        m_audioSegments = info->segments;
    m_audioHasByteRange = body.contains(QByteArrayLiteral("#EXT-X-BYTERANGE:"));
    if (track.encrypted) {
        HlsPreloadIndex::instance().openJournal(QString());
        if (!m_bodyPath.isEmpty())
            QFile::remove(m_bodyPath + QStringLiteral(".preload.jsonl"));
        qCInfo(lcHlsCache) << "加密音轨不使用分片预加载索引";
    }

    qCInfo(lcHlsCache) << "音轨：" << trackSourceLabel(m_audioDirect)
                       << "分片数" << track.segmentCount
                       << HttpClient::redactUrl(m_audioSource);
    startDownloads();
}

/// @brief 读取轨道续传现场并启动下载。
/// @note 视频轨和独立音轨并行下载；没有音轨时直接把 audioDone 置为 true，
///       让 maybeRemux() 只等待视频轨。退出前已下完的轨道不再重下，直接进入转封装。
void HlsCacheBuilder::startDownloads()
{
    if (!m_active)
        return;

    const QString videoBase = m_bodyPath + QStringLiteral(".video");
    const QString audioBase = m_bodyPath + QStringLiteral(".audio");
    m_videoPath = videoBase;
    m_audioPath = audioBase;
    // 上次退出若停在 FFmpeg 转封装，只丢弃无用的转封装临时文件；两条轨道保留。
    QFile::remove(m_bodyPath + QStringLiteral(".mvpremux"));

    m_stage = Stage::Downloading;
    m_videoDone = false;
    m_audioDone = false;
    m_videoPct = -1;
    m_audioPct = -1;
    m_videoReceived = 0;
    m_videoTotal = 0;
    m_audioReceived = 0;
    m_audioTotal = 0;
    m_lastOverallPct = -1;

    if (m_videoSource.isEmpty()) {
        finishFailed(tr("没有找到可下载的视频地址"));
        return;
    }

    m_videoResume = loadTrackResume(videoBase);
    const QString videoTarget = m_videoResume.valid && !m_videoResume.targetPath.isEmpty()
                                    ? m_videoResume.targetPath
                                    : videoBase;
    if (m_videoResume.valid && m_videoResume.completed && QFileInfo::exists(videoTarget)) {
        // 退出发生在两条轨道都下完、FFmpeg 尚未收尾时：直接复用成品轨道。
        m_videoPath = videoTarget;
        m_videoDone = true;
        m_videoPct = 100;
        m_videoTotal = QFileInfo(videoTarget).size();
        m_videoReceived = m_videoTotal;
        qCInfo(lcHlsCache) << "复用上次已下载完成的视频轨：" << videoTarget;
    } else if (!startTrack(true, m_videoSource, videoBase)) {
        return;
    }

    if (m_hasAudio) {
        if (m_audioSource.isEmpty()) {
            finishFailed(tr("没有找到可下载的音轨地址"));
            return;
        }
        m_audioResume = loadTrackResume(audioBase);
        const QString audioTarget = m_audioResume.valid && !m_audioResume.targetPath.isEmpty()
                                        ? m_audioResume.targetPath
                                        : audioBase;
        if (m_audioResume.valid && m_audioResume.completed
            && QFileInfo::exists(audioTarget)) {
            m_audioPath = audioTarget;
            m_audioDone = true;
            m_audioPct = 100;
            m_audioTotal = QFileInfo(audioTarget).size();
            m_audioReceived = m_audioTotal;
            qCInfo(lcHlsCache) << "复用上次已下载完成的音轨：" << audioTarget;
        } else if (!startTrack(false, m_audioSource, audioBase)) {
            return;
        }
    } else {
        m_audioDone = true;
    }

    // 两条轨道都可能直接复用上次成果，此时不必等下载信号，立即转封装。
    if (m_videoDone && m_audioDone)
        maybeRemux();
}

/// @brief 为单条轨道创建 DownloadTask 并连接进度/完成/失败信号。
/// @param video true 表示视频轨，false 表示独立音轨。
/// @param source 轨道源地址（媒体列表或单文件 BYTERANGE 地址）。
/// @param basePath 轨道固定基名（`.video` / `.audio`），续传状态按它保存。
/// @return 任务启动成功返回 true；失败时已走 finishFailed()。
bool HlsCacheBuilder::startTrack(bool video, const QUrl &source, const QString &basePath)
{
    TrackResume resume = loadTrackResume(basePath);
    const QString targetPath = resume.valid && !resume.targetPath.isEmpty()
                                   ? resume.targetPath
                                   : basePath;
    const bool hlsTrack = video ? !m_videoDirect : !m_audioDirect;
    DownloadTask *task = DownloadTask::start(m_http, source, targetPath, m_item.headers, this);
    if (task == nullptr) {
        finishFailed(video ? tr("无法启动视频缓存任务") : tr("无法启动音轨缓存任务"));
        return false;
    }
    // HLS 媒体分片不再由后台任务直接回源，而是登记到播放器同一个流转代理：
    // 播放已经拉过的分片直接复用代理缓存，尚未拉过的分片也由代理统一排队，
    // 从根源上避免播放和整段缓存各自下载同一片、互抢带宽。
    // 单文件 BYTERANGE 轨道（m_*Direct）保留整段 Range 直下，不走分片映射。
    if (hlsTrack && m_proxyUrlResolver) {
        task->setHlsSegmentUrlMapper(
            [resolver = m_proxyUrlResolver, headers = m_item.headers,
             identity = m_identity, origin = m_originUrl](const QUrl &segmentUrl) {
                return resolver(segmentUrl, headers, QString(), identity, origin);
            });
    }
    if (video) {
        m_videoTask = task;
        m_videoPath = targetPath;
    } else {
        m_audioTask = task;
        m_audioPath = targetPath;
    }

    // 先把固定基名对应的现场登记好，后续 checkpoint 不等同于内存里的路径。
    resume.valid = true;
    resume.completed = false;
    resume.targetPath = targetPath;
    if (video)
        m_videoResume = resume;
    else
        m_audioResume = resume;
    saveTrackResume(basePath, resume);

    task->setMaxBytes(std::numeric_limits<qint64>::max());
    task->setAllowIncompletePlaylist(false);
    task->setOriginUrl(m_originUrl);
    for (const QString &line : std::as_const(m_master.sessionKeyLines))
        task->setHlsSessionKey(line, m_masterBaseUrl);
    // HLS 分片必须同时有指纹和分片序号；单文件 Range 轨道只续传字节偏移。
    if (hlsTrack && (resume.bytesWritten > 0 || resume.segmentIndex > 0)
        && !resume.fingerprint.isEmpty()) {
        task->setResumeState(resume.bytesWritten, resume.segmentIndex, true);
        task->setResumeFingerprint(resume.fingerprint);
        qCInfo(lcHlsCache) << "HLS 轨道续传：" << basePath
                           << "bytes=" << resume.bytesWritten
                           << "segments=" << resume.segmentIndex << "/" << resume.segmentCount;
    } else if (!hlsTrack && resume.bytesWritten > 0) {
        task->setResumeState(resume.bytesWritten, 0, false);
        qCInfo(lcHlsCache) << "单文件轨道续传：" << basePath
                           << "bytes=" << resume.bytesWritten;
    }
    connect(task, &DownloadTask::targetPathChanged, this,
            [this, video, basePath](const QString &path, bool) {
                if (video)
                    m_videoPath = path;
                else
                    m_audioPath = path;
                TrackResume &state = video ? m_videoResume : m_audioResume;
                state.valid = true;
                state.targetPath = path;
                saveTrackResume(basePath, state);
            });
    connect(task, &DownloadTask::checkpoint, this,
            [this, video, basePath](qint64 bytesWritten, int segmentIndex, int segmentCount) {
                TrackResume &state = video ? m_videoResume : m_audioResume;
                state.valid = true;
                state.completed = false;
                state.bytesWritten = qMax<qint64>(0, bytesWritten);
                state.segmentIndex = qMax(0, segmentIndex);
                if (segmentCount >= 0)
                    state.segmentCount = segmentCount;
                saveTrackResume(basePath, state);
            });
    connect(task, &DownloadTask::playlistFingerprint, this,
            [this, video, basePath](const QString &fingerprint) {
                TrackResume &state = video ? m_videoResume : m_audioResume;
                // 指纹变化意味着分片结构和旧合并文件不再对应，旧索引必须在
                // 新分片登记前失效，不能只等 DownloadTask 稍后清零文件。
                if (state.bytesWritten > 0 && !state.fingerprint.isEmpty()
                    && state.fingerprint != fingerprint) {
                    const QString target = state.targetPath.isEmpty()
                                               ? (video ? m_videoPath : m_audioPath)
                                               : state.targetPath;
                    HlsPreloadIndex::instance().clearTrack(target);
                }
                state.valid = true;
                state.fingerprint = fingerprint;
                saveTrackResume(basePath, state);
                // 旧缓存迁移必须等新播放列表算完指纹；只有分片结构完全一致
                // 才允许把旧合并文件按当前分片顺序建立索引。
                maybeStartMigration(video, fingerprint);
            });
    connect(task, &DownloadTask::targetResetted, this, [this, video] {
        const QString trackPath = video ? m_videoPath : m_audioPath;
        if (!trackPath.isEmpty() && !m_identity.isEmpty())
            HlsPreloadIndex::instance().clearTrack(trackPath);
    });
    connect(task, &DownloadTask::segmentWritten, this,
            [this, video](const QUrl &url, const QByteArray &range, qint64 offset, qint64 size) {
                // DownloadTask 在完成分片时写的是 `.mvpdownload`；索引路径也要
                // 指向临时文件，否则下载期间播放代理读不到刚下好的分片。
                const QString trackPath = video ? m_videoPath : m_audioPath;
                if (trackPath.isEmpty() || m_identity.isEmpty())
                    return;
                // 加密轨道落盘的是明文，播放列表却仍要求 FFmpeg 解密；
                // 这类分片绝不能进索引，否则会被二次解密成坏数据。
                if (video ? m_videoTrack.encrypted : m_audioTrack.encrypted)
                    return;
                HlsPreloadEntry entry;
                entry.url = url;
                entry.range = range;
                entry.filePath = trackPath + QStringLiteral(".mvpdownload");
                entry.offset = offset;
                entry.size = size;
                entry.contentType = QByteArrayLiteral("video/mp2t");
                HlsPreloadIndex::instance().record(m_identity, entry);
            });
    connect(task, &DownloadTask::progress, this,
            [this, video](qint64 received, qint64 total) {
                onTrackProgress(video, received, total);
            });
    connect(task, &DownloadTask::succeeded, this,
            [this, video, basePath](const QString &path) {
                TrackResume &state = video ? m_videoResume : m_audioResume;
                state.valid = true;
                state.completed = true;
                state.targetPath = path;
                state.bytesWritten = QFileInfo(path).size();
                state.segmentCount = state.segmentIndex;
                saveTrackResume(basePath, state);
                onTrackFinished(video, path);
            });
    connect(task, &DownloadTask::failed, this,
            [this, video](const QString &message) { onTrackFailed(video, message); });
    return true;
}

/// @brief 旧版合并缓存的安全迁移入口。
/// @note 只在以下条件全部满足时启动：续传指纹与新播放列表一致、分片数一致、
///       轨道未加密且不是单文件 BYTERANGE、旧 `.mvpdownload` 至少包含记录前缀。
///       任一条件不满足就放弃迁移，保留旧文件但绝不用估算偏移冒充缓存。
void HlsCacheBuilder::maybeStartMigration(bool video, const QString &fingerprint)
{
    if (!m_active || m_http == nullptr || fingerprint.isEmpty() || m_identity.isEmpty())
        return;
    const TrackResume &resume = video ? m_videoResume : m_audioResume;
    const HlsCacheManifest::MediaTrack &track = video ? m_videoTrack : m_audioTrack;
    const QList<QUrl> &segments = video ? m_videoSegments : m_audioSegments;
    const QPointer<HlsPreloadMigrator> &slot = video ? m_videoMigrator : m_audioMigrator;
    const QString target = resume.targetPath.isEmpty()
                               ? (video ? m_videoPath : m_audioPath)
                               : resume.targetPath;

    if (!resume.valid || resume.completed || resume.bytesWritten <= 0
        || resume.segmentIndex <= 0 || resume.segmentIndex > segments.size())
        return;
    if (resume.fingerprint.isEmpty() || resume.fingerprint != fingerprint) {
        qCInfo(lcHlsCache) << "旧缓存迁移跳过：续传指纹与当前播放列表不一致"
                           << (video ? "视频轨" : "音轨");
        return;
    }
    if (resume.segmentCount != segments.size()) {
        qCInfo(lcHlsCache) << "旧缓存迁移跳过：续传分片数与当前播放列表不一致"
                           << resume.segmentCount << segments.size();
        return;
    }
    if (track.singleFile || track.encrypted
        || (video ? m_videoHasByteRange : m_audioHasByteRange)) {
        qCInfo(lcHlsCache) << "旧缓存迁移跳过：加密、单文件或 BYTERANGE 无法取得"
                              "与落盘一致的分片边界"
                           << (video ? "视频轨" : "音轨");
        return;
    }
    if (HlsPreloadIndex::instance().countTrack(m_identity, target) > 0) {
        qCInfo(lcHlsCache) << "旧缓存已有分片索引，跳过迁移" << (video ? "视频轨" : "音轨");
        return;
    }
    if (!slot.isNull())
        return;

    QString trackPath = target + QStringLiteral(".mvpdownload");
    if (!QFileInfo::exists(trackPath) && QFileInfo::exists(target))
        trackPath = target;
    if (!QFileInfo::exists(trackPath)) {
        qCInfo(lcHlsCache) << "旧缓存迁移跳过：找不到旧轨道文件" << trackPath;
        return;
    }
    const qint64 actualBytes = QFileInfo(trackPath).size();
    if (actualBytes < resume.bytesWritten) {
        qCWarning(lcHlsCache) << "旧缓存迁移跳过：轨道文件短于续传记录"
                              << actualBytes << resume.bytesWritten;
        return;
    }

    const qint64 generation = m_generation;
    auto *migrator = new HlsPreloadMigrator(
        m_http, segments.mid(0, resume.segmentIndex), resume.segmentIndex,
        resume.bytesWritten, trackPath, QByteArrayLiteral("video/mp2t"), m_item.headers,
        m_originUrl,
        [this, video, generation](bool, const QList<HlsPreloadEntry> &entries,
                                  const QString &error) {
            onMigrationFinished(video, generation, entries, error);
        },
        this);
    if (video)
        m_videoMigrator = migrator;
    else
        m_audioMigrator = migrator;
    qCInfo(lcHlsCache) << "开始安全迁移旧 HLS 缓存：" << (video ? "视频轨" : "音轨")
                       << "分片数" << resume.segmentIndex << "字节" << resume.bytesWritten
                       << trackPath;
    migrator->start();
}

/// @brief 迁移结果只在同一次 start/generation 内落索引，防止切换条目后写错日志。
void HlsCacheBuilder::onMigrationFinished(bool video, qint64 generation,
                                          const QList<HlsPreloadEntry> &entries,
                                          const QString &error)
{
    if (video)
        m_videoMigrator = nullptr;
    else
        m_audioMigrator = nullptr;
    if (!m_active || generation != m_generation)
        return;
    if (!error.isEmpty()) {
        qCWarning(lcHlsCache) << "旧缓存安全迁移失败：" << (video ? "视频轨" : "音轨")
                              << error;
        return;
    }
    if (entries.isEmpty())
        return;
    HlsPreloadIndex::instance().recordMany(m_identity, entries);
    qCInfo(lcHlsCache) << "旧缓存安全迁移完成，已建立分片索引："
                       << (video ? "视频轨" : "音轨") << entries.size() << "片";
}

/// @brief 停止旧缓存迁移探测；切换条目/取消/退出时不能留下迟到的应答。
void HlsCacheBuilder::stopMigrations()
{
    if (m_videoMigrator != nullptr) {
        HlsPreloadMigrator *migrator = m_videoMigrator.data();
        m_videoMigrator = nullptr;
        migrator->abort();
        migrator->deleteLater();
    }
    if (m_audioMigrator != nullptr) {
        HlsPreloadMigrator *migrator = m_audioMigrator.data();
        m_audioMigrator = nullptr;
        migrator->abort();
        migrator->deleteLater();
    }
}

/// @brief 记录单条轨道的下载进度并触发总体进度汇总。
/// @param total 总字节数；<=0 表示服务器未给出总长，百分比按未知处理。
void HlsCacheBuilder::onTrackProgress(bool video, qint64 received, qint64 total)
{
    if (!m_active)
        return;

    int percent = -1;
    if (total > 0) {
        const qint64 clamped = std::clamp<qint64>(received, 0, total);
        percent = int((clamped * 100) / total);
    }
    if (video) {
        m_videoPct = percent;
        m_videoReceived = qMax<qint64>(0, received);
        m_videoTotal = qMax<qint64>(0, total);
    } else {
        m_audioPct = percent;
        m_audioReceived = qMax<qint64>(0, received);
        m_audioTotal = qMax<qint64>(0, total);
    }
    emitProgress();
}

/// @brief 汇总双轨进度并按节流频率发出 progressChanged。
/// @note 双轨总长都已知时按字节加权；否则退回百分比平均。0、100 和首个
///       进度会强制发出，其余最多每 500ms 一次，避免刷爆状态栏。
void HlsCacheBuilder::emitProgress()
{
    int percent = -1;
    if (m_hasAudio) {
        if (m_videoPct >= 0 || m_audioPct >= 0) {
            // 两条轨都已知总长时按字节加权；音轨通常只有几十 MB，
            // 与几百 MB 的视频轨简单算术平均会让总进度长期停在 50% 附近。
            if (m_videoTotal > 0 && m_audioTotal > 0) {
                const qint64 done = std::clamp<qint64>(m_videoReceived, 0, m_videoTotal)
                                    + std::clamp<qint64>(m_audioReceived, 0, m_audioTotal);
                percent = int((done * 100) / (m_videoTotal + m_audioTotal));
            } else {
                // 任一轨总长未知时退回百分比平均，另一条轨按 0 计。
                const int video = m_videoPct >= 0 ? m_videoPct : 0;
                const int audio = m_audioPct >= 0 ? m_audioPct : 0;
                percent = (video + audio) / 2;
            }
        }
    } else {
        percent = m_videoPct;
    }
    if (percent == m_lastOverallPct)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool force = percent == 0 || percent == 100 || m_lastOverallPct < 0;
    if (!force && now - m_lastProgressMs < 500)
        return;

    const QString detail = m_hasAudio
        ? tr("影视缓存：视频 %1% / 音轨 %2%")
              .arg(m_videoPct >= 0 ? QString::number(m_videoPct) : QStringLiteral("…"),
                   m_audioPct >= 0 ? QString::number(m_audioPct) : QStringLiteral("…"))
        : (percent >= 0 ? tr("影视缓存：%1%").arg(percent) : tr("影视缓存：下载中"));
    m_lastOverallPct = percent;
    m_lastProgressMs = now;
    emit progressChanged(m_item, percent, detail);
}

/// @brief 单条轨道下载完成：更新状态、发出进度并尝试进入转封装。
void HlsCacheBuilder::onTrackFinished(bool video, const QString &path)
{
    if (!m_active)
        return;
    if (video) {
        m_videoTask = nullptr;
        m_videoPath = path;
        m_videoDone = true;
        m_videoPct = 100;
        if (m_videoTotal > 0)
            m_videoReceived = m_videoTotal;
    } else {
        m_audioTask = nullptr;
        m_audioPath = path;
        m_audioDone = true;
        m_audioPct = 100;
        if (m_audioTotal > 0)
            m_audioReceived = m_audioTotal;
    }
    emitProgress();
    maybeRemux();
}

/// @brief 任一轨道失败都会终止整次后台缓存，避免留下半套轨道。
void HlsCacheBuilder::onTrackFailed(bool video, const QString &message)
{
    if (!m_active)
        return;
    finishFailed(video ? tr("视频轨缓存失败：%1").arg(message)
                       : tr("音轨缓存失败：%1").arg(message));
}

/// @brief 两条轨道都完成后启动 FFmpeg 无损转封装。
/// @note 输出先写 .mvpremux 临时文件，成功后由 MediaRemux 改名为正文；
///       进入 Remuxing 后不再接受下载进度更新。
void HlsCacheBuilder::maybeRemux()
{
    if (!m_active || m_stage != Stage::Downloading)
        return;
    if (!m_videoDone || !m_audioDone)
        return;

    if (!QFileInfo::exists(m_videoPath)) {
        finishFailed(tr("视频轨下载完成，但找不到缓存文件"));
        return;
    }
    if (m_hasAudio && !QFileInfo::exists(m_audioPath)) {
        finishFailed(tr("音轨下载完成，但找不到缓存文件"));
        return;
    }

    m_stage = Stage::Remuxing;
    m_variantTimer->stop();
    if (QFileInfo::exists(m_bodyPath))
        QFile::remove(m_bodyPath);

    m_remux = new MediaRemux(this);
    connect(m_remux, &MediaRemux::finished, this, &HlsCacheBuilder::onRemuxFinished);
    qCInfo(lcHlsCache) << "轨道下载完成，开始转封装：" << m_item.displayName()
                       << (m_hasAudio ? "含独立音轨" : "单轨");
    m_remux->remuxToMp4(m_videoPath, m_hasAudio ? m_audioPath : QString(), m_bodyPath);
}

/// @brief 处理转封装结果：写入旁车元数据、清理轨道临时文件并发出完成信号。
/// @param outputPath MediaRemux 产出的无后缀正文路径。
/// @note 取消后迟到的成功结果必须删除，否则会留下无归属 MP4；元数据写入
///       失败也会删除正文，避免出现“文件在但缓存索引不认识”的半成品。
void HlsCacheBuilder::onRemuxFinished(bool success, const QString &outputPath,
                                      const QString &error)
{
    if (m_remux != nullptr) {
        m_remux->deleteLater();
        m_remux = nullptr;
    }
    if (!m_active) {
        // 取消后 MediaRemux 才完成：此时没有任务认领输出，必须删掉，
        // 否则缓存目录会留下一个无主 MP4/半成品，后续扫描也找不到归属。
        if (!outputPath.isEmpty() && QFileInfo::exists(outputPath))
            QFile::remove(outputPath);
        return;
    }

    if (!success) {
        finishFailed(error.isEmpty() ? tr("FFmpeg 无损转封装失败") : error);
        return;
    }

    const qint64 total = QFileInfo(outputPath).size();
    if (total <= 0) {
        QFile::remove(outputPath);
        finishFailed(tr("转封装后的 MP4 文件为空"));
        return;
    }

    if (!MediaCache::writeState(outputPath, MediaCache::cacheIdentity(m_item),
                                preferredPlaybackUrl(m_item), "video/mp4", total,
                                true, QStringLiteral(".mp4"))) {
        QFile::remove(outputPath);
        finishFailed(tr("MP4 已生成，但写入缓存元数据失败"));
        return;
    }

    // 缓存完成不等于当前播放已经改走本地 MP4：本轮播放仍然拿着代理地址，
    // 后面的分片只能靠 `.preload.jsonl` 指向轨道文件读取。主窗口会话保留
    // 现场，等切换条目/退出时清理；独立缓存任务仍按原契约立即清理。
    if (m_retainCompletedTrackFiles)
        retainCurrentTrackFiles();
    else
        cleanupTrackFiles();
    QFile::remove(m_bodyPath + QStringLiteral(".mvpremux"));
    m_active = false;
    m_stage = Stage::Idle;
    m_variantTimer->stop();
    const MediaItem item = m_item;
    const QString cachePath = outputPath;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_videoPath.clear();
    m_audioPath.clear();
    m_master = HlsCacheManifest::Master();
    qCInfo(lcHlsCache) << "HLS 影视缓存完成：" << item.displayName()
                       << cachePath << total << "字节";
    emit progressChanged(item, 100, tr("影视缓存完成"));
    emit completed(item, cachePath);
}

/// @brief 退出程序时中止下载/转码，但保留轨道文件与续传状态。
/// @note 与 cancel() 的区别：退出不是用户删除节目，已下载的分片必须留给
///       下次播放续传，不能调用 cleanupPartial()。
void HlsCacheBuilder::stopForExit()
{
    const bool hadWork = m_active || m_videoTask != nullptr || m_audioTask != nullptr
                         || m_remux != nullptr;
    m_active = false;
    m_stage = Stage::Idle;
    ++m_generation;
    m_variantTimer->stop();
    abortPlaylistFetch();
    stopMigrations();
    stopTasksForExit();

    if (hadWork) {
        qCInfo(lcHlsCache) << "退出中止 HLS 后台缓存，保留已下载分片："
                           << m_item.displayName()
                           << "videoBytes=" << m_videoResume.bytesWritten
                           << "audioBytes=" << m_audioResume.bytesWritten;
    }
    m_item = MediaItem();
    m_bodyPath.clear();
    m_identity.clear();
    m_videoSegments.clear();
    m_audioSegments.clear();
    m_videoPath.clear();
    m_audioPath.clear();
    m_videoResume = TrackResume();
    m_audioResume = TrackResume();
    m_master = HlsCacheManifest::Master();
}

/// @brief 退出专用：DownloadTask 补精确检查点后保留 `.mvpdownload`，转码器直接终止。
void HlsCacheBuilder::stopTasksForExit()
{
    if (m_videoTask != nullptr) {
        DownloadTask *task = m_videoTask.data();
        m_videoTask = nullptr;
        // 不能 disconnect(this)：stopForExit() 还会同步发出最后一次 checkpoint，
        // 续传状态正是靠这个信号写入的。
        task->stopForExit();
    }
    if (m_audioTask != nullptr) {
        DownloadTask *task = m_audioTask.data();
        m_audioTask = nullptr;
        task->stopForExit();
    }
    if (m_remux != nullptr) {
        MediaRemux *remux = m_remux.data();
        m_remux = nullptr;
        remux->disconnect(this);
        remux->cancel();
        remux->deleteLater();
    }
}

/// @brief 轨道续传状态文件：固定放在 `.video.resume.json` / `.audio.resume.json`。
QString HlsCacheBuilder::resumeStatePath(const QString &basePath) const
{
    return basePath + QStringLiteral(".resume.json");
}

/// @brief 读取上次退出的轨道续传点；文件不存在或格式不合法时按不可续处理。
HlsCacheBuilder::TrackResume HlsCacheBuilder::loadTrackResume(const QString &basePath) const
{
    TrackResume resume;
    QFile file(resumeStatePath(basePath));
    if (!file.open(QIODevice::ReadOnly))
        return resume;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcHlsCache) << "HLS 续传状态无法解析，将从头缓存：" << basePath
                              << error.errorString();
        return resume;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt(-1) != 1)
        return resume;

    resume.targetPath = object.value(QStringLiteral("targetPath")).toString();
    // 状态文件也在缓存目录里，必须限制目标与基名同目录，避免被篡改成任意写盘路径。
    if (!resume.targetPath.isEmpty()) {
        const QString baseDir = QDir::cleanPath(QFileInfo(basePath).absolutePath());
        const QString targetDir =
            QDir::cleanPath(QFileInfo(resume.targetPath).absolutePath());
        if (baseDir != targetDir) {
            qCWarning(lcHlsCache) << "HLS 续传目标越界，忽略状态：" << resume.targetPath;
            resume.targetPath.clear();
        }
    }
    resume.bytesWritten = qMax<qint64>(
        0, object.value(QStringLiteral("bytesWritten")).toVariant().toLongLong());
    resume.segmentIndex = qMax(
        0, object.value(QStringLiteral("segmentIndex")).toInt(0));
    resume.segmentCount = object.value(QStringLiteral("segmentCount")).toInt(-1);
    resume.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
    resume.completed = object.value(QStringLiteral("completed")).toBool(false);
    resume.valid = true;
    return resume;
}

/// @brief 原子写入轨道续传点；写失败只记日志，不影响正在进行的缓存。
void HlsCacheBuilder::saveTrackResume(const QString &basePath, const TrackResume &resume)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(QStringLiteral("targetPath"), resume.targetPath);
    object.insert(QStringLiteral("bytesWritten"),
                  QJsonValue(double(resume.bytesWritten)));
    object.insert(QStringLiteral("segmentIndex"), resume.segmentIndex);
    object.insert(QStringLiteral("segmentCount"), resume.segmentCount);
    object.insert(QStringLiteral("fingerprint"), resume.fingerprint);
    object.insert(QStringLiteral("completed"), resume.completed);

    QSaveFile file(resumeStatePath(basePath));
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcHlsCache) << "无法写入 HLS 续传状态：" << file.errorString();
        return;
    }
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(body) != body.size() || !file.commit())
        qCWarning(lcHlsCache) << "HLS 续传状态提交失败：" << file.errorString();
}

/// @brief 删除轨道续传状态；完成落盘或用户删除条目后调用。
void HlsCacheBuilder::removeTrackResume(const QString &basePath)
{
    QFile::remove(resumeStatePath(basePath));
}

/// @brief 中止当前缓存流程，清理未完成的轨道与临时文件。
/// @note 仅用于用户明确删除条目；退出/切换条目走 stopForExit() 保留续传现场。
///       已完成正文不在 m_bodyPath 上，因此 cancel 不会误删完整缓存。
void HlsCacheBuilder::cancel()
{
    m_active = false;
    m_stage = Stage::Idle;
    ++m_generation;
    m_variantTimer->stop();
    abortPlaylistFetch();
    stopMigrations();
    cancelTasks();
    if (!m_bodyPath.isEmpty())
        cleanupPartial(false);
    m_item = MediaItem();
    m_bodyPath.clear();
    m_identity.clear();
    m_videoSegments.clear();
    m_audioSegments.clear();
    m_videoPath.clear();
    m_audioPath.clear();
    m_videoResume = TrackResume();
    m_audioResume = TrackResume();
    m_master = HlsCacheManifest::Master();
}

/// @brief 取消当前缓存并删除该条目的所有缓存（含已完成正文）。
/// @note 用于删除播放列表条目；先复制 MediaItem，再 cancel() 清空成员。
void HlsCacheBuilder::cancelAndRemove()
{
    const MediaItem item = m_item;
    cancel();
    if (item.isValid())
        MediaCache::removeForItem(item);
}

/// @brief 中止当前播放列表请求并释放 reply。
/// @note 先清空 QPointer 再 abort，避免 finished 回调再次进入清理流程。
void HlsCacheBuilder::abortPlaylistFetch()
{
    if (m_playlistReply == nullptr)
        return;
    QNetworkReply *reply = m_playlistReply.data();
    m_playlistReply = nullptr;
    reply->abort();
    reply->deleteLater();
}

/// @brief 取消视频/音轨下载任务与转码器。
/// @note 先断开信号再 cancel，防止取消过程中触发 onTrackFailed 走重复失败
///       流程；MediaRemux 即使 cancel() 后也要 deleteLater()，释放进程对象。
void HlsCacheBuilder::cancelTasks()
{
    if (m_videoTask != nullptr) {
        DownloadTask *task = m_videoTask.data();
        m_videoTask = nullptr;
        task->disconnect(this);
        task->cancel();
    }
    if (m_audioTask != nullptr) {
        DownloadTask *task = m_audioTask.data();
        m_audioTask = nullptr;
        task->disconnect(this);
        task->cancel();
    }
    if (m_remux != nullptr) {
        MediaRemux *remux = m_remux.data();
        m_remux = nullptr;
        remux->disconnect(this);
        remux->cancel();
        // cancel 只终止 ffmpeg 进程；对象本身仍挂在 this 下并可能持有进程/管道，
        // 必须 deleteLater，否则反复取消会累积未释放的转码器。
        remux->deleteLater();
    }
}

/// @brief 记录完成后的轨道/索引现场，供当前播放会话继续命中。
/// @note 只保存路径，不删除任何文件；下一次 start() 切换条目或 stopForExit()
///       时由 cleanupRetainedTracks() 统一回收。
void HlsCacheBuilder::retainCurrentTrackFiles()
{
    m_retainedBasePath = m_bodyPath;
    m_retainedVideoPath = m_videoPath;
    m_retainedAudioPath = m_audioPath;
    if (!m_retainedBasePath.isEmpty()) {
        qCInfo(lcHlsCache) << "缓存完成，保留当前会话分片索引："
                           << m_retainedBasePath;
    }
}

/// @brief 回收上一次完成时保留的轨道文件与分片索引。
void HlsCacheBuilder::cleanupRetainedTracks()
{
    if (m_retainedBasePath.isEmpty())
        return;
    cleanupTrackFilesAt(m_retainedBasePath, m_retainedVideoPath, m_retainedAudioPath);
    qCInfo(lcHlsCache) << "清理延迟保留的缓存轨道：" << m_retainedBasePath;
    m_retainedBasePath.clear();
    m_retainedVideoPath.clear();
    m_retainedAudioPath.clear();
}

/// @brief 删除视频/音轨的中间文件，包括合并产物、转码产物、续传临时文件与续传状态。
void HlsCacheBuilder::cleanupTrackFiles()
{
    cleanupTrackFilesAt(m_bodyPath, m_videoPath, m_audioPath);
}

/// @brief 按指定条目基名清理轨道文件；缓存完成后的延迟清理也走这里。
void HlsCacheBuilder::cleanupTrackFilesAt(const QString &bodyPath, const QString &videoPath,
                                          const QString &audioPath)
{
    const QString videoBase = bodyPath + QStringLiteral(".video");
    const QString audioBase = bodyPath + QStringLiteral(".audio");
    // 先清内存索引再删文件：否则当前进程里仍可能命中指向已删除轨道的切片，
    // 播放器会收到空响应或错位字节。
    if (!videoPath.isEmpty())
        HlsPreloadIndex::instance().clearTrack(videoPath);
    if (!videoBase.isEmpty())
        HlsPreloadIndex::instance().clearTrack(videoBase);
    if (!audioPath.isEmpty())
        HlsPreloadIndex::instance().clearTrack(audioPath);
    if (!audioBase.isEmpty())
        HlsPreloadIndex::instance().clearTrack(audioBase);
    for (const QString &path : { videoPath, videoBase, videoBase + QStringLiteral(".ts"),
                                 videoBase + QStringLiteral(".mp4"),
                                 videoBase + QStringLiteral(".mvpdownload"),
                                 videoBase + QStringLiteral(".ts.mvpdownload"),
                                 videoBase + QStringLiteral(".mp4.mvpdownload") }) {
        if (!path.isEmpty())
            QFile::remove(path);
    }
    for (const QString &path : { audioPath, audioBase, audioBase + QStringLiteral(".ts"),
                                 audioBase + QStringLiteral(".mp4"),
                                 audioBase + QStringLiteral(".mvpdownload"),
                                 audioBase + QStringLiteral(".ts.mvpdownload"),
                                 audioBase + QStringLiteral(".mp4.mvpdownload") }) {
        if (!path.isEmpty())
            QFile::remove(path);
    }
    if (!videoBase.isEmpty())
        removeTrackResume(videoBase);
    // 删除条目时同步删除分片索引日志，避免下次同名身份重新打开时残留旧偏移。
    if (!bodyPath.isEmpty())
        QFile::remove(bodyPath + QStringLiteral(".preload.jsonl"));
    if (!audioBase.isEmpty())
        removeTrackResume(audioBase);
    if (!bodyPath.isEmpty())
        MediaCache::removePartialIdentity(bodyPath);
}

/// @brief 清理当前构建留下的半成品。
/// @param removeBody true 时连同正文和旁车 JSON 一起删除；失败路径使用。
/// @note 只尝试删除空目录，目录里还有用户其他缓存时 rmdir 失败可忽略。
void HlsCacheBuilder::cleanupPartial(bool removeBody)
{
    cleanupTrackFiles();
    if (removeBody && !m_bodyPath.isEmpty()) {
        QFile::remove(m_bodyPath);
        QFile::remove(m_bodyPath + QStringLiteral(".json"));
    }
    if (!m_bodyPath.isEmpty())
        QFile::remove(m_bodyPath + QStringLiteral(".mvpremux"));

    const QString path = m_bodyPath.isEmpty() ? m_videoPath : m_bodyPath;
    if (!path.isEmpty()) {
        const QString folder = QFileInfo(path).absolutePath();
        // 只删空目录；同目录里还有用户其他缓存时 rmdir 会失败，忽略即可。
        QDir().rmdir(folder);
    }
}

/// @brief 统一失败收尾：停止计时器/请求/任务，删除半成品并发出 failed。
/// @param message 面向用户的失败原因。
void HlsCacheBuilder::finishFailed(const QString &message)
{
    if (!m_active)
        return;
    const MediaItem item = m_item;
    m_active = false;
    m_stage = Stage::Idle;
    ++m_generation;
    m_variantTimer->stop();
    abortPlaylistFetch();
    stopMigrations();
    cancelTasks();
    cleanupPartial(true);
    m_item = MediaItem();
    m_bodyPath.clear();
    m_videoPath.clear();
    m_audioPath.clear();
    m_master = HlsCacheManifest::Master();
    qCWarning(lcHlsCache) << "HLS 影视缓存失败：" << item.displayName() << message;
    emit failed(item, message);
}
