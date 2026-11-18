#ifndef DOWNLOADTASK_H
#define DOWNLOADTASK_H

#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QString>
#include <QUrl>

class HttpClient;
class QNetworkReply;

/// 单个下载任务：QSaveFile 原子落盘，失败/取消不留半截文件，
/// 进度回调对 total <= 0（无 Content-Length）做了保护。
class DownloadTask : public QObject
{
    Q_OBJECT

public:
    static DownloadTask *start(HttpClient *http,
                               const QUrl &url,
                               const QString &filePath,
                               QObject *parent = nullptr);

    /// 去掉 Windows 非法字符并补扩展名
    static QString sanitizeFileName(const QString &name, const QString &suffix);

    void cancel();

signals:
    void progress(qint64 received, qint64 total);
    void succeeded(const QString &filePath);
    void failed(const QString &message);
    void finished();

private:
    DownloadTask(HttpClient *http,
                 const QUrl &url,
                 const QString &filePath,
                 QObject *parent);

    void startRequest();
    void onReadyRead();
    void onFinished();
    void finish();

    HttpClient *m_http;
    QUrl m_url;
    QString m_filePath;
    QPointer<QNetworkReply> m_reply;
    QPointer<QSaveFile> m_file;
    bool m_done = false;
};

#endif // DOWNLOADTASK_H
