/// @file DownloadController.cpp
/// @brief 下载入口的目录选择、缓存复用与队列桥接实现。
#include "ui/DownloadController.h"

#include "cache/MediaCache.h"
#include "download/DownloadManager.h"
#include "download/DownloadPaths.h"
#include "download/DownloadTask.h"
#include "settings/AppSettings.h"
#include "ui/DownloadManagerDialog.h"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>

#include <utility>

/// @brief 构造下载控制器并创建内部 DownloadManager。
/// @param http 共享 HTTP 客户端。
/// @param dialogParent 下载管理器对话框的父窗口，可为空。
/// @param parent 父对象。
DownloadController::DownloadController(HttpClient *http, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_dialogParent(dialogParent)
    , m_manager(new DownloadManager(http, this))
{
    connect(m_manager, &DownloadManager::statusMessage,
            this, &DownloadController::statusMessage);
    // 下载管理器可能还没打开；先把最新提示缓存下来，打开窗口时补显示。
    connect(this, &DownloadController::statusMessage, this,
            [this](const QString &message) {
                m_lastStatus = message;
                if (m_dialog != nullptr)
                    m_dialog->setStatusMessage(message);
            });
    connect(m_manager, &DownloadManager::jobCompleted,
            this, &DownloadController::itemDownloaded);
}

/// @brief 设置条目补全回调（通常注入 SourceRegistry::mediaItemWithCredentials）。
void DownloadController::setMediaItemResolver(std::function<MediaItem(const MediaItem &)> resolver)
{
    m_itemResolver = std::move(resolver);
}

/// @brief 把在线条目加入下载队列，或从已有播放缓存直接生成下载文件。
/// @param item 用户请求下载的条目。
/// @note 只接受 http/https 直链；下载目录按音乐/影视拆分，文件名走唯一化；
///       队列提示用 enqueue 返回的实际路径，避免提示预分配但未使用的名字。
void DownloadController::download(const MediaItem &item)
{
    const MediaItem resolved = m_itemResolver ? m_itemResolver(item) : item;
    if (!resolved.online) {
        emit statusMessage(tr("本地文件无需下载"));
        return;
    }

    const QUrl source = preferredPlaybackUrl(resolved);
    if (!source.isValid()
        || (source.scheme() != QLatin1String("http")
            && source.scheme() != QLatin1String("https"))) {
        emit statusMessage(tr("该条目没有可下载的直链地址"));
        return;
    }

    // 下载根目录由设置决定，其下再按媒体类型分“音乐”“影视”子目录；
    // 队列里的目标路径仍由唯一文件名生成，避免同名覆盖。
    const QString root = AppSettings::downloadDir();
    const QString directory =
        DownloadPaths::mediaDirectory(root, resolved.type == MediaType::Video);
    if (!QDir().mkpath(directory)) {
        const QString message = tr("无法创建下载目录：%1").arg(directory);
        emit statusMessage(message);
        if (m_dialogParent != nullptr)
            QMessageBox::warning(m_dialogParent, tr("无法下载"), message);
        return;
    }

    const bool hls = DownloadTask::looksLikeHls(source);
    const bool hlsVideo = hls && resolved.type == MediaType::Video;
    const QString suffix = hlsVideo
        ? QStringLiteral(".mp4")
        : DownloadPaths::suffixForDownload(source, hls,
                                           resolved.type == MediaType::Video);
    QString fileName = resolved.title.trimmed();
    if (fileName.isEmpty())
        fileName = tr("未命名");
    fileName = DownloadTask::sanitizeFileName(fileName, suffix);
    const QString target = m_manager->reserveFilePath(directory, fileName);
    if (target.isEmpty()) {
        const QString message = tr("无法为下载分配唯一文件名：%1").arg(directory);
        emit statusMessage(message);
        if (m_dialogParent != nullptr)
            QMessageBox::warning(m_dialogParent, tr("无法下载"), message);
        return;
    }
    const qint64 limit = qint64(AppSettings::downloadLimitMb()) * 1024 * 1024;

    // 播放缓存是给播放器用的无后缀副本；下载是用户要长期保存的文件。
    // 下载前先看这条目是否已有完整缓存：有就直接异步复制成带正确后缀的
    // 下载文件，既省一次网络下载，也保留用户可识别的文件名。
    const QString cached = MediaCache::completeFileForItem(resolved);
    if (!cached.isEmpty()) {
        QString queuedPath;
        bool alreadyQueued = false;
        const QString copyId = m_manager->importFromCache(resolved, cached, target, limit,
                                                         &queuedPath, &alreadyQueued);
        if (alreadyQueued)
            return;
        if (!copyId.isEmpty()) {
            if (queuedPath.isEmpty())
                queuedPath = target;
            emit statusMessage(tr("已从播放缓存生成下载：%1")
                                   .arg(QFileInfo(queuedPath).fileName()));
            return;
        }
        // 缓存读取失败时退回普通网络下载，不让下载入口因此失效。
    }

    QString queuedPath;
    bool alreadyQueued = false;
    DownloadManager::CacheSeed seed;
    bool hasSeed = false;
    if (hlsVideo) {
        // 半成品播放缓存按“已完成分片前缀”复用：先复制到下载暂存区，
        // 由 DownloadTask 重新解析播放列表并用指纹校验；不是同一条时间线
        // 时任务会自动丢弃这段前缀，不会把两段不同分片拼在一起。
        MediaCache::HlsTrackSeed cacheSeed;
        if (MediaCache::hlsTrackSeedForItem(resolved, &cacheSeed)) {
            seed.sourcePath = cacheSeed.dataPath;
            seed.bytes = cacheSeed.bytes;
            seed.segmentIndex = cacheSeed.segmentIndex;
            seed.fingerprint = cacheSeed.fingerprint;
            seed.cacheBodyPath = cacheSeed.bodyPath;
            hasSeed = true;
        }
    }

    m_manager->enqueue(resolved, source, resolved.headers, target, hls, limit,
                       &queuedPath, &alreadyQueued, hasSeed ? &seed : nullptr);
    if (alreadyQueued) {
        // enqueue 已经发出“该条目已在下载队列中”；这里不能再拿预分配但
        // 未实际使用的 (1) 文件名去提示，否则用户会去找一个不存在的文件。
        return;
    }
    if (queuedPath.isEmpty())
        queuedPath = target;

    if (hasSeed) {
        emit statusMessage(tr("已加入下载：%1（先复用播放缓存已完成分片，再补齐剩余部分）")
                               .arg(QFileInfo(queuedPath).fileName()));
    } else if (hls) {
        emit statusMessage(tr("已加入下载：%1（下载暂存完成后自动整合为 MP4）")
                               .arg(QFileInfo(queuedPath).fileName()));
    } else {
        emit statusMessage(tr("已加入下载：%1").arg(QFileInfo(queuedPath).fileName()));
    }
}

/// @brief 打开下载管理器窗口；已打开时置顶并激活。
void DownloadController::openManager()
{
    if (m_dialog != nullptr) {
        m_dialog->show();
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }

    auto *dialog = new DownloadManagerDialog(m_manager, m_dialogParent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_dialog = dialog;
    if (!m_lastStatus.isEmpty())
        dialog->setStatusMessage(m_lastStatus);
    dialog->show();
}

/// @brief 是否还有进行中的下载任务（退出前提示用）。
bool DownloadController::hasActiveDownloads() const
{
    return activeDownloadCount() > 0;
}

/// @brief 返回进行中的下载任务数。
int DownloadController::activeDownloadCount() const
{
    return m_manager != nullptr ? m_manager->activeCount() : 0;
}

/// @brief 退出前让下载管理器保存快照并停止调度。
void DownloadController::prepareForExit()
{
    if (m_manager != nullptr)
        m_manager->shutdownForExit();
}
