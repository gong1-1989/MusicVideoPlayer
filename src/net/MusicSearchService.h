#ifndef MUSICSEARCHSERVICE_H
#define MUSICSEARCHSERVICE_H

#include "media/MediaItem.h"

#include <QObject>
#include <QPointer>
#include <QVector>

class HttpClient;
class QNetworkReply;

/// 在线搜索服务。把"请求-解析-错误处理"从界面里剥离，
/// 界面只消费 searchSucceeded / searchFailed。
class MusicSearchService : public QObject
{
    Q_OBJECT

public:
    explicit MusicSearchService(HttpClient *http, QObject *parent = nullptr);

    void search(const QString &keyword);
    void cancel();
    bool isBusy() const { return !m_reply.isNull(); }

signals:
    void searchStarted();
    void searchSucceeded(const QVector<MediaItem> &items);
    void searchFailed(const QString &message);
    void searchFinished();

private:
    void onFinished();

    HttpClient *m_http;
    QPointer<QNetworkReply> m_reply;
};

#endif // MUSICSEARCHSERVICE_H
