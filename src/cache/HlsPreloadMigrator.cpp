#include "cache/HlsPreloadMigrator.h"

#include "net/HttpClient.h"
#include "stream/StreamRange.h"
#include "util/HeaderPolicy.h"

#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <utility>

namespace {
constexpr int kMigrationConcurrency = 3;
}

HlsPreloadMigrator::HlsPreloadMigrator(HttpClient *http, QList<QUrl> segmentUrls, int prefixCount,
                                       qint64 expectedBytes, const QString &trackPath,
                                       const QByteArray &contentType,
                                       const QHash<QString, QString> &headers, const QUrl &origin,
                                       Callback callback, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_urls(std::move(segmentUrls))
    , m_prefixCount(prefixCount)
    , m_expectedBytes(expectedBytes)
    , m_trackPath(trackPath)
    , m_contentType(contentType)
    , m_headers(headers)
    , m_origin(origin)
    , m_callback(std::move(callback))
{
}

HlsPreloadMigrator::~HlsPreloadMigrator()
{
    abort();
}

void HlsPreloadMigrator::start()
{
    if (m_done || m_aborted)
        return;
    if (m_http == nullptr || m_prefixCount <= 0 || m_prefixCount > m_urls.size()
        || m_expectedBytes <= 0 || m_trackPath.isEmpty()) {
        fail(tr("旧缓存迁移参数不完整"));
        return;
    }
    m_sizes = QList<qint64>(m_prefixCount, -1);
    fetchNext();
}

void HlsPreloadMigrator::abort()
{
    if (m_done && m_replies.isEmpty())
        return;
    m_aborted = true;
    m_done = true;
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_replies)) {
        if (!reply.isNull()) {
            reply->abort();
            reply->deleteLater();
        }
    }
    m_replies.clear();
    m_active = 0;
}

void HlsPreloadMigrator::fetchNext()
{
    while (!m_done && !m_aborted && m_active < kMigrationConcurrency
           && m_nextIndex < m_prefixCount) {
        const int index = m_nextIndex++;
        const QUrl url = m_urls.at(index);
        if (!url.isValid()) {
            fail(tr("旧缓存迁移遇到无效分片地址"));
            return;
        }
        QPointer<HlsPreloadMigrator> self(this);
        const QHash<QString, QString> headers =
            HeaderPolicy::sanitizedForTarget(url, m_headers, m_origin);
        // HttpClient::stream 是异步接口，返回 void；reply 只能在回调里拿到。
        // 先占用并发槽位，回调 onReply 再递减，避免同步/异步实现差异漏记。
        ++m_active;
        m_http->stream(
            url, QByteArrayLiteral("bytes=0-0"), headers,
            [self, index](QNetworkReply *ready) {
                if (!self.isNull())
                    self->onReply(ready, index);
            },
            m_origin);
    }
}

void HlsPreloadMigrator::onReply(QNetworkReply *reply, int index)
{
    if (m_active > 0)
        --m_active;
    if (m_done || m_aborted || reply == nullptr) {
        if (reply != nullptr) {
            reply->abort();
            reply->deleteLater();
        } else if (!m_done && !m_aborted) {
            fail(tr("旧缓存迁移探测请求被拒绝"));
        }
        return;
    }
    m_replies.append(reply);
    if (!HttpClient::isHttpSuccess(reply)) {
        fail(tr("旧缓存迁移探测失败：%1").arg(HttpClient::describeError(reply)));
        reply->deleteLater();
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qint64 total = -1;
    if (status == 206) {
        qint64 start = -1;
        qint64 end = -1;
        if (!StreamRange::parseContentRange(reply->rawHeader(QByteArrayLiteral("Content-Range")),
                                            &start, &end, &total)
            || start != 0 || end != 0 || total <= 0) {
            fail(tr("旧缓存迁移无法取得分片准确长度（Content-Range 无效）"));
            reply->deleteLater();
            return;
        }
    } else if (status == 200) {
        // 源站忽略 Range 时只要 Content-Length 仍然准确；有压缩编码的长度
        // 不是落盘长度，不能用。
        const QByteArray encoding = reply->rawHeader(QByteArrayLiteral("Content-Encoding"));
        if (!encoding.isEmpty() && encoding.compare("identity", Qt::CaseInsensitive) != 0) {
            fail(tr("旧缓存迁移遇到压缩响应，无法取得落盘长度"));
            reply->deleteLater();
            return;
        }
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (!length.isValid()) {
            fail(tr("旧缓存迁移无法取得分片长度"));
            reply->deleteLater();
            return;
        }
        total = length.toLongLong();
        if (total <= 0) {
            fail(tr("旧缓存迁移得到无效分片长度"));
            reply->deleteLater();
            return;
        }
    } else {
        fail(tr("旧缓存迁移返回异常状态码：%1").arg(status));
        reply->deleteLater();
        return;
    }

    // 只要响应头，不读取正文；避免为了迁移把旧分片再下一遍。
    reply->abort();
    reply->deleteLater();
    if (index < 0 || index >= m_sizes.size()) {
        fail(tr("旧缓存迁移索引越界"));
        return;
    }
    m_sizes[index] = total;
    fetchNext();

    for (qint64 size : std::as_const(m_sizes)) {
        if (size <= 0)
            return;
    }
    finish();
}

void HlsPreloadMigrator::fail(const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_replies)) {
        if (!reply.isNull()) {
            reply->abort();
            reply->deleteLater();
        }
    }
    m_replies.clear();
    m_active = 0;
    if (!m_aborted && m_callback)
        m_callback(false, {}, error);
}

void HlsPreloadMigrator::finish()
{
    if (m_done)
        return;
    m_done = true;

    qint64 offset = 0;
    QList<HlsPreloadEntry> entries;
    entries.reserve(m_prefixCount);
    for (int i = 0; i < m_prefixCount; ++i) {
        const qint64 size = m_sizes.at(i);
        HlsPreloadEntry entry;
        entry.url = m_urls.at(i);
        entry.filePath = m_trackPath;
        entry.offset = offset;
        entry.size = size;
        entry.contentType = m_contentType;
        entries.append(entry);
        offset += size;
    }
    if (offset != m_expectedBytes) {
        if (m_callback)
            m_callback(false, {},
                       tr("旧缓存迁移长度不一致：分片累计 %1，续传记录 %2")
                           .arg(offset)
                           .arg(m_expectedBytes));
        return;
    }
    if (m_callback)
        m_callback(true, entries, QString());
}
