#include "download/DownloadTask.h"

#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "util/HeaderPolicy.h"
#include "util/PlaylistSniffer.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <utility>

Q_LOGGING_CATEGORY(lcDownload, "mvp.download")

namespace {

const QRegularExpression kUriAttribute(QStringLiteral("URI=\"([^\"]*)\""));
const QRegularExpression kByteRangeAttribute(QStringLiteral("BYTERANGE=\"([^\"]*)\""));
const QRegularExpression kBandwidth(QStringLiteral("BANDWIDTH=(\\d+)"));
const QRegularExpression kContentRange(QStringLiteral("^bytes\\s+(\\d+)-(\\d+)/(\\d+|\\*)"),
                                       QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kRequestRange(QStringLiteral("^bytes=(\\d+)-(\\d+)$"),
                                        QRegularExpression::CaseInsensitiveOption);

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
    // 源站不能默认成流地址：CDN 直链会把 Cookie 当成发给 CDN 自己的。
    , m_originUrl(HeaderPolicy::originFromHeaders(headers))
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
    return PlaylistSniffer::metadataIndicatesPlaylist(url, QString());
}

bool DownloadTask::openTargetFile()
{
    if (m_file != nullptr)
        return true;

    const QFileInfo info(partialPath());
    QDir().mkpath(info.absolutePath());

    if (m_resumeBytes > 0)
        m_bytesWritten = m_resumeBytes;
    else
        m_bytesWritten = 0;

    m_file = new QFile(partialPath(), this);
    if (!m_file->open(QIODevice::ReadWrite)) {
        fail(tr("无法写入临时文件：%1").arg(m_file->errorString()), true);
        return false;
    }

    const qint64 actual = m_file->size();
    if (actual < m_bytesWritten) {
        // 状态文件和实际临时文件不一致时不能盲目续传，从头开始最安全。
        qCWarning(lcDownload) << "临时文件比续传点短，改为从头下载 actual=" << actual
                              << "resume=" << m_bytesWritten;
        m_bytesWritten = 0;
        m_resumeSegmentIndex = 0;
    } else if (actual > m_bytesWritten) {
        // 上次进程在写盘过程中退出，截掉没有记录进状态文件的尾部。
        if (!m_file->resize(m_bytesWritten)) {
            fail(tr("无法截断临时文件：%1").arg(m_file->errorString()), true);
            return false;
        }
    }
    if (!m_file->seek(m_bytesWritten)) {
        fail(tr("无法定位临时文件写位置：%1").arg(m_file->errorString()), true);
        return false;
    }

    if (m_bytesWritten > m_maxBytes) {
        fail(tr("已有临时文件超过 %1 MB 上限").arg(m_maxBytes / (1024 * 1024)), true);
        return false;
    }
    return true;
}

void DownloadTask::resetTargetFile()
{
    if (m_file == nullptr)
        return;
    m_file->resize(0);
    m_file->seek(0);
    m_bytesWritten = 0;
}

void DownloadTask::setResumeState(qint64 bytesWritten, int segmentIndex, bool hls)
{
    m_resumeBytes = qMax<qint64>(0, bytesWritten);
    m_resumeSegmentIndex = qMax(0, segmentIndex);
    if (hls)
        m_isHls = true;
}

void DownloadTask::setOriginUrl(const QUrl &origin)
{
    if (origin.isValid() && !origin.host().isEmpty())
        m_originUrl = origin;
}

void DownloadTask::setPathResolver(std::function<QString(const QString &)> resolver)
{
    m_pathResolver = std::move(resolver);
}

QString DownloadTask::partialPath() const
{
    return m_filePath + QStringLiteral(".mvpdownload");
}

void DownloadTask::startRequest()
{
    if (m_done)
        return;
    if (m_http == nullptr || m_url.isEmpty()
        || (m_url.scheme() != QLatin1String("http")
            && m_url.scheme() != QLatin1String("https"))) {
        fail(tr("下载地址无效"), true);
        return;
    }

    if (!openTargetFile())
        return;

    if (m_isHls) {
        // 已知是 HLS：先重新拉播放列表，再按 m_resumeSegmentIndex 跳过已完成分片。
        startPlaylistFetch(m_url, 0);
        return;
    }
    startDirectRequest();
}

void DownloadTask::startDirectRequest()
{
    if (m_done)
        return;

    QByteArray range;
    if (m_bytesWritten > 0)
        range = QStringLiteral("bytes=%1-").arg(m_bytesWritten).toUtf8();

    QPointer<DownloadTask> self(this);
    const QHash<QString, QString> headers =
        HeaderPolicy::sanitizedForTarget(m_url, m_headers, m_originUrl);
    m_http->stream(m_url, range, headers, [self](QNetworkReply *reply) {
        if (self.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        self->onHeaders(reply);
    }, m_originUrl);
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
        // 响应体是播放列表，不是媒体字节；之前按 URL 猜的 .mp4/.mp3
        // 扩展名要改成 .ts，否则合并出来的是 TS 容器却挂着 MP4 后缀。
        if (!switchToHlsFile())
            return;
        m_expectedBytes = -1;
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            onPlaylistFinished(reply);
        });
        // 播放列表不是媒体字节，必须在接收过程中就设上限；
        // 否则异常大的 m3u8 仍会先被 QNetworkReply 缓冲。
        HttpClient::limitReplySize(reply, NetLimits::kPlaylistBytes);
        if (reply->isFinished())
            onPlaylistFinished(reply);
        return;
    }

    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (!isMediaContentType(contentType)) {
        fail(tr("下载链接返回的是网页/接口数据（%1），不是音视频").arg(contentType),
             true);
        return;
    }

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    const QByteArray contentRange = reply->rawHeader(QByteArrayLiteral("Content-Range"));
    const QByteArray encoding = reply->rawHeader(QByteArrayLiteral("Content-Encoding"));
    const bool identityEncoding =
        encoding.isEmpty() || encoding.compare("identity", Qt::CaseInsensitive) == 0;

    if (m_bytesWritten > 0) {
        if (status == 206) {
            qint64 start = -1;
            qint64 end = -1;
            qint64 total = -1;
            if (!parseContentRange(contentRange, &start, &end, &total)
                || start != m_bytesWritten) {
                fail(tr("服务器返回的续传起点不正确（%1），已停止")
                         .arg(QString::fromLatin1(contentRange)));
                return;
            }
            m_expectedBytes = total;
            qCInfo(lcDownload) << "从已有临时文件续传，偏移" << m_bytesWritten;
        } else {
            // 服务器忽略 Range，只能从头写；清空临时文件避免前半段重复。
            resetTargetFile();
            qCWarning(lcDownload) << "服务器不支持 Range，已从头重新下载";
            m_expectedBytes = (length.isValid() && identityEncoding)
                                  ? length.toLongLong()
                                  : -1;
        }
    } else if (status == 206) {
        qint64 start = -1;
        qint64 end = -1;
        qint64 total = -1;
        if (parseContentRange(contentRange, &start, &end, &total) && start >= 0) {
            m_expectedBytes = total;
        } else {
            m_expectedBytes = (length.isValid() && identityEncoding)
                                  ? length.toLongLong()
                                  : -1;
        }
    } else {
        m_expectedBytes = (length.isValid() && identityEncoding)
                              ? length.toLongLong()
                              : -1;
    }

    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (!m_done)
            writeReplyData(reply);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        onDirectFinished(reply);
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

    bool tooLarge = false;
    const QByteArray body =
        HttpClient::readAllLimited(reply, NetLimits::kPlaylistBytes, &tooLarge);
    const QUrl baseUrl = reply->url();
    m_reply = nullptr;
    reply->deleteLater();

    if (tooLarge) {
        fail(tr("播放列表超过 4 MB 上限"));
        return;
    }
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

    int resumeIndex = m_isHls ? m_resumeSegmentIndex : 0;
    if (resumeIndex > m_segments.size()) {
        qCWarning(lcDownload) << "播放列表分片数少于续传序号，改为从头下载 resume="
                              << resumeIndex << "count=" << m_segments.size();
        resetTargetFile();
        m_segmentIndex = 0;
    } else {
        m_segmentIndex = resumeIndex;
    }

    emit progress(m_segmentIndex, m_segments.size());
    if (m_segmentIndex >= m_segments.size()) {
        // 上次可能已经拉完所有分片，只差改名落盘。
        commitFile();
        return;
    }
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
    const QHash<QString, QString> headers =
        HeaderPolicy::sanitizedForTarget(url, m_headers, m_originUrl);
    m_http->stream(url, QByteArray(), headers, [self](QNetworkReply *reply) {
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
        // 播放列表不是媒体字节，必须在接收过程中就设上限；否则异常大的
        // m3u8 会先被 QNetworkReply 完整缓冲，finished 后的 4 MB 检查太晚。
        HttpClient::limitReplySize(reply, NetLimits::kPlaylistBytes);
        self->connect(reply, &QNetworkReply::finished, self.data(), [self, reply] {
            if (!self.isNull())
                self->onPlaylistFinished(reply);
        });
        if (reply->isFinished() && !self.isNull())
            self->onPlaylistFinished(reply);
    }, m_originUrl);
}

bool DownloadTask::parsePlaylist(const QByteArray &body, const QUrl &baseUrl, QUrl *variantUrl,
                                bool *isMaster, QString *error)
{
    if (isMaster != nullptr)
        *isMaster = false;

    const QString text = QString::fromUtf8(body);
    if (!PlaylistSniffer::bodyIsPlaylist(body)) {
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

                // #EXT-X-MAP 也可能带 BYTERANGE="length@offset"；只取 URI
                // 会把整个 init 段都拼进去，fMP4 的 moov 索引会错位。
                QByteArray mapRange;
                const QRegularExpressionMatch rangeMatch =
                    kByteRangeAttribute.match(line);
                if (rangeMatch.hasMatch()) {
                    const QString value = rangeMatch.captured(1).trimmed();
                    const QStringList parts = value.split(QLatin1Char('@'));
                    bool lengthOk = false;
                    const qint64 length = parts.value(0).toLongLong(&lengthOk);
                    qint64 offset = 0;
                    if (parts.size() > 1) {
                        bool offsetOk = false;
                        offset = parts.value(1).toLongLong(&offsetOk);
                        if (!offsetOk || offset < 0)
                            lengthOk = false;
                    }
                    if (!lengthOk || length <= 0) {
                        if (error != nullptr)
                            *error = tr("初始化分片字节范围无效：%1").arg(value);
                        return false;
                    }
                    mapRange = QStringLiteral("bytes=%1-%2")
                                   .arg(offset)
                                   .arg(offset + length - 1)
                                   .toUtf8();
                }
                m_segments.append({mapUrl, mapRange});
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
        if (m_segments.size() >= NetLimits::kPlaylistSegments) {
            if (error != nullptr)
                *error = tr("分片数量超过 %1 上限").arg(NetLimits::kPlaylistSegments);
            return false;
        }
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
    m_segmentBytes = 0;
    m_segmentExpected = expectedRangeBytes(segment.range);
    QPointer<DownloadTask> self(this);
    const QHash<QString, QString> headers =
        HeaderPolicy::sanitizedForTarget(segment.url, m_headers, m_originUrl);
    m_http->stream(segment.url, segment.range, headers, [self, segment](QNetworkReply *reply) {
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
        if (segment.range.isEmpty()) {
            const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
            const QByteArray encoding = reply->rawHeader(QByteArrayLiteral("Content-Encoding"));
            self->m_segmentExpected =
                (length.isValid() && (encoding.isEmpty()
                                      || encoding.compare("identity", Qt::CaseInsensitive) == 0))
                ? length.toLongLong()
                : -1;
        }
        if (!segment.range.isEmpty()
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
            self->fail(QObject::tr("服务器没有按字节范围返回分片，已取消以避免生成损坏文件"));
            return;
        }
        if (!segment.range.isEmpty()) {
            qint64 start = -1;
            qint64 end = -1;
            const QByteArray contentRange =
                reply->rawHeader(QByteArrayLiteral("Content-Range"));
            if (!parseContentRange(contentRange, &start, &end)) {
                self->fail(QObject::tr("分片缺少有效的 Content-Range，已取消"));
                return;
            }
            const QRegularExpressionMatch requestMatch =
                kRequestRange.match(QString::fromLatin1(segment.range));
            if (requestMatch.hasMatch()
                && (start != requestMatch.captured(1).toLongLong()
                    || end != requestMatch.captured(2).toLongLong())) {
                self->fail(QObject::tr("服务器返回了错误的字节范围（%1），已取消")
                               .arg(QString::fromLatin1(contentRange)));
                return;
            }
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
    }, m_originUrl);
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

    if (m_segmentExpected >= 0 && m_segmentBytes != m_segmentExpected) {
        fail(tr("分片长度不完整：期望 %1 字节，实际 %2 字节")
                 .arg(m_segmentExpected)
                 .arg(m_segmentBytes));
        return;
    }

    m_segmentExpected = -1;
    m_segmentBytes = 0;
    ++m_segmentIndex;
    // 只在完整分片边界记录续传点：进程中途退出时，状态文件里的字节数
    // 对应的一定是完整分片前缀，重启后可安全截断半截分片并重下该片。
    emit checkpoint(m_bytesWritten, m_segmentIndex, m_segments.size());
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

    if (m_expectedBytes >= 0 && m_bytesWritten != m_expectedBytes) {
        fail(tr("下载长度不完整：期望 %1 字节，实际 %2 字节")
                 .arg(m_expectedBytes)
                 .arg(m_bytesWritten));
        return;
    }

    commitFile();
}

void DownloadTask::writeReplyData(QNetworkReply *reply)
{
    if (reply == nullptr || m_file == nullptr)
        return;

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return;

    if (m_bytesWritten + chunk.size() > m_maxBytes) {
        fail(tr("文件超过 %1 MB 上限，已停止下载").arg(m_maxBytes / (1024 * 1024)));
        return;
    }
    const qint64 written = m_file->write(chunk);
    if (written != chunk.size()) {
        fail(tr("写入文件不完整：%1").arg(m_file->errorString()));
        return;
    }
    m_bytesWritten += chunk.size();
    m_segmentBytes += chunk.size();

    if (m_isHls) {
        // HLS 的续传点按分片边界记录，避免把半截分片写进状态。
        emit progress(m_segmentIndex, m_segments.size());
    } else {
        emit progress(m_bytesWritten, m_expectedBytes);
        emit checkpoint(m_bytesWritten, 0, -1);
    }
}

bool DownloadTask::switchToHlsFile()
{
    if (m_filePath.endsWith(QLatin1String(".ts"), Qt::CaseInsensitive)) {
        m_isHls = true;
        return true;
    }

    const int slash = m_filePath.lastIndexOf(QLatin1Char('/'));
    const int dot = m_filePath.lastIndexOf(QLatin1Char('.'));
    QString base = m_filePath;
    if (dot > slash && dot >= 0)
        base = m_filePath.left(dot);
    QString newPath = base + QStringLiteral(".ts");
    if (m_pathResolver) {
        const QString resolved = m_pathResolver(newPath);
        if (!resolved.isEmpty())
            newPath = resolved;
    }

    if (m_file != nullptr) {
        const QString oldPartial = partialPath();
        m_file->close();
        m_file->deleteLater();
        m_file = nullptr;
        // 此刻还没有任何媒体字节；清掉按错误扩展名建的空临时文件。
        QFile::remove(oldPartial);
    }

    m_filePath = newPath;
    m_bytesWritten = 0;
    m_resumeSegmentIndex = 0;
    m_isHls = true;
    if (!openTargetFile())
        return false;
    emit targetPathChanged(m_filePath, true);
    return true;
}

void DownloadTask::commitFile()
{
    if (m_file == nullptr) {
        fail(tr("下载文件已失效"));
        return;
    }

    if (!m_file->flush()) {
        fail(tr("刷新文件失败：%1").arg(m_file->errorString()));
        return;
    }
    m_file->close();
    const QString temp = partialPath();
    const QString originalPath = m_filePath;
    m_file->deleteLater();
    m_file = nullptr;

    // 下载期间用户可能新建了同名文件，另一个任务也可能刚好完成并占用了
    // 这个路径。最终提交必须重新确认，冲突时换成新名字，绝不删除已有文件。
    QString finalPath = m_filePath;
    if (QFileInfo::exists(finalPath) && m_pathResolver) {
        const QString resolved = m_pathResolver(finalPath);
        if (!resolved.isEmpty())
            finalPath = resolved;
    }
    m_filePath = finalPath;

    if (!QFile::rename(temp, finalPath)) {
        // 失败时恢复到原目标路径：manager 里的 job.filePath 仍是原路径，
        // 临时文件也是按原路径命名的；否则重试会拿错 .mvpdownload 路径。
        m_filePath = originalPath;
        fail(tr("保存文件失败：无法把临时文件改名到 %1").arg(finalPath));
        return;
    }

    qCInfo(lcDownload) << "下载完成，已保存到" << m_filePath
                       << (m_isHls ? "（HLS 分片已合并）" : "");
    emit succeeded(m_filePath);
    finish();
}

void DownloadTask::fail(const QString &message, bool removePartial)
{
    if (m_done)
        return;

    // 必须先置 done 再 abort：abort() 会同步发出 finished，若此时 m_done
    // 仍为 false，onDirectFinished 会把“取消”再当成一次失败，导致 UI 收到
    // 两条 failed（集成测试已复现：字节超限时先报 Operation canceled）。
    m_done = true;

    if (!m_reply.isNull()) {
        QNetworkReply *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (m_file != nullptr) {
        m_file->flush();
        m_file->close();
        const QString temp = partialPath();
        m_file->deleteLater();
        m_file = nullptr;
        // 下载失败默认保留临时文件，下载管理器可以重试续传；
        // 地址无效/返回网页等根本不可能成功的情况才删掉空文件。
        if (removePartial || m_bytesWritten == 0)
            QFile::remove(temp);
    }

    qCWarning(lcDownload) << "下载失败：" << message;
    emit failed(message);
    emit finished();
    deleteLater();
}

void DownloadTask::stopForExit()
{
    if (m_done)
        return;
    m_done = true;

    if (!m_reply.isNull()) {
        QNetworkReply *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (m_file != nullptr) {
        m_file->flush();
        m_file->close();
        m_file->deleteLater();
        m_file = nullptr;
    }

    // 不发出 failed：退出不是下载失败，管理器会保留状态供下次续传。
    emit finished();
    deleteLater();
}

void DownloadTask::cancel()
{
    if (m_done)
        return;

    // 与 fail() 同理：先置 done，避免 abort 同步触发的 finished 再报一次失败。
    m_done = true;

    if (!m_reply.isNull()) {
        QNetworkReply *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    const QString temp = partialPath();
    if (m_file != nullptr) {
        m_file->close();
        m_file->deleteLater();
        m_file = nullptr;
    }
    // 只删当前任务自己的半成品；最终文件可能已经属于另一个同名任务或用户，
    // 不能在这里无条件删除。
    QFile::remove(temp);

    emit failed(tr("下载已取消"));
    emit finished();
    deleteLater();
}

void DownloadTask::setMaxBytes(qint64 bytes)
{
    if (bytes > 0)
        m_maxBytes = bytes;
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
    if (PlaylistSniffer::metadataIndicatesPlaylist(reply->url(), type))
        return true;
    // 部分源把 m3u8 以 text/plain 返回，且签名地址没有 .m3u8 后缀。
    // 这类响应先按播放列表处理；解析正文时再用 #EXTM3U 确认，
    // 不会把 m3u8 文本当成媒体字节写进目标文件。
    return type.startsWith(QLatin1String("text/plain"));
}

bool DownloadTask::isMediaContentType(const QString &contentType)
{
    const QString type = contentType.toLower().trimmed();
    if (type.isEmpty())
        return true;
    if (type.startsWith(QLatin1String("audio/"))
        || type.startsWith(QLatin1String("video/"))
        || type.contains(QLatin1String("octet-stream"))
        || type.contains(QLatin1String("mpegurl")))
        return true;
    // 网页、接口 JSON/XML、图片、PDF 都不是音视频，不能落成媒体文件。
    return false;
}

bool DownloadTask::parseContentRange(const QByteArray &value, qint64 *start, qint64 *end,
                                     qint64 *total)
{
    if (value.isEmpty() || start == nullptr || end == nullptr)
        return false;
    const QRegularExpressionMatch match = kContentRange.match(QString::fromLatin1(value));
    if (!match.hasMatch())
        return false;
    bool startOk = false;
    bool endOk = false;
    const qint64 first = match.captured(1).toLongLong(&startOk);
    const qint64 last = match.captured(2).toLongLong(&endOk);
    if (!startOk || !endOk || first < 0 || last < first)
        return false;
    *start = first;
    *end = last;
    if (total != nullptr) {
        bool totalOk = false;
        const qint64 parsedTotal = match.captured(3).toLongLong(&totalOk);
        *total = (totalOk && parsedTotal > last) ? parsedTotal : -1;
    }
    return true;
}

qint64 DownloadTask::expectedRangeBytes(const QByteArray &range)
{
    if (range.isEmpty())
        return 0;
    const QRegularExpressionMatch match = kRequestRange.match(QString::fromLatin1(range));
    if (!match.hasMatch())
        return -1;
    bool startOk = false;
    bool endOk = false;
    const qint64 start = match.captured(1).toLongLong(&startOk);
    const qint64 end = match.captured(2).toLongLong(&endOk);
    if (!startOk || !endOk || end < start)
        return -1;
    return end - start + 1;
}
