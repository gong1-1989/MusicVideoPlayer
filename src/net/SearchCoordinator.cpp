/// @file SearchCoordinator.cpp
/// @brief 多源并发搜索调度：单源/全局预算、结果归并、失败汇总与取消。
#include "net/SearchCoordinator.h"

#include "net/ConfigSearchProvider.h"
#include "net/ExternalSearchProvider.h"
#include "net/MaccmsWebProvider.h"
#include "net/SourceRegistry.h"

#include <QLoggingCategory>
#include <utility>

Q_LOGGING_CATEGORY(lcSearch, "mvp.search")

/// @brief 按源配置类型创建对应的搜索实现。
/// @param config 源配置，kind 决定实现：builtin / external / 可配置 HTTP。
/// @param http 共享 HTTP 客户端。
/// @param parent 父对象，负责生命周期回收。
/// @return 新建的提供者；遇到程序不支持的内置 provider 时返回 nullptr，由调用方跳过。
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

/// @brief 构造协调器并创建全局预算计时器。
/// @param registry 源注册表，为空时搜索直接返回空结果。
/// @param http 共享 HTTP 客户端。
/// @param parent 父对象。
SearchCoordinator::SearchCoordinator(SourceRegistry *registry, HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_http(http)
{
    m_globalTimer = new QTimer(this);
    m_globalTimer->setSingleShot(true);
    connect(m_globalTimer, &QTimer::timeout, this, &SearchCoordinator::onGlobalTimeout);
}

/// @brief 发起一次多源搜索。
/// @param keyword 搜索关键词；空串或已有搜索在途时直接返回。
/// @param limitPerSource 每个源的条数上限，<=0 时按 20。
/// @note 会清空上一轮分桶并启动全局预算计时器；没有任何启用源时立即以空结果结束。
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

/// @brief 实例化全部启用源并统一下发搜索。
/// @note 先完成全部实例化再启动，避免某个源同步完成/报错时把尚未启动的源
///       误判成“全部结束”；不支持的内置 provider 直接计为失败并跳过。
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

/// @brief 返回指定源的单源预算（毫秒）。
int SearchCoordinator::timeoutForSource(const SourceConfig &config) const
{
    return sourceTimeoutMs(config, m_limit);
}

/// @brief 计算单源超时预算。
/// @param config 源配置。
/// @param limitPerSource 每源条数上限。
/// @return 配置了 timeoutMs 时按其值夹到 1..60 秒；否则网页影视源随条数放宽，
///         其他源使用 kSourceTimeoutMs。
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

/// @brief 计算整次搜索的全局预算，至少覆盖最慢的单源预算。
int SearchCoordinator::globalBudgetForLimit() const
{
    // 总预算至少覆盖最慢的单源配置，否则单源还没到自己的超时就被全局计时截断。
    int budget = qBound(12000, 8000 + m_limit * 500, 45000);
    for (const SourceConfig &config : std::as_const(m_queue))
        budget = qMax(budget, sourceTimeoutMs(config, m_limit));
    return budget;
}

/// @brief 按提供者指针查找在途任务下标。
/// @return 任务下标；已超时/取消/完成时返回 -1。
int SearchCoordinator::taskIndexFor(SearchProviderBase *provider) const
{
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).provider == provider)
            return i;
    }
    return -1;
}

/// @brief 处理单源超时：中止该源、记录失败原因并推进进度。
/// @param provider 超时的提供者。
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

/// @brief 全局预算用尽：中止全部未完成源、补齐失败原因并结束搜索。
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

/// @brief 接收单个源的结果，写入对应分桶并推进进度。
/// @param provider 完成的提供者。
/// @param items 该源返回的条目。
/// @param filtered 被该源过滤掉的条数。
/// @param error 该源失败原因，空表示成功。
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

/// @brief 按源配置顺序合并各源结果，并补齐缺失的品质字段。
/// @return 合并后的条目列表；顺序稳定，不随网络返回先后变化。
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

/// @brief 汇总各源被过滤掉的条数。
int SearchCoordinator::mergedFiltered() const
{
    int total = 0;
    for (int filtered : m_bucketFiltered)
        total += filtered;
    return total;
}

/// @brief 返回所有非空的源失败说明，供界面提示。
QStringList SearchCoordinator::failedSources() const
{
    QStringList failed;
    for (const QString &error : m_errors) {
        if (!error.isEmpty())
            failed.append(error);
    }
    return failed;
}

/// @brief 结束本轮搜索：停全局计时、归并结果并发出 succeeded/finished。
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

/// @brief 取消本轮搜索：中止所有在途源，并发出空结果让界面清掉半成品。
/// @note 没有在途搜索时直接返回，避免误清已经展示的结果。
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
