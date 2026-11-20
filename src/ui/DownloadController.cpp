#include "ui/DownloadController.h"

#include "download/DownloadTask.h"
#include "media/MediaItem.h"
#include "net/HttpClient.h"
#include "settings/AppSettings.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcDownloadUi, "mvp.download")

DownloadController::DownloadController(HttpClient *http, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_dialogParent(dialogParent)
{
    Q_ASSERT(m_http != nullptr);
    Q_ASSERT(m_dialogParent != nullptr);
}

void DownloadController::download(const MediaItem &item)
{
    if (!item.online) {
        emit statusMessage(tr("本地文件无需下载"));
        return;
    }

    const QUrl downloadUrl = preferredPlaybackUrl(item);
    const bool hls = DownloadTask::looksLikeHls(downloadUrl);
    const QString suffix = hls ? QStringLiteral("ts")
                               : (item.type == MediaType::Video ? QStringLiteral("mp4")
                                                                : QStringLiteral("mp3"));
    const QString filter = hls ? tr("视频文件 (*.ts *.mp4)")
                               : tr("媒体文件 (*.%1)").arg(suffix);
    const QString suggested =
        QDir(AppSettings::lastDir())
            .filePath(DownloadTask::sanitizeFileName(item.displayName(), suffix));
    const QString target =
        QFileDialog::getSaveFileName(m_dialogParent, tr("保存文件"), suggested, filter);
    if (target.isEmpty())
        return;

    AppSettings::setLastDir(QFileInfo(target).absolutePath());

    QString finalTarget = target;
    if (hls) {
        if (!finalTarget.endsWith(QLatin1String(".ts"), Qt::CaseInsensitive)
            && !finalTarget.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
            finalTarget += QLatin1String(".ts");
        }
    } else if (!finalTarget.endsWith(QStringLiteral(".") + suffix, Qt::CaseInsensitive)) {
        finalTarget += QStringLiteral(".") + suffix;
    }

    qCInfo(lcDownloadUi) << "开始下载" << item.displayName() << "到" << finalTarget
                       << (hls ? "（HLS 分片合并）" : "");
    auto *task = DownloadTask::start(m_http, downloadUrl, finalTarget, item.headers, this);
    m_task = task;

    connect(task, &DownloadTask::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0)
            emit statusMessage(tr("下载中 %1%").arg(static_cast<int>(received * 100 / total)));
        else
            emit statusMessage(tr("下载中 %1 KB").arg(received / 1024));
    });
    connect(task, &DownloadTask::succeeded, this, [this](const QString &path) {
        emit statusMessage(tr("下载完成：%1").arg(QDir::toNativeSeparators(path)));
    });
    connect(task, &DownloadTask::failed, this, [this](const QString &message) {
        emit statusMessage(tr("下载失败：%1").arg(message));
    });
    connect(task, &DownloadTask::finished, this, [this, task] {
        if (m_task == task)
            m_task.clear();
    });
}
