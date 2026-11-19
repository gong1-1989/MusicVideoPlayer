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

    // 二次解析：搜索命中后逐条取详情换直链（串行，避免一口气打出几十个请求）
    void startNextDetail();
    void onDetailFinished();
    void finishSearch();
    void appendItem(const MediaItem &base, const QString &stream);

    SourceConfig m_config;
    HttpClient *m_http;
    QPointer<QNetworkReply> m_reply;
    QString m_keyword;
    int m_limit = 20;
    bool m_done = false;

    QVector<MediaItem> m_result;
    int m_filtered = 0;

    QVector<MediaItem> m_pendingBases;
    QVector<QString> m_pendingIds;
    QVector<QString> m_pendingTitles;
    QVector<QString> m_pendingArtists;
    int m_detailIndex = 0;
    QPointer<QNetworkReply> m_detailReply;
};

#endif // CONFIGSEARCHPROVIDER_H
