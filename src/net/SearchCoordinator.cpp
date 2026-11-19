#include "net/SearchCoordinator.h"

#include "net/ConfigSearchProvider.h"
#include "net/ExternalSearchProvider.h"
#include "net/MaccmsWebProvider.h"
#include "net/SourceRegistry.h"

#include <QLoggingCategory>

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
    m_sourceTimeout.setSingleShot(true);
    connect(&m_sourceTimeout, &QTimer::timeout, this, &SearchCoordinator::onSourceTimeout);
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
    m_queueNames.clear();
    for (const SourceConfig &config : m_queue)
        m_queueNames.append(config.name);

    if (m_queue.isEmpty()) {
        qCWarning(lcSearch) << "没有启用的搜索源";
        emit searchSucceeded({}, 0, {tr("（没有启用任何搜索源，请到系统设置里添加）")});
        emit searchFinished();
        return;
    }

    m_busy = true;
    m_index = 0;
    m_keyword = keyword;
    m_limit = limitPerSource > 0 ? limitPerSource : 20;
    m_items.clear();
    m_filtered = 0;
    m_failed.clear();

    emit searchStarted();
    startNext();
}

void SearchCoordinator::startNext()
{
    if (m_index >= m_queue.size()) {
        m_sourceTimeout.stop();
        m_busy = false;
        qCInfo(lcSearch) << "搜索完成：" << m_items.size() << "条，过滤" << m_filtered
                         << "条，失败源" << m_failed;
        emit searchSucceeded(m_items, m_filtered, m_failed);
        emit searchFinished();
        return;
    }

    const SourceConfig config = m_queue.at(m_index);

    SearchProviderBase *provider = createSearchProvider(config, m_http, this);
    if (provider == nullptr) {
        // 历史配置里残留的内置源（如已下线的 B站）没有实现类，跳过它并说明原因，
        // 不能让整次搜索因为一个源而中断
        m_failed.append(QStringLiteral("%1（程序已不支持该内置源，已跳过）").arg(config.name));
        ++m_index;
        startNext();
        return;
    }
    m_current = provider;
    connect(provider, &SearchProviderBase::finished, this,
            [this](const QString &sourceId, const QVector<MediaItem> &items, int filtered,
                   const QString &error) {
                Q_UNUSED(sourceId)
                onProviderFinished(items, filtered, error);
            });
    // 网页型影视源每条结果要打详情+播放页；条数调大时给更长的单源预算，
    // 否则 20/30/50 条会在 20 秒处被截断，用户会以为“设置没生效”。
    const bool webVideo = config.kind == QLatin1String("builtin")
                          && config.provider == QLatin1String("maccms_web");
    m_sourceTimeoutMs = webVideo
                            ? qBound(kSourceTimeoutMs, 12000 + m_limit * 400, 45000)
                            : kSourceTimeoutMs;
    // 单源超时：慢源/挂死源不能把整次搜索拖住，超时后跳过继续下一个。
    m_sourceTimeout.start(m_sourceTimeoutMs);
    provider->search(m_keyword, m_limit);
}

void SearchCoordinator::onSourceTimeout()
{
    if (!m_busy || m_current == nullptr)
        return;

    const QString name = m_index < m_queue.size() ? m_queue.at(m_index).name
                                                  : QStringLiteral("unknown");
    m_failed.append(QStringLiteral("%1（单源超过 %2 秒，已跳过）")
                        .arg(name)
                        .arg(m_sourceTimeoutMs / 1000));

    disconnect(m_current, nullptr, this, nullptr);
    m_current->deleteLater();
    m_current = nullptr;

    ++m_index;
    startNext();
}

void SearchCoordinator::onProviderFinished(const QVector<MediaItem> &items, int filtered,
                                           const QString &error)
{
    if (!m_busy)
        return;

    m_sourceTimeout.stop();

    if (!error.isEmpty()) {
        const QString name = m_index < m_queue.size() ? m_queue.at(m_index).name
                                                      : QStringLiteral("unknown");
        m_failed.append(QStringLiteral("%1（%2）").arg(name, error));
    }

    m_items += items;
    m_filtered += filtered;

    if (m_current != nullptr) {
        m_current->deleteLater();
        m_current = nullptr;
    }

    ++m_index;
    emit searchPartial(m_items, m_filtered, m_index, m_queue.size());
    startNext();
}

void SearchCoordinator::cancel()
{
    m_sourceTimeout.stop();
    if (m_current != nullptr) {
        disconnect(m_current, nullptr, this, nullptr);
        m_current->deleteLater();
        m_current = nullptr;
    }
    m_busy = false;
    m_queue.clear();
    m_index = 0;
    emit searchFinished();
}
