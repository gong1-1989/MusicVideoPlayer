#ifndef DOWNLOADCONTROLLER_H
#define DOWNLOADCONTROLLER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QPointer>

class DownloadManager;
class DownloadManagerDialog;
class HttpClient;
class QWidget;

/// 下载入口：按设置里的默认目录直接生成不冲突的文件名，不再弹路径选择。
/// 队列、续传、退出保留由 DownloadManager 负责；本类只做界面装配和提示。
class DownloadController : public QObject
{
    Q_OBJECT

public:
    DownloadController(HttpClient *http, QWidget *dialogParent, QObject *parent = nullptr);

    void download(const MediaItem &item);
    void openManager();

    bool hasActiveDownloads() const;
    int activeDownloadCount() const;
    void prepareForExit();

signals:
    void statusMessage(const QString &message);
    void itemDownloaded(const MediaItem &item, const QString &localPath);

private:
    HttpClient *m_http;
    QWidget *m_dialogParent;
    DownloadManager *m_manager = nullptr;
    QPointer<DownloadManagerDialog> m_dialog;
};

#endif // DOWNLOADCONTROLLER_H
