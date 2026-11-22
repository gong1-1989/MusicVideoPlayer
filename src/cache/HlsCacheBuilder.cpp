#include "cache/HlsCacheBuilder.h"

#include "cache/MediaCache.h"
#include "download/DownloadTask.h"
#include "download/MediaRemux.h"
#include "net/HttpClient.h"
#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <limits>

Q_LOGGING_CATEGORY(lcHlsCache, "mvp.hls.cache")

HlsCacheBuilder::HlsCacheBuilder(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
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
    // 没命中完整缓存时可能残留上一次的半成品；先清掉再开始，避免续传点
    // 和新的播放列表错位（同一集的 m3u8 签名/分片顺序可能已变化）。
    MediaCache::removeForItem(item);
    m_bodyPath = MediaCache::filePathForItem(item);
    m_tsPath = m_bodyPath + QStringLiteral(".ts");
    if (!QDir().mkpath(QFileInfo(m_bodyPath).absolutePath()))
        return false;

    m_active = true;
    const QUrl url = preferredPlaybackUrl(item);
    m_task = DownloadTask::start(m_http, url, m_tsPath, item.headers, this);
    if (m_task.isNull()) {
        finishFailed(tr("无法启动 HLS 缓存任务"));
        return false;
    }
    m_task->setMaxBytes(std::numeric_limits<qint64>::max());
    // 自动缓存必须等完整 ENDLIST / VOD；不完整的直播列表按快照缓存会得到
    // 一段看似成功、实际缺结尾的视频，用户下次播放会更困惑。
    m_task->setAllowIncompletePlaylist(false);
    const QUrl origin =
        item.sourceOrigin.isValid() && !item.sourceOrigin.host().isEmpty()
            ? item.sourceOrigin
            : HeaderPolicy::originFromHeaders(item.headers);
    m_task->setOriginUrl(origin);

    connect(m_task, &DownloadTask::targetPathChanged, this,
            [this](const QString &path, bool hls) {
                if (hls && !path.isEmpty())
                    m_tsPath = path;
            });
    connect(m_task, &DownloadTask::succeeded, this,
            &HlsCacheBuilder::onTsReady);
    connect(m_task, &DownloadTask::failed, this,
            [this](const QString &message) { finishFailed(message); });

    qCInfo(lcHlsCache) << "开始后台缓存 HLS：" << item.displayName()
                       << HttpClient::redactUrl(url);
    return true;
}

void HlsCacheBuilder::cancel()
{
    if (m_task != nullptr) {
        DownloadTask *task = m_task.data();
        m_task = nullptr;
        task->disconnect(this);
        task->cancel();
        task->deleteLater();
    }
    if (m_remux != nullptr) {
        MediaRemux *remux = m_remux.data();
        m_remux = nullptr;
        remux->disconnect(this);
        remux->cancel();
        remux->deleteLater();
    }
    if (!m_tsPath.isEmpty() || !m_bodyPath.isEmpty())
        cleanupPartial(false);
    m_active = false;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_tsPath.clear();
}

void HlsCacheBuilder::cancelAndRemove()
{
    const MediaItem item = m_item;
    if (m_task != nullptr) {
        DownloadTask *task = m_task.data();
        m_task = nullptr;
        task->disconnect(this);
        task->cancel();
        task->deleteLater();
    }
    if (m_remux != nullptr) {
        MediaRemux *remux = m_remux.data();
        m_remux = nullptr;
        remux->disconnect(this);
        remux->cancel();
        remux->deleteLater();
    }
    cleanupPartial(false);
    m_active = false;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_tsPath.clear();
    // 先按身份找到实际占用的正文/JSON，再整体删条目目录。不能先删 JSON：
    // 同名条目会解析到 (2)/(3)，JSON 一旦丢了就无法定位真正文。
    if (item.isValid())
        MediaCache::removeForItem(item);
}

void HlsCacheBuilder::onTsReady(const QString &tsPath)
{
    if (!m_active)
        return;
    if (!tsPath.isEmpty())
        m_tsPath = tsPath;
    if (!QFileInfo::exists(m_tsPath)) {
        finishFailed(tr("HLS 分片合并完成，但找不到合并后的 TS 文件"));
        return;
    }

    // DownloadTask 在 succeeded 之后还会自己 finish/deleteLater；这里只摘掉
    // 观察指针，不主动删除，免得在信号调用栈里销毁发送者。
    m_task = nullptr;

    if (QFileInfo::exists(m_bodyPath))
        QFile::remove(m_bodyPath);

    m_remux = new MediaRemux(this);
    connect(m_remux, &MediaRemux::finished, this,
            &HlsCacheBuilder::onRemuxFinished);
    qCInfo(lcHlsCache) << "HLS 分片完成，开始无损转封装：" << m_item.displayName();
    m_remux->remuxToMp4(m_tsPath, m_bodyPath);
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

    QFile::remove(m_tsPath);
    QFile::remove(m_tsPath + QStringLiteral(".mvpdownload"));
    m_active = false;
    const MediaItem item = m_item;
    const QString cachePath = outputPath;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_tsPath.clear();
    qCInfo(lcHlsCache) << "HLS 影视缓存完成：" << item.displayName()
                       << cachePath << total << "字节";
    emit completed(item, cachePath);
}

void HlsCacheBuilder::cleanupPartial(bool removeBody)
{
    if (!m_tsPath.isEmpty()) {
        QFile::remove(m_tsPath);
        QFile::remove(m_tsPath + QStringLiteral(".mvpdownload"));
    }
    if (removeBody && !m_bodyPath.isEmpty()) {
        QFile::remove(m_bodyPath);
        QFile::remove(m_bodyPath + QStringLiteral(".json"));
    }
    if (!m_bodyPath.isEmpty())
        QFile::remove(m_bodyPath + QStringLiteral(".mvpremux"));

    const QString path = m_bodyPath.isEmpty() ? m_tsPath : m_bodyPath;
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
    cleanupPartial(false);
    m_active = false;
    m_item = MediaItem();
    m_bodyPath.clear();
    m_tsPath.clear();
    qCWarning(lcHlsCache) << "HLS 影视缓存失败：" << item.displayName() << message;
    emit failed(item, message);
}
