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

QString trackSourceLabel(bool direct)
{
    return direct ? QStringLiteral("单文件 BYTERANGE → MP4 Range")
                  : QStringLiteral("分片列表 → 合并");
}

} // namespace

HlsCacheBuilder::HlsCacheBuilder(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_variantTimer(new QTimer(this))
{
    m_variantTimer->setInterval(250);
    connect(m_variantTimer, &QTimer::timeout, this, &HlsCacheBuilder::onVariantTick);
}

HlsCacheBuilder::~HlsCacheBuilder()
{
    cancel();
}

void HlsCacheBuilder::setPlaybackTrackResolvers(UrlResolver video, UrlResolver audio)
{
    m_videoResolver = std::move(video);
    m_audioResolver = std::move(audio);
}

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
    MediaCache::removeForItem(item);
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
    m_videoDirect = false;
    m_audioDirect = false;
    m_hasAudio = false;
    m_videoDone = false;
    m_audioDone = false;
    m_videoPct = -1;
    m_audioPct = -1;
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
        m_masterUrl = finalUrl;
        m_stage = Stage::WaitingVariant;
        beginVariantSelection();
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
    m_audioTrack = track;
    m_audioDirect = track.singleFile;
    m_audioSource = track.singleFile ? track.singleFileUrl : finalUrl;

    qCInfo(lcHlsCache) << "音轨：" << trackSourceLabel(m_audioDirect)
                       << "分片数" << track.segmentCount
                       << HttpClient::redactUrl(m_audioSource);
    startDownloads();
}

void HlsCacheBuilder::startDownloads()
{
    if (!m_active)
        return;

    m_videoPath = m_bodyPath + QStringLiteral(".video");
    m_audioPath = m_bodyPath + QStringLiteral(".audio");
    QFile::remove(m_videoPath);
    QFile::remove(m_videoPath + QStringLiteral(".ts"));
    QFile::remove(m_videoPath + QStringLiteral(".mvpdownload"));
    QFile::remove(m_audioPath);
    QFile::remove(m_audioPath + QStringLiteral(".ts"));
    QFile::remove(m_audioPath + QStringLiteral(".mvpdownload"));

    m_stage = Stage::Downloading;
    m_videoDone = false;
    m_audioDone = false;
    m_videoPct = -1;
    m_audioPct = -1;
    m_lastOverallPct = -1;

    if (m_videoSource.isEmpty()) {
        finishFailed(tr("没有找到可下载的视频地址"));
        return;
    }
    startTrack(true, m_videoSource, m_videoPath);
    if (m_hasAudio) {
        if (m_audioSource.isEmpty()) {
            finishFailed(tr("没有找到可下载的音轨地址"));
            return;
        }
        startTrack(false, m_audioSource, m_audioPath);
    } else {
        m_audioDone = true;
    }
}

void HlsCacheBuilder::startTrack(bool video, const QUrl &source, const QString &targetPath)
{
    DownloadTask *task = DownloadTask::start(m_http, source, targetPath, m_item.headers, this);
    if (task == nullptr) {
        finishFailed(video ? tr("无法启动视频缓存任务") : tr("无法启动音轨缓存任务"));
        return;
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
}

void HlsCacheBuilder::onTrackProgress(bool video, qint64 received, qint64 total)
{
    if (!m_active)
        return;

    int percent = -1;
    if (total > 0) {
        const qint64 clamped = std::clamp<qint64>(received, 0, total);
        percent = int((clamped * 100) / total);
    }
    if (video)
        m_videoPct = percent;
    else
        m_audioPct = percent;
    emitProgress();
}

void HlsCacheBuilder::emitProgress()
{
    int percent = -1;
    if (m_hasAudio) {
        if (m_videoPct >= 0 || m_audioPct >= 0) {
            // 另一条轨还没报进度时按 0 计，避免视频先到 100 就显示“完成”。
            const int video = m_videoPct >= 0 ? m_videoPct : 0;
            const int audio = m_audioPct >= 0 ? m_audioPct : 0;
            percent = (video + audio) / 2;
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

void HlsCacheBuilder::onTrackFinished(bool video, const QString &path)
{
    if (!m_active)
        return;
    if (video) {
        m_videoTask = nullptr;
        m_videoPath = path;
        m_videoDone = true;
        m_videoPct = 100;
    } else {
        m_audioTask = nullptr;
        m_audioPath = path;
        m_audioDone = true;
        m_audioPct = 100;
    }
    emitProgress();
    maybeRemux();
}

void HlsCacheBuilder::onTrackFailed(bool video, const QString &message)
{
    if (!m_active)
        return;
    finishFailed(video ? tr("视频轨缓存失败：%1").arg(message)
                       : tr("音轨缓存失败：%1").arg(message));
}

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

void HlsCacheBuilder::onRemuxFinished(bool success, const QString &outputPath,
                                      const QString &error)
{
    if (m_remux != nullptr) {
        m_remux->deleteLater();
        m_remux = nullptr;
    }
    if (!m_active)
        return;

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

void HlsCacheBuilder::cancelAndRemove()
{
    const MediaItem item = m_item;
    cancel();
    if (item.isValid())
        MediaCache::removeForItem(item);
}

void HlsCacheBuilder::abortPlaylistFetch()
{
    if (m_playlistReply == nullptr)
        return;
    QNetworkReply *reply = m_playlistReply.data();
    m_playlistReply = nullptr;
    reply->abort();
    reply->deleteLater();
}

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
    }
}

void HlsCacheBuilder::cleanupTrackFiles()
{
    const QString videoBase = m_bodyPath + QStringLiteral(".video");
    const QString audioBase = m_bodyPath + QStringLiteral(".audio");
    for (const QString &path : { m_videoPath, videoBase, videoBase + QStringLiteral(".ts"),
                                 videoBase + QStringLiteral(".mvpdownload") }) {
        if (!path.isEmpty())
            QFile::remove(path);
    }
    for (const QString &path : { m_audioPath, audioBase, audioBase + QStringLiteral(".ts"),
                                 audioBase + QStringLiteral(".mvpdownload") }) {
        if (!path.isEmpty())
            QFile::remove(path);
    }
}

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
