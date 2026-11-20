#ifndef CONFIGSEARCHPROVIDER_H
#define CONFIGSEARCHPROVIDER_H

#include "media/MediaItem.h"
#include "net/SearchProviderBase.h"
#include "net/SourceConfig.h"

#include <QObject>
#include <QPointer>

class HttpClient;
class QNetworkReply;

/// 一个由配置描述驱动的搜索源：发请求 → 取 JSON → 按路径取值 → 建 MediaItem。
/// 所有平台差异都写在配置里，这个类本身不含任何平台知识。
class ConfigSearchProvider : public SearchProviderBase
{
    Q_OBJECT

public:
    ConfigSearchProvider(const SourceConfig &config, HttpClient *http, QObject *parent = nullptr);

    const SourceConfig &config() const { return m_config; }

    void search(const QString &keyword, int limit) override;

private:
    void onReplyFinished();

    // 二次解析：搜索命中后取详情换直链，最多 3 路并发；结果按原索引缓存保证顺序
    void launchDetails();
    void onDetailFinished(QNetworkReply *reply, int index);
    void finishSearch();
    QVector<MediaItem> buildItems(const MediaItem &base, const QString &stream);

    SourceConfig m_config;
    HttpClient *m_http;
    QPointer<QNetworkReply> m_reply;
    QString m_keyword;
    int m_limit = 20;
    bool m_done = false;
    QUrl m_baseUrl;

    QVector<MediaItem> m_result;
    int m_filtered = 0;

    QVector<MediaItem> m_pendingBases;
    QVector<QString> m_pendingIds;
    QVector<QString> m_pendingTitles;
    QVector<QString> m_pendingArtists;
    int m_nextDetail = 0;
    int m_activeDetails = 0;
    QVector<QVector<MediaItem>> m_detailBuckets;
};

#endif // CONFIGSEARCHPROVIDER_H
