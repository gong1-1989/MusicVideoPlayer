#ifndef SEARCHCOORDINATOR_H
#define SEARCHCOORDINATOR_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

class SearchProviderBase;
class HttpClient;
class SourceRegistry;

/// 按源的 kind 创建对应实现：内置 / 外部解析器 / 配置驱动
SearchProviderBase *createSearchProvider(const SourceConfig &config, HttpClient *http,
                                         QObject *parent);

/// 并发跑所有启用源，按“音乐/影视分组 + 相关性”合并结果；单个源失败/超时不影响其它源。
class SearchCoordinator : public QObject
{
    Q_OBJECT

public:
    SearchCoordinator(SourceRegistry *registry, HttpClient *http, QObject *parent = nullptr);

    void search(const QString &keyword, int limitPerSource);
    void cancel();
    bool isBusy() const { return m_busy; }

    /// 单源预算：timeoutMs>0 时所有源类型都按配置值，0 时按源类型/条数自动分配。
    static int sourceTimeoutMs(const SourceConfig &config, int limitPerSource);

signals:
    void searchStarted();
    /// 每完成一个源就增量刷新，慢源不会把首屏拖到全部结束才出现
    void searchPartial(const QVector<MediaItem> &items, int filtered, int done, int total);
    void searchSucceeded(const QVector<MediaItem> &items, int filtered,
                         const QStringList &failedSources);
    void searchFinished();

private:
    struct SourceTask
    {
        SearchProviderBase *provider = nullptr;
        QTimer *timer = nullptr;
        QString name;
        int sourceIndex = -1;
        int timeoutMs = 0;
    };

    void startAll();
    void finishSearch();
    void onGlobalTimeout();
    void onProviderFinished(SearchProviderBase *provider, const QVector<MediaItem> &items,
                            int filtered, const QString &error);
    void onSourceTimeout(SearchProviderBase *provider);
    int timeoutForSource(const SourceConfig &config) const;
    int globalBudgetForLimit() const;
    QVector<MediaItem> mergedItems() const;
    int mergedFiltered() const;
    QStringList failedSources() const;
    int taskIndexFor(SearchProviderBase *provider) const;

    SourceRegistry *m_registry;
    HttpClient *m_http;

    QVector<SourceConfig> m_queue;
    QVector<SourceTask> m_tasks;
    QVector<QVector<MediaItem>> m_buckets;
    QVector<int> m_bucketFiltered;
    QVector<QString> m_errors;

    QString m_keyword;
    int m_limit = 20;
    int m_total = 0;
    int m_done = 0;
    bool m_busy = false;
    /// 正在批量启动：同步完成的源不能提前收尾，否则后面的源还没发出去
    bool m_launching = false;
    QElapsedTimer m_elapsed;
    QTimer *m_globalTimer = nullptr;

    static constexpr int kSourceTimeoutMs = 20000;
};

#endif // SEARCHCOORDINATOR_H
