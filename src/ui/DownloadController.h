#ifndef DOWNLOADCONTROLLER_H
#define DOWNLOADCONTROLLER_H

#include <QObject>
#include <QPointer>

class HttpClient;
class DownloadTask;
class QWidget;
struct MediaItem;

/// 下载流程：保存对话框、HLS 后缀处理、任务进度与结果提示。
/// 目录记忆仍由主窗持有，控制器只通过信号回报本次选择的目录。
class DownloadController : public QObject
{
    Q_OBJECT

public:
    DownloadController(HttpClient *http, QWidget *dialogParent, QObject *parent = nullptr);

    void download(const MediaItem &item);

signals:
    void statusMessage(const QString &message);

private:
    HttpClient *m_http;
    QWidget *m_dialogParent;
    QPointer<DownloadTask> m_task;
};

#endif // DOWNLOADCONTROLLER_H
