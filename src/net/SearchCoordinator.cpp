#include "net/SearchCoordinator.h"

#include "net/ConfigSearchProvider.h"
#include "net/SourceRegistry.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSearch, "mvp.search")

SearchCoordinator::SearchCoordinator(SourceRegistry *registry, HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_http(http)
{
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
        qCWarning(lcSearch) << "no enabled sources";
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
        m_busy = false;
        qCInfo(lcSearch) << "finished:" << m_items.size() << "items, filtered" << m_filtered
                         << "failed" << m_failed;
        emit searchSucceeded(m_items, m_filtered, m_failed);
        emit searchFinished();
        return;
    }

    const SourceConfig config = m_queue.at(m_index);
    auto *provider = new ConfigSearchProvider(config, m_http, this);
    m_current = provider;
    connect(provider, &ConfigSearchProvider::finished, this,
            [this](const QString &sourceId, const QVector<MediaItem> &items, int filtered,
                   const QString &error) {
                Q_UNUSED(sourceId)
                onProviderFinished(items, filtered, error);
            });
    provider->search(m_keyword, m_limit);
}

void SearchCoordinator::onProviderFinished(const QVector<MediaItem> &items, int filtered,
                                           const QString &error)
{
    if (!m_busy)
        return;

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
    startNext();
}

void SearchCoordinator::cancel()
{
    if (m_current != nullptr) {
        disconnect(m_current, nullptr, this, nullptr);
        m_current->deleteLater();
        m_current = nullptr;
    }
    m_busy = false;
    m_queue.clear();
    m_index = 0;
}
