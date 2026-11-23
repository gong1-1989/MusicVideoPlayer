#include "net/SearchCoordinator.h"

#include "net/ConfigSearchProvider.h"
#include "net/ExternalSearchProvider.h"
#include "net/MaccmsWebProvider.h"
#include "net/SourceRegistry.h"

#include <QLoggingCategory>
#include <utility>

Q_LOGGING_CATEGORY(lcSearch, "mvp.search")

SearchProviderBase *createSearchProvider(const SourceConfig &config, HttpClient *http,
                                         QObject *parent)
{
    if (config.kind == QLatin1String("builtin")) {
        // 内置实现在配置里用 provider 区分：网页型影视（内置抓取）/ 网易云音乐 / iTunes
        if (config.provider == QLatin1String("maccms_web"))
            return new MaccmsWebProvider(config, http, parent);
        // 内置源如果是程序不认识的 provider（例如历史配置里残留的 bilibili），
        // 返回空指针由调用方跳过，不让它把整次搜索拖崩
        return nullptr;
    }
    if (config.kind == QLatin1String("external"))
        return new ExternalSearchProvider(config, parent);
    return new ConfigSearchProvider(config, http, parent);
}

SearchCoordinator::SearchCoordinator(SourceRegistry *registry, HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_http(http)
{
    m_globalTimer = new QTimer(this);
    m_globalTimer->setSingleShot(true);
    connect(m_globalTimer, &QTimer::timeout, this, &SearchCoordinator::onGlobalTimeout);
}

void SearchCoordinator::search(const QString &keyword, int limitPerSource)
{
    if (m_busy || keyword.trimmed().isEmpty())
        return;
    if (m_registry == nullptr) {
        emit searchSucceeded({}, 0, QStringList());
        emit searchFinished();
        return;
    }

    m_queue = m_registry->enabledSources();

    if (m_queue.isEmpty()) {
        qCWarning(lcSearch) << "没有启用的搜索源";
        emit searchSucceeded({}, 0, {tr("（没有启用任何搜索源，请到系统设置里添加）")});
        emit searchFinished();
        return;
    }

    m_busy = true;
    m_keyword = keyword;
    m_limit = limitPerSource > 0 ? limitPerSource : 20;
    m_total = m_queue.size();
    m_done = 0;
    m_tasks.clear();
    m_buckets.clear();
    m_buckets.resize(m_total);
    m_bucketFiltered.fill(0, m_total);
    m_errors.fill(QString(), m_total);

    emit searchStarted();
    m_elapsed.start();
    const int budget = globalBudgetForLimit();
    qCInfo(lcSearch) << "并发搜索开始：" << m_total << "个源，关键词" << keyword
                     << "全局预算" << budget << "ms";
    m_globalTimer->start(budget);
    startAll();
}

void SearchCoordinator::startAll()
{
    // 先把所有源都实例化出来，再统一下发搜索；任何一个源同步报错/同步完成
    // 都不会让后面的源还没启动就被误判成“全部结束”。
    for (int i = 0; i < m_queue.size(); ++i) {
        const SourceConfig &config = m_queue.at(i);

        SearchProviderBase *provider = createSearchProvider(config, m_http, this);
        if (provider == nullptr) {
            // 历史配置里残留的内置源（如已下线的 B站）没有实现类，跳过它并说明原因
            m_errors[i] = QStringLiteral("%1（程序已不支持该内置源，已跳过）").arg(config.name);
            ++m_done;
            qCWarning(lcSearch) << "跳过不支持的内置源：" << config.name;
            continue;
        }

        QTimer *timer = new QTimer(this);
        timer->setSingleShot(true);

        SourceTask task;
        task.provider = provider;
        task.timer = timer;
        task.name = config.name;
        task.sourceIndex = i;
        task.timeoutMs = timeoutForSource(config);
        m_tasks.append(task);

        connect(provider, &SearchProviderBase::finished, this,
                [this, provider](const QString &sourceId, const QVector<MediaItem> &items,
                                 int filtered, const QString &error) {
                    Q_UNUSED(sourceId)
                    onProviderFinished(provider, items, filtered, error);
                });
        connect(timer, &QTimer::timeout, this, [this, provider] { onSourceTimeout(provider); });
    }

    if (m_tasks.isEmpty()) {
        finishSearch();
        return;
    }

    // 每个源各自计时，总耗时取决于最慢的那个源，而不是所有源预算相加。
    // 网页影视源按条数动态放宽，避免 20/30/50 条被统一的短预算截断。
    const QVector<SourceTask> tasks = m_tasks;
    m_launching = true;
    for (const SourceTask &task : tasks) {
        if (task.provider == nullptr || task.timer == nullptr)
            continue;
        qCInfo(lcSearch) << "启动源：" << task.name << "预算" << task.timeoutMs << "ms";
        task.timer->start(task.timeoutMs);
        task.provider->search(m_keyword, m_limit);
    }
    m_launching = false;

    if (m_done >= m_total)
        finishSearch();
}

int SearchCoordinator::timeoutForSource(const SourceConfig &config) const
{
    return sourceTimeoutMs(config, m_limit);
}

int SearchCoordinator::sourceTimeoutMs(const SourceConfig &config, int limitPerSource)
{
    // 配置了超时的源必须真正按配置限时，否则设置界面上的值只是装饰。
    if (config.timeoutMs > 0)
        return qBound(1000, config.timeoutMs, 60000);

    // 网页型影视源每条结果要打详情+播放页；条数调大时给更长的单源预算，
    // 否则 20/30/50 条会在 20 秒处被截断，用户会以为“设置没生效”。
    const bool webVideo = config.kind == QLatin1String("builtin")
                          && config.provider == QLatin1String("maccms_web");
    const int limit = limitPerSource > 0 ? limitPerSource : 20;
    return webVideo ? qBound(kSourceTimeoutMs, 12000 + limit * 400, 45000)
                    : kSourceTimeoutMs;
}

int SearchCoordinator::globalBudgetForLimit() const
{
    // 总预算至少覆盖最慢的单源配置，否则单源还没到自己的超时就被全局计时截断。
    int budget = qBound(12000, 8000 + m_limit * 500, 45000);
    for (const SourceConfig &config : std::as_const(m_queue))
        budget = qMax(budget, sourceTimeoutMs(config, m_limit));
    return budget;
}

int SearchCoordinator::taskIndexFor(SearchProviderBase *provider) const
{
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).provider == provider)
            return i;
    }
    return -1;
}

void SearchCoordinator::onSourceTimeout(SearchProviderBase *provider)
{
    const int index = taskIndexFor(provider);
    if (index < 0)
        return; // 已经完成或被取消，迟到的超时不再处理

    const SourceTask task = m_tasks.at(index);
    m_tasks.removeAt(index);
    if (task.timer != nullptr) {
        task.timer->stop();
        task.timer->deleteLater();
    }

    disconnect(provider, nullptr, this, nullptr);
    provider->abort();
    provider->deleteLater();

    if (task.sourceIndex >= 0 && task.sourceIndex < m_errors.size()) {
        m_errors[task.sourceIndex] =
            QStringLiteral("%1（单源超过 %2 秒，已跳过）")
                .arg(task.name)
                .arg(task.timeoutMs / 1000);
    }
    ++m_done;

    qCWarning(lcSearch) << "源超时：" << task.name << "预算" << task.timeoutMs << "ms，累计"
                        << m_done << "/" << m_total;
    emit searchPartial(mergedItems(), mergedFiltered(), m_done, m_total);

    if (!m_launching && m_done >= m_total)
        finishSearch();
}

void SearchCoordinator::onGlobalTimeout()
{
    if (!m_busy)
        return;

    const QVector<SourceTask> pending = m_tasks;
    m_tasks.clear();
    for (const SourceTask &task : pending) {
        if (task.sourceIndex >= 0 && task.sourceIndex < m_errors.size()
            && m_errors.at(task.sourceIndex).isEmpty()) {
            m_errors[task.sourceIndex] =
                QStringLiteral("%1（整体超过 %2 秒，已跳过）")
                    .arg(task.name)
                    .arg(globalBudgetForLimit() / 1000);
        }
        if (task.timer != nullptr) {
            task.timer->stop();
            task.timer->deleteLater();
        }
        if (task.provider != nullptr) {
            disconnect(task.provider, nullptr, this, nullptr);
            task.provider->abort();
            task.provider->deleteLater();
        }
    }
    m_done = m_total;
    qCWarning(lcSearch) << "全局预算用尽，跳过未完成源" << pending.size() << "个";
    emit searchPartial(mergedItems(), mergedFiltered(), m_done, m_total);
    finishSearch();
}

void SearchCoordinator::onProviderFinished(SearchProviderBase *provider,
                                           const QVector<MediaItem> &items, int filtered,
                                           const QString &error)
{
    const int index = taskIndexFor(provider);
    if (index < 0)
        return; // 超时后已摘掉，或者取消后迟到的 finished

    const SourceTask task = m_tasks.at(index);
    m_tasks.removeAt(index);
    if (task.timer != nullptr) {
        task.timer->stop();
        task.timer->deleteLater();
    }
    provider->deleteLater();

    if (task.sourceIndex >= 0 && task.sourceIndex < m_buckets.size()) {
        m_buckets[task.sourceIndex] = items;
        m_bucketFiltered[task.sourceIndex] = filtered;
        m_errors[task.sourceIndex] = error;
    }
    ++m_done;

    qCInfo(lcSearch) << "源完成：" << task.name << items.size() << "条，过滤" << filtered
                     << "条，累计" << m_done << "/" << m_total << "用时"
                     << m_elapsed.elapsed() << "ms";
    emit searchPartial(mergedItems(), mergedFiltered(), m_done, m_total);

    if (!m_launching && m_done >= m_total)
        finishSearch();
}

QVector<MediaItem> SearchCoordinator::mergedItems() const
{
    // 按源配置顺序合并，而不是按网络返回先后；同一关键词多次搜索的列表顺序保持稳定
    QVector<MediaItem> all;
    for (const QVector<MediaItem> &bucket : m_buckets)
        all += bucket;
    // 各源字段差异很大：影视站有 vod_remarks、音乐源多数没有独立品质字段。
    // 统一在合并出口补一次推断，搜索结果品质栏不会长期空着。
    for (MediaItem &item : all) {
        if (item.quality.isEmpty()) {
            item.quality = inferQuality(item.title + QLatin1Char(' ') + item.description
                                        + QLatin1Char(' ') + item.url.toString(),
                                        item.type);
        }
        // 音乐源大多不返回码率字段；给一个“标准”兜底，品质栏才不会整列空着。
        if (item.quality.isEmpty() && item.type == MediaType::Music)
            item.quality = QStringLiteral("标准");
    }
    return all;
}

int SearchCoordinator::mergedFiltered() const
{
    int total = 0;
    for (int filtered : m_bucketFiltered)
        total += filtered;
    return total;
}

QStringList SearchCoordinator::failedSources() const
{
    QStringList failed;
    for (const QString &error : m_errors) {
        if (!error.isEmpty())
            failed.append(error);
    }
    return failed;
}

void SearchCoordinator::finishSearch()
{
    if (!m_busy)
        return;

    if (m_globalTimer != nullptr)
        m_globalTimer->stop();
    m_busy = false;
    const QVector<MediaItem> items = mergedItems();
    const int filtered = mergedFiltered();
    const QStringList failed = failedSources();
    qCInfo(lcSearch) << "搜索完成：" << items.size() << "条，过滤" << filtered
                     << "条，失败源" << failed << "，总用时" << m_elapsed.elapsed() << "ms";
    emit searchSucceeded(items, filtered, failed);
    emit searchFinished();
}

void SearchCoordinator::cancel()
{
    // 没有在途请求时不要发空结果：调用方可能只是收尾，空信号会把已经
    // 展示的搜索结果清掉。
    if (!m_busy)
        return;
    if (m_globalTimer != nullptr)
        m_globalTimer->stop();
    const QVector<SourceTask> tasks = m_tasks;
    m_tasks.clear();
    for (const SourceTask &task : tasks) {
        if (task.timer != nullptr) {
            task.timer->stop();
            task.timer->deleteLater();
        }
        if (task.provider != nullptr) {
            disconnect(task.provider, nullptr, this, nullptr);
            task.provider->abort();
            task.provider->deleteLater();
        }
    }

    m_busy = false;
    m_launching = false;
    m_done = 0;
    m_total = 0;
    m_buckets.clear();
    m_bucketFiltered.clear();
    m_errors.clear();
    // 取消后必须发一个空的结果信号，让搜索面板把已经追加的条目清掉；
    // 只发 searchFinished 会让部分结果留在 UI 上，看起来像搜索成功了一半。
    emit searchSucceeded(QVector<MediaItem>(), 0, QStringList());
    emit searchFinished();
}
