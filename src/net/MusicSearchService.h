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

    /// 单次返回条数，来自系统设置
    void setResultLimit(int limit);
    int resultLimit() const { return m_resultLimit; }

signals:
    void searchStarted();
    void searchSucceeded(const QVector<MediaItem> &items);
    void searchFailed(const QString &message);
    void searchFinished();

private:
    void onFinished();

    HttpClient *m_http;
    QPointer<QNetworkReply> m_reply;
    int m_resultLimit = 20;
};

#endif // MUSICSEARCHSERVICE_H
