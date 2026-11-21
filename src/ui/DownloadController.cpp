#include "ui/DownloadController.h"

#include "download/DownloadManager.h"
#include "download/DownloadPaths.h"
#include "download/DownloadTask.h"
#include "settings/AppSettings.h"
#include "ui/DownloadManagerDialog.h"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QUrl>

DownloadController::DownloadController(HttpClient *http, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_dialogParent(dialogParent)
    , m_manager(new DownloadManager(http, this))
{
    connect(m_manager, &DownloadManager::statusMessage,
            this, &DownloadController::statusMessage);
    connect(m_manager, &DownloadManager::jobCompleted,
            this, &DownloadController::itemDownloaded);
}

void DownloadController::download(const MediaItem &item)
{
    if (!item.online) {
        emit statusMessage(tr("本地文件无需下载"));
        return;
    }

    const QUrl source = preferredPlaybackUrl(item);
    if (!source.isValid()
        || (source.scheme() != QLatin1String("http")
            && source.scheme() != QLatin1String("https"))) {
        emit statusMessage(tr("该条目没有可下载的直链地址"));
        return;
    }

    const QString directory = AppSettings::downloadDir();
    if (!QDir().mkpath(directory)) {
        const QString message = tr("无法创建下载目录：%1").arg(directory);
        emit statusMessage(message);
        if (m_dialogParent != nullptr)
            QMessageBox::warning(m_dialogParent, tr("无法下载"), message);
        return;
    }

    const bool hls = DownloadTask::looksLikeHls(source);
    const QString suffix =
        DownloadPaths::suffixForDownload(source, hls, item.type == MediaType::Video);
    QString fileName = item.title.trimmed();
    if (fileName.isEmpty())
        fileName = tr("未命名");
    fileName = DownloadTask::sanitizeFileName(fileName, suffix);
    const QString target = DownloadPaths::uniqueFilePath(directory, fileName);

    const qint64 limit = qint64(AppSettings::downloadLimitMb()) * 1024 * 1024;
    m_manager->enqueue(item, source, item.headers, target, hls, limit);

    emit statusMessage(hls
                           ? tr("已加入下载：%1（HLS 完成后自动转 MP4）").arg(QFileInfo(target).fileName())
                           : tr("已加入下载：%1").arg(QFileInfo(target).fileName()));
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
