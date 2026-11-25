#ifndef DOWNLOADCONTROLLER_H
#define DOWNLOADCONTROLLER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

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

    /// 下载前按 sourceId 从源配置补回内存态 Cookie；不改变落盘内容。
    void setMediaItemResolver(std::function<MediaItem(const MediaItem &)> resolver);

    /// 当前正在播放的同一条目如果换过备用线路，下载应跟随实际线路，
    /// 不能重新去请求已经失败的主线路。返回空表示没有可用覆盖地址。
    void setActiveSourceResolver(std::function<QUrl(const MediaItem &)> resolver);

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
    std::function<MediaItem(const MediaItem &)> m_itemResolver;
    std::function<QUrl(const MediaItem &)> m_activeSourceResolver;
    QString m_lastStatus;
};

#endif // DOWNLOADCONTROLLER_H
