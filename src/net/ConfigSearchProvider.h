#ifndef CONFIGSEARCHPROVIDER_H
#define CONFIGSEARCHPROVIDER_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QObject>
#include <QPointer>

class HttpClient;
class QNetworkReply;

/// 一个由配置描述驱动的搜索源：发请求 → 取 JSON → 按路径取值 → 建 MediaItem。
/// 所有平台差异都写在配置里，这个类本身不含任何平台知识。
class ConfigSearchProvider : public QObject
{
    Q_OBJECT

public:
    ConfigSearchProvider(const SourceConfig &config, HttpClient *http, QObject *parent = nullptr);

    const SourceConfig &config() const { return m_config; }

    void search(const QString &keyword, int limit);

signals:
    /// error 为空表示成功；失败时 items 为空
    void finished(const QString &sourceId, const QVector<MediaItem> &items, int filtered,
                  const QString &error);

private:
    void onReplyFinished();

    SourceConfig m_config;
    HttpClient *m_http;
    QPointer<QNetworkReply> m_reply;
    QString m_keyword;
    int m_limit = 20;
    bool m_done = false;
};

#endif // CONFIGSEARCHPROVIDER_H
