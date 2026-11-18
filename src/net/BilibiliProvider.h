#ifndef BILIBILIPROVIDER_H
#define BILIBILIPROVIDER_H

#include "net/SearchProviderBase.h"
#include "net/SourceConfig.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QVector>

#include <functional>

class HttpClient;

/// B站内置源：搜索接口需要 wbi 签名，播放地址不需要（直接取单文件 mp4）。
/// 签名算法三步：nav 取 img_key/sub_key → 按固定表重排成 mixinKey →
/// 参数排序加时间戳后 md5 得到 w_rid。
class BilibiliProvider : public SearchProviderBase
{
    Q_OBJECT

public:
    BilibiliProvider(const SourceConfig &config, HttpClient *http, QObject *parent = nullptr);

    void search(const QString &keyword, int limit) override;

private:
    struct Entry
    {
        QString bvid;
        QString title;
        QString author;
    };

    using JsonCallback = std::function<void(const QJsonObject &object, const QString &error)>;

    void requestJson(const QUrl &url, const JsonCallback &callback);
    void fetchKeys();
    void doSearch();
    void resolveNext();
    void finish(const QString &error);

    QString signedQuery(QHash<QString, QString> params) const;
    static QString buildMixinKey(const QString &imgKey, const QString &subKey);
    static QString cleanTitle(const QString &raw);

    SourceConfig m_config;
    HttpClient *m_http;

    QString m_keyword;
    int m_limit = 10;
    QString m_mixinKey;
    QDateTime m_keysAt;

    QVector<Entry> m_entries;
    QVector<MediaItem> m_items;
    int m_cursor = 0;
    int m_filtered = 0;
    bool m_done = false;
};

#endif // BILIBILIPROVIDER_H
