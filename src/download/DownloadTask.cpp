#include "download/DownloadTask.h"

#include "net/HttpClient.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

Q_LOGGING_CATEGORY(lcDownload, "mvp.download")

namespace {

const QRegularExpression kUriAttribute(QStringLiteral("URI=\"([^\"]*)\""));
const QRegularExpression kBandwidth(QStringLiteral("BANDWIDTH=(\\d+)"));

QUrl resolveUrl(const QUrl &base, const QString &raw)
{
    const QString uri = raw.trimmed();
    if (uri.isEmpty())
        return QUrl();
    return base.resolved(QUrl(uri));
}

} // namespace

DownloadTask::DownloadTask(HttpClient *http,
                           const QUrl &url,
                           const QString &filePath,
                           const QHash<QString, QString> &headers,
                           QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_url(url)
    , m_filePath(filePath)
    , m_headers(headers)
{
}

DownloadTask *DownloadTask::start(HttpClient *http,
                                  const QUrl &url,
                                  const QString &filePath,
                                  QObject *parent)
{
    return start(http, url, filePath, QHash<QString, QString>(), parent);
}

DownloadTask *DownloadTask::start(HttpClient *http,
                                  const QUrl &url,
                                  const QString &filePath,
                                  const QHash<QString, QString> &headers,
                                  QObject *parent)
{
    auto *task = new DownloadTask(http, url, filePath, headers, parent);
    // 延迟到事件循环：调用方在 start() 返回后立刻 connect 也不会漏掉
    // “地址无效/文件打不开”这类同步 failed 信号。
    QTimer::singleShot(0, task, &DownloadTask::startRequest);
    return task;
}

QString DownloadTask::sanitizeFileName(const QString &name, const QString &suffix)
{
    QString base = name;
    base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\x00-\x1F])")),
                 QStringLiteral("_"));
    base = base.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("download");
    if (base.size() > 120)
        base.truncate(120);

    const QString tail = QStringLiteral(".") + suffix;
    if (!base.endsWith(tail, Qt::CaseInsensitive))
        base += tail;
    return base;
}

bool DownloadTask::looksLikeHls(const QUrl &url)
{
    return url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive)
        || url.toString().contains(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

void DownloadTask::startRequest()
{
    if (m_done)
        return;
    if (m_http == nullptr || m_url.isEmpty()
        || (m_url.scheme() != QLatin1String("http")
            && m_url.scheme() != QLatin1String("https"))) {
        fail(tr("下载地址无效"));
        return;
    }

    m_file = new QSaveFile(m_filePath, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        fail(tr("无法写入文件：%1").arg(m_file->errorString()));
        return;
    }

    QPointer<DownloadTask> self(this);
    m_http->stream(m_url, QByteArray(), m_headers, [self](QNetworkReply *reply) {
        if (self.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        self->onHeaders(reply);
    });
}

void DownloadTask::onHeaders(QNetworkReply *reply)
{
    if (m_done) {
        if (reply != nullptr) {
            reply->abort();
            reply->deleteLater();
        }
        return;
    }
    if (reply == nullptr) {
        fail(tr("重定向过多或地址不被允许"));
        return;
    }

    m_reply = reply;

    if (!HttpClient::isHttpSuccess(reply)) {
        fail(HttpClient::describeError(reply));
        return;
    }

    if (isPlaylistReply(reply)) {
        m_isHls = true;
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            onPlaylistFinished(reply);
        });
        if (reply->isFinished())
            onPlaylistFinished(reply);
        return;
    }

    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (!isMediaContentType(contentType)) {
        fail(tr("下载链接返回的是网页/接口数据（%1），不是音视频").arg(contentType));
        return;
    }

    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (!m_done)
            writeReplyData(reply);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        onDirectFinished(reply);
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (!m_done)
                    emit progress(received, total);
            });

    if (reply->bytesAvailable() > 0)
        writeReplyData(reply);
    if (reply->isFinished())
        onDirectFinished(reply);
}

void DownloadTask::onPlaylistFinished(QNetworkReply *reply)
{
    if (m_done || reply == nullptr)
        return;

    if (!HttpClient::isHttpSuccess(reply)) {
        fail(HttpClient::describeError(reply));
        return;
    }

    const QByteArray body = reply->readAll();
    const QUrl baseUrl = reply->url();
    m_reply = nullptr;
    reply->deleteLater();

    if (body.isEmpty()) {
        fail(tr("播放列表为空"));
        return;
    }

    QUrl variantUrl;
    bool isMaster = false;
    QString error;
    if (!parsePlaylist(body, baseUrl, &variantUrl, &isMaster, &error)) {
        fail(error);
        return;
    }

    if (isMaster) {
        if (m_playlistHop >= 2) {
            fail(tr("播放列表嵌套层级过深，无法确定码率"));
            return;
        }
        startPlaylistFetch(variantUrl, m_playlistHop + 1);
        return;
    }

    if (m_segments.isEmpty()) {
        fail(tr("播放列表里没有可下载的分片（可能是直播流或字段为空）"));
        return;
    }

    qCInfo(lcDownload) << "HLS 播放列表解析完成，分片数：" << m_segments.size();
    m_segmentIndex = 0;
    emit progress(0, m_segments.size());
    startNextSegment();
}

void DownloadTask::startPlaylistFetch(const QUrl &url, int hop)
{
    if (m_done)
        return;
    if (!url.isValid() || url.isEmpty()) {
        fail(tr("子播放列表地址无效"));
        return;
    }
    if (hop > 3) {
        fail(tr("播放列表嵌套层级过深"));
        return;
    }

    m_playlistHop = hop;
    QPointer<DownloadTask> self(this);
    m_http->stream(url, QByteArray(), m_headers, [self](QNetworkReply *reply) {
        if (self.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        if (reply == nullptr) {
            self->fail(QObject::tr("子播放列表重定向失败"));
            return;
        }
        self->m_reply = reply;
        self->connect(reply, &QNetworkReply::finished, self.data(), [self, reply] {
            if (!self.isNull())
                self->onPlaylistFinished(reply);
        });
        if (reply->isFinished() && !self.isNull())
            self->onPlaylistFinished(reply);
    });
}

bool DownloadTask::parsePlaylist(const QByteArray &body, const QUrl &baseUrl, QUrl *variantUrl,
                                bool *isMaster, QString *error)
{
    if (isMaster != nullptr)
        *isMaster = false;

    const QString text = QString::fromUtf8(body);
    if (!text.contains(QLatin1String("#EXTM3U"))) {
        if (error != nullptr)
            *error = tr("服务器返回的不是 m3u8 播放列表（可能是网页或错误页）");
        return false;
    }
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    // master playlist：选码率最高的 variant；#EXT-X-STREAM-INF 的下一行才是地址。
    if (text.contains(QLatin1String("#EXT-X-STREAM-INF"))) {
        // 主列表里的 SESSION-KEY 会作用到所有子列表，子列表可能不再重复写 KEY。
        const QRegularExpression methodRe(QStringLiteral("METHOD=([^,\\s]+)"));
        for (const QString &line : lines) {
            if (!line.trimmed().startsWith(QLatin1String("#EXT-X-SESSION-KEY")))
                continue;
            const QRegularExpressionMatch match = methodRe.match(line);
            const QString method = match.hasMatch() ? match.captured(1) : QString();
            if (!method.isEmpty()
                && method.compare(QLatin1String("NONE"), Qt::CaseInsensitive) != 0) {
                if (error != nullptr)
                    *error = tr("该播放列表使用了 %1 加密，当前版本暂不支持解密下载；"
                                "已取消，未生成损坏文件。")
                                 .arg(method);
                return false;
            }
        }

        int bestBandwidth = -1;
        QUrl bestUrl;
        bool pending = false;
        int pendingBandwidth = 0;

        for (QString line : lines) {
            line = line.trimmed();
            if (line.isEmpty())
                continue;
            if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF"))) {
                pending = true;
                const QRegularExpressionMatch match = kBandwidth.match(line);
                pendingBandwidth = match.hasMatch() ? match.captured(1).toInt() : 0;
                continue;
            }
            if (pending) {
                if (!line.startsWith(QLatin1Char('#'))) {
                    const QUrl candidate = resolveUrl(baseUrl, line);
                    if (candidate.isValid()
                        && pendingBandwidth >= bestBandwidth) {
                        bestBandwidth = pendingBandwidth;
                        bestUrl = candidate;
                    }
                }
                pending = false;
            }
        }

        if (!bestUrl.isValid()) {
            if (error != nullptr)
                *error = tr("主播放列表里没有找到可用的清晰度地址");
            return false;
        }
        if (variantUrl != nullptr)
            *variantUrl = bestUrl;
        if (isMaster != nullptr)
            *isMaster = true;
        return true;
    }

    m_segments.clear();
    m_nextRangeOffset = 0;

    QByteArray pendingRange;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-SESSION-KEY"))) {
            const QRegularExpression methodRe(QStringLiteral("METHOD=([^,\\s]+)"));
            const QRegularExpressionMatch match = methodRe.match(line);
            const QString method = match.hasMatch() ? match.captured(1) : QString();
            if (method.compare(QLatin1String("NONE"), Qt::CaseInsensitive) != 0
                && !method.isEmpty()) {
                if (error != nullptr)
                    *error = tr("该播放列表使用了 %1 加密，当前版本暂不支持解密下载；"
                                "已取消，未生成损坏文件。")
                                 .arg(method);
                return false;
            }
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-KEY"))) {
            const QRegularExpression methodRe(QStringLiteral("METHOD=([^,\\s]+)"));
            const QRegularExpressionMatch match = methodRe.match(line);
            const QString method = match.hasMatch() ? match.captured(1) : QString();
            if (method.compare(QLatin1String("NONE"), Qt::CaseInsensitive) != 0
                && !method.isEmpty()) {
                if (error != nullptr) {
                    *error = tr("该播放列表使用了 %1 加密，当前版本暂不支持解密下载；"
                                "已取消，未生成损坏文件。")
                                 .arg(method);
                }
                return false;
            }
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-BYTERANGE:"))) {
            const QString value =
                line.mid(QStringLiteral("#EXT-X-BYTERANGE:").size()).trimmed();
            const QStringList parts = value.split(QLatin1Char('@'));
            bool ok = false;
            const qint64 length = parts.value(0).toLongLong(&ok);
            if (!ok || length <= 0) {
                if (error != nullptr)
                    *error = tr("分片字节范围格式无效：%1").arg(value);
                return false;
            }
            qint64 offset = m_nextRangeOffset;
            if (parts.size() > 1) {
                bool offsetOk = false;
                offset = parts.value(1).toLongLong(&offsetOk);
                if (!offsetOk || offset < 0) {
                    if (error != nullptr)
                        *error = tr("分片字节范围偏移无效：%1").arg(value);
                    return false;
                }
            }
            const qint64 end = offset + length - 1;
            pendingRange = QStringLiteral("bytes=%1-%2").arg(offset).arg(end).toUtf8();
            m_nextRangeOffset = offset + length;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-MAP:"))) {
            const QRegularExpressionMatch match = kUriAttribute.match(line);
            if (match.hasMatch()) {
                const QUrl mapUrl = resolveUrl(baseUrl, match.captured(1));
                if (!mapUrl.isValid() || mapUrl.isEmpty()
                    || (mapUrl.scheme() != QLatin1String("http")
                        && mapUrl.scheme() != QLatin1String("https"))) {
                    if (error != nullptr)
                        *error = tr("初始化分片地址无效");
                    return false;
                }
                m_segments.append({mapUrl, QByteArray()});
            }
            continue;
        }

        if (line.startsWith(QLatin1Char('#')))
            continue;

        const QUrl segmentUrl = resolveUrl(baseUrl, line);
        if (!segmentUrl.isValid()
            || (segmentUrl.scheme() != QLatin1String("http")
                && segmentUrl.scheme() != QLatin1String("https"))) {
            if (error != nullptr)
                *error = tr("分片地址无效：%1").arg(line);
            return false;
        }

        Segment segment;
        segment.url = segmentUrl;
        segment.range = pendingRange;
        pendingRange.clear();
        m_segments.append(segment);
    }

    return true;
}

void DownloadTask::startNextSegment()
{
    if (m_done)
        return;
    if (m_segmentIndex >= m_segments.size()) {
        commitFile();
        return;
    }

    const Segment segment = m_segments.at(m_segmentIndex);
    QPointer<DownloadTask> self(this);
    m_http->stream(segment.url, segment.range, m_headers, [self, segment](QNetworkReply *reply) {
        if (self.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        if (self->m_done) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        if (reply == nullptr) {
            self->fail(QObject::tr("分片地址重定向失败"));
            return;
        }

        self->m_reply = reply;
        if (!HttpClient::isHttpSuccess(reply)) {
            self->fail(HttpClient::describeError(reply));
            return;
        }
        if (!segment.range.isEmpty()
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
            self->fail(QObject::tr("服务器没有按字节范围返回分片，已取消以避免生成损坏文件"));
            return;
        }

        self->connect(reply, &QNetworkReply::readyRead, self.data(), [self, reply] {
            if (!self.isNull() && !self->m_done)
                self->writeReplyData(reply);
        });
        self->connect(reply, &QNetworkReply::finished, self.data(), [self, reply] {
            if (!self.isNull())
                self->onSegmentFinished(reply);
        });

        if (reply->bytesAvailable() > 0)
            self->writeReplyData(reply);
        if (reply->isFinished())
            self->onSegmentFinished(reply);
    });
}

void DownloadTask::onSegmentFinished(QNetworkReply *reply)
{
    if (m_done || reply == nullptr)
        return;

    if (!HttpClient::isHttpSuccess(reply)) {
        fail(HttpClient::describeError(reply));
        return;
    }

    writeReplyData(reply);
    m_reply = nullptr;
    reply->deleteLater();

    if (m_done)
        return;

    ++m_segmentIndex;
    emit progress(m_segmentIndex, m_segments.size());
    startNextSegment();
}

void DownloadTask::onDirectFinished(QNetworkReply *reply)
{
    if (m_done || reply == nullptr)
        return;

    if (!HttpClient::isHttpSuccess(reply)) {
        fail(HttpClient::describeError(reply));
        return;
    }

    writeReplyData(reply);
    m_reply = nullptr;
    reply->deleteLater();

    if (m_done)
        return;

    commitFile();
}

void DownloadTask::writeReplyData(QNetworkReply *reply)
{
    if (reply == nullptr || m_file.isNull())
        return;

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return;

    if (!m_file->write(chunk)) {
        fail(tr("写入文件失败：%1").arg(m_file->errorString()));
        return;
    }
}

void DownloadTask::commitFile()
{
    if (m_file.isNull()) {
        fail(tr("下载文件已失效"));
        return;
    }

    if (!m_file->commit()) {
        fail(tr("保存文件失败：%1").arg(m_file->errorString()));
        return;
    }
    m_file->deleteLater();
    m_file = nullptr;

    qCInfo(lcDownload) << "下载完成，已保存到" << m_filePath
                       << (m_isHls ? "（HLS 分片已合并）" : "");
    emit succeeded(m_filePath);
    finish();
}

void DownloadTask::fail(const QString &message)
{
    if (m_done)
        return;

    if (!m_reply.isNull()) {
        QNetworkReply *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (!m_file.isNull()) {
        m_file->cancelWriting();
        m_file->deleteLater();
        m_file = nullptr;
    }

    qCWarning(lcDownload) << "下载失败：" << message;
    emit failed(message);
    finish();
}

void DownloadTask::cancel()
{
    if (m_done)
        return;

    if (!m_reply.isNull()) {
        QNetworkReply *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (!m_file.isNull()) {
        m_file->cancelWriting();
        m_file->deleteLater();
        m_file = nullptr;
    }

    emit failed(tr("下载已取消"));
    finish();
}

void DownloadTask::finish()
{
    if (m_done)
        return;
    m_done = true;
    emit finished();
    deleteLater();
}

bool DownloadTask::isPlaylistReply(QNetworkReply *reply)
{
    if (reply == nullptr)
        return false;
    const QString type =
        reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    if (type.contains(QLatin1String("mpegurl")))
        return true;
    if (reply->url().path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive))
        return true;
    return type.isEmpty()
        && reply->url().toString().contains(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

bool DownloadTask::isMediaContentType(const QString &contentType)
{
    const QString type = contentType.toLower();
    if (type.isEmpty())
        return true;
    if (type.startsWith(QLatin1String("audio/"))
        || type.startsWith(QLatin1String("video/"))
        || type.contains(QLatin1String("octet-stream"))
        || type.contains(QLatin1String("mpegurl")))
        return true;
    if (type.startsWith(QLatin1String("text/"))
        || type.contains(QLatin1String("json"))
        || type.contains(QLatin1String("xml"))
        || type.contains(QLatin1String("html")))
        return false;
    return true;
}
