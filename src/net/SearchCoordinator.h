#ifndef SEARCHCOORDINATOR_H
#define SEARCHCOORDINATOR_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QObject>
#include <QStringList>

class ConfigSearchProvider;
class HttpClient;
class SourceRegistry;

/// 依次跑所有启用源，把结果合并成一份；单个源失败不影响其它源。
class SearchCoordinator : public QObject
{
    Q_OBJECT

public:
    SearchCoordinator(SourceRegistry *registry, HttpClient *http, QObject *parent = nullptr);

    void search(const QString &keyword, int limitPerSource);
    void cancel();
    bool isBusy() const { return m_busy; }

signals:
    void searchStarted();
    void searchSucceeded(const QVector<MediaItem> &items, int filtered,
                         const QStringList &failedSources);
    void searchFinished();

private:
    void startNext();
    void onProviderFinished(const QVector<MediaItem> &items, int filtered, const QString &error);

    SourceRegistry *m_registry;
    HttpClient *m_http;

    QVector<SourceConfig> m_queue;
    int m_index = 0;
    bool m_busy = false;

    QString m_keyword;
    int m_limit = 20;
    QVector<MediaItem> m_items;
    int m_filtered = 0;
    QStringList m_failed;
    QVector<QString> m_queueNames;
    ConfigSearchProvider *m_current = nullptr;
};

#endif // SEARCHCOORDINATOR_H
