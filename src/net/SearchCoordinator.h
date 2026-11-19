#ifndef SEARCHCOORDINATOR_H
#define SEARCHCOORDINATOR_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QObject>
#include <QStringList>
#include <QTimer>

class SearchProviderBase;
class HttpClient;
class SourceRegistry;

/// 按源的 kind 创建对应实现：内置 / 外部解析器 / 配置驱动
SearchProviderBase *createSearchProvider(const SourceConfig &config, HttpClient *http,
                                         QObject *parent);

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
    /// 每完成一个源就增量刷新，慢源不会把首屏拖到全部结束才出现
    void searchPartial(const QVector<MediaItem> &items, int filtered, int done, int total);
    void searchSucceeded(const QVector<MediaItem> &items, int filtered,
                         const QStringList &failedSources);
    void searchFinished();

private:
    void startNext();
    void onProviderFinished(const QVector<MediaItem> &items, int filtered, const QString &error);
    void onSourceTimeout();

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
    SearchProviderBase *m_current = nullptr;
    QTimer m_sourceTimeout;
    int m_sourceTimeoutMs = 20000;
    static constexpr int kSourceTimeoutMs = 20000;
};

#endif // SEARCHCOORDINATOR_H
