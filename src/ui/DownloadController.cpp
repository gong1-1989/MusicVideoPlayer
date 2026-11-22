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

void DownloadController::setMediaItemResolver(std::function<MediaItem(const MediaItem &)> resolver)
{
    m_itemResolver = std::move(resolver);
}

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
    const QString suffix =
        DownloadPaths::suffixForDownload(source, hls, resolved.type == MediaType::Video);
    QString fileName = resolved.title.trimmed();
    if (fileName.isEmpty())
        fileName = tr("未命名");
    fileName = DownloadTask::sanitizeFileName(fileName, suffix);
    const QString target = m_manager->reserveFilePath(directory, fileName);

    // 播放缓存是给播放器用的无后缀副本；下载是用户要长期保存的文件。
    // 下载前先看这条目是否已有完整缓存：有就直接异步复制成带正确后缀的
    // 下载文件，既省一次网络下载，也保留用户可识别的文件名。
    const QString cached = MediaCache::completeFileForItem(resolved);
    if (!cached.isEmpty()) {
        QString queuedPath;
        bool alreadyQueued = false;
        const QString copyId = m_manager->importFromCache(resolved, cached, target,
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

    const qint64 limit = qint64(AppSettings::downloadLimitMb()) * 1024 * 1024;
    QString queuedPath;
    bool alreadyQueued = false;
    m_manager->enqueue(resolved, source, resolved.headers, target, hls, limit,
                       &queuedPath, &alreadyQueued);
    if (alreadyQueued) {
        // enqueue 已经发出“该条目已在下载队列中”；这里不能再拿预分配但
        // 未实际使用的 (1) 文件名去提示，否则用户会去找一个不存在的文件。
        return;
    }
    if (queuedPath.isEmpty())
        queuedPath = target;

    emit statusMessage(hls
                           ? tr("已加入下载：%1（HLS 完成后自动转 MP4）")
                                 .arg(QFileInfo(queuedPath).fileName())
                           : tr("已加入下载：%1").arg(QFileInfo(queuedPath).fileName()));
}

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

bool DownloadController::hasActiveDownloads() const
{
    return activeDownloadCount() > 0;
}

int DownloadController::activeDownloadCount() const
{
    return m_manager != nullptr ? m_manager->activeCount() : 0;
}

void DownloadController::prepareForExit()
{
    if (m_manager != nullptr)
        m_manager->shutdownForExit();
}
