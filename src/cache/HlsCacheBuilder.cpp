/// @file HlsCacheBuilder.cpp
/// @brief 播放中 HLS 影视后台整段缓存的编排实现。
/// @details 状态机顺序为：主列表 → 等待播放器选码率 → 视频/音频媒体列表 →
///          并行下载轨道 → FFmpeg 转封装 → 写入缓存元数据。每次 start/cancel/
///          失败都会递增 generation，用于丢弃迟到的网络/下载回调。
#include "cache/HlsCacheBuilder.h"

#include "cache/MediaCache.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QNetworkReply>
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

/// @brief 析构时中止所有网络请求、下载与转码，避免子任务回调访问已销毁对象。
HlsCacheBuilder::~HlsCacheBuilder()
{
    cancel();
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

/// @brief 开始为指定在线 HLS 影视条目构建完整缓存。
/// @param item 当前播放条目，必须是在线视频且是 HLS。
/// @return 已启动返回 true；非 HLS、已有完整缓存或清理旧缓存失败返回 false。
/// @note 已有完整缓存时直接跳过，避免重复占用磁盘和带宽。
bool HlsCacheBuilder::start(const MediaItem &item)
{
    cancel();

    // HLS 影视才需要整段缓存；直链 MP4 已有 StreamProxy 的完整文件落盘。
    if (m_http == nullptr || !item.online || item.type != MediaType::Video
        || !playbackIsHls(item) || !item.isValid()) {
        return false;
    }

    if (!MediaCache::completeFileForItem(item).isEmpty()) {
        qCDebug(lcHlsCache) << "HLS 完整缓存已存在，跳过：" << item.displayName();
        return false;
    }

    m_item = item;
    m_sourceUrl = preferredPlaybackUrl(item);
    m_originUrl = item.sourceOrigin.isValid() && !item.sourceOrigin.host().isEmpty()
                      ? item.sourceOrigin
                      : HeaderPolicy::originFromHeaders(item.headers);
    // 没命中完整缓存时可能残留上一次的半成品；先清掉再开始。
    // 删除失败说明有文件仍被占用（例如播放器还在读），此时继续构建会
    // 覆盖/争用同一路径；本次放弃，下一轮再试。
    if (!MediaCache::removeForItem(item)) {
        qCWarning(lcHlsCache) << "无法清理旧的 HLS 缓存文件，跳过本次后台缓存："
                              << item.displayName();
        return false;
    }
    m_bodyPath = MediaCache::filePathForItem(item);
    m_videoPath = m_bodyPath + QStringLiteral(".video");
    m_audioPath = m_bodyPath + QStringLiteral(".audio");
    if (!QDir().mkpath(QFileInfo(m_bodyPath).absolutePath()))
        return false;

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

    qCInfo(lcHlsCache) << "音轨：" << trackSourceLabel(m_audioDirect)
                       << "分片数" << track.segmentCount
                       << HttpClient::redactUrl(m_audioSource);
    startDownloads();
}

/// @brief 清理旧轨道临时文件并启动下载。
/// @note 视频轨和独立音轨并行下载；没有音轨时直接把 audioDone 置为 true，
///       让 maybeRemux() 只等待视频轨。
void HlsCacheBuilder::startDownloads()
{
    if (!m_active)
        return;

    m_videoPath = m_bodyPath + QStringLiteral(".video");
    m_audioPath = m_bodyPath + QStringLiteral(".audio");
    QFile::remove(m_videoPath);
    QFile::remove(m_videoPath + QStringLiteral(".ts"));
    QFile::remove(m_videoPath + QStringLiteral(".mp4"));
    QFile::remove(m_videoPath + QStringLiteral(".mvpdownload"));
    QFile::remove(m_videoPath + QStringLiteral(".mp4.mvpdownload"));
    QFile::remove(m_audioPath);
    QFile::remove(m_audioPath + QStringLiteral(".ts"));
    QFile::remove(m_audioPath + QStringLiteral(".mp4"));
    QFile::remove(m_audioPath + QStringLiteral(".mvpdownload"));
    QFile::remove(m_audioPath + QStringLiteral(".mp4.mvpdownload"));

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
    if (!startTrack(true, m_videoSource, m_videoPath))
        return;
    if (m_hasAudio) {
        if (m_audioSource.isEmpty()) {
            finishFailed(tr("没有找到可下载的音轨地址"));
            return;
        }
        if (!startTrack(false, m_audioSource, m_audioPath))
            return;
    } else {
        m_audioDone = true;
    }
}

/// @brief 为单条轨道创建 DownloadTask 并连接进度/完成/失败信号。
/// @param video true 表示视频轨，false 表示独立音轨。
/// @param source 轨道源地址（媒体列表或单文件 BYTERANGE 地址）。
/// @param targetPath 下载目标路径。
/// @return 任务启动成功返回 true；失败时已走 finishFailed()。
bool HlsCacheBuilder::startTrack(bool video, const QUrl &source, const QString &targetPath)
{
    DownloadTask *task = DownloadTask::start(m_http, source, targetPath, m_item.headers, this);
    if (task == nullptr) {
        finishFailed(video ? tr("无法启动视频缓存任务") : tr("无法启动音轨缓存任务"));
        return false;
    }
    if (video) {
        m_videoTask = task;
        m_videoPath = targetPath;
    } else {
        m_audioTask = task;
        m_audioPath = targetPath;
    }

    task->setMaxBytes(std::numeric_limits<qint64>::max());
    task->setAllowIncompletePlaylist(false);
    task->setOriginUrl(m_originUrl);
    for (const QString &line : std::as_const(m_master.sessionKeyLines))
        task->setHlsSessionKey(line, m_masterBaseUrl);
    connect(task, &DownloadTask::targetPathChanged, this,
            [this, video](const QString &path, bool) {
                if (!m_active)
                    return;
                if (video)
                    m_videoPath = path;
                else
                    m_audioPath = path;
            });
    connect(task, &DownloadTask::progress, this,
            [this, video](qint64 received, qint64 total) {
                onTrackProgress(video, received, total);
            });
    connect(task, &DownloadTask::succeeded, this,
            [this, video](const QString &path) { onTrackFinished(video, path); });
    connect(task, &DownloadTask::failed, this,
            [this, video](const QString &message) { onTrackFailed(video, message); });
    return true;
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

/// @brief 中止当前缓存流程，清理未完成的轨道与临时文件。
/// @note 已完成正文不在 m_bodyPath 上，因此 cancel 不会误删完整缓存；
///       递增 generation 可让所有在途回调失效。
void HlsCacheBuilder::cancel()
{
    m_active = false;
    m_stage = Stage::Idle;
    ++m_generation;
    m_variantTimer->stop();
    abortPlaylistFetch();
    cancelTasks();
    if (!m_bodyPath.isEmpty())
        cleanupPartial(false);
    m_item = MediaItem();
    m_bodyPath.clear();
    m_videoPath.clear();
    m_audioPath.clear();
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

/// @brief 删除视频/音轨的中间文件，包括合并产物、转码产物和续传临时文件。
void HlsCacheBuilder::cleanupTrackFiles()
{
    const QString videoBase = m_bodyPath + QStringLiteral(".video");
    const QString audioBase = m_bodyPath + QStringLiteral(".audio");
    for (const QString &path : { m_videoPath, videoBase, videoBase + QStringLiteral(".ts"),
                                 videoBase + QStringLiteral(".mp4"),
                                 videoBase + QStringLiteral(".mvpdownload"),
                                 videoBase + QStringLiteral(".mp4.mvpdownload") }) {
        if (!path.isEmpty())
            QFile::remove(path);
    }
    for (const QString &path : { m_audioPath, audioBase, audioBase + QStringLiteral(".ts"),
                                 audioBase + QStringLiteral(".mp4"),
                                 audioBase + QStringLiteral(".mvpdownload"),
                                 audioBase + QStringLiteral(".mp4.mvpdownload") }) {
        if (!path.isEmpty())
            QFile::remove(path);
    }
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
