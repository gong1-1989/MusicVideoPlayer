#ifndef HLSPRELOADMIGRATOR_H
#define HLSPRELOADMIGRATOR_H

#include "cache/HlsPreloadIndex.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>

class HttpClient;
class QNetworkReply;

/// 旧版合并缓存的“安全迁移”探测器。
///
/// 旧缓存只有 `.video/.audio`，没有逐分片边界。这里只对**未加密、非单文件
/// BYTERANGE**的轨道，逐个向上游取分片的准确长度（`Range: bytes=0-0`），
/// 只有长度累计与续传状态完全一致时才生成索引。任何分片探测失败、长度
/// 不一致或服务器忽略 Range 又没有 Content-Length，都整体放弃，绝不按比例
/// 估算偏移。
class HlsPreloadMigrator : public QObject
{
public:
    using Callback = std::function<void(bool ok, const QList<HlsPreloadEntry> &entries,
                                        const QString &error)>;

    HlsPreloadMigrator(HttpClient *http, QList<QUrl> segmentUrls, int prefixCount,
                       qint64 expectedBytes, const QString &trackPath,
                       const QByteArray &contentType, const QHash<QString, QString> &headers,
                       const QUrl &origin, Callback callback, QObject *parent = nullptr);
    ~HlsPreloadMigrator() override;

    void start();
    void abort();

private:
    void fetchNext();
    void onReply(QNetworkReply *reply, int index);
    void fail(const QString &error);
    void finish();

    HttpClient *m_http = nullptr;
    QList<QUrl> m_urls;
    int m_prefixCount = 0;
    qint64 m_expectedBytes = 0;
    QString m_trackPath;
    QByteArray m_contentType;
    QHash<QString, QString> m_headers;
    QUrl m_origin;
    Callback m_callback;

    QList<qint64> m_sizes;
    QList<QPointer<QNetworkReply>> m_replies;
    int m_nextIndex = 0;
    int m_active = 0;
    bool m_aborted = false;
    bool m_done = false;
};

#endif // HLSPRELOADMIGRATOR_H
