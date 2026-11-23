#include "download/DownloadTask.h"

#include "core/FileNameUtils.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "util/HeaderPolicy.h"
#include "util/PlaylistSniffer.h"

#include <QCryptographicHash>
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

const QRegularExpression kUriAttribute(QStringLiteral("URI\\s*=\\s*\"([^\"]*)\""));
const QRegularExpression kUriAttributeBare(QStringLiteral("URI\\s*=\\s*([^,\\s]+)"));
const QRegularExpression kByteRangeAttribute(QStringLiteral("BYTERANGE=\"([^\"]*)\""));
const QRegularExpression kBandwidth(QStringLiteral("BANDWIDTH=(\\d+)"));
const QRegularExpression kContentRange(QStringLiteral("^bytes\\s+(\\d+)-(\\d+)/(\\d+|\\*)"),
                                       QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kRequestRange(QStringLiteral("^bytes=(\\d+)-(\\d+)$"),
                                        QRegularExpression::CaseInsensitiveOption);

QString uriAttribute(const QString &line)
{
    const QRegularExpressionMatch quoted = kUriAttribute.match(line);
    if (quoted.hasMatch())
        return quoted.captured(1).trimmed();
    // RFC 8216 的 URI 允许不加引号；只按引号解析会漏掉 fMP4 的 init 段。
    const QRegularExpressionMatch bare = kUriAttributeBare.match(line);
    return bare.hasMatch() ? bare.captured(1).trimmed() : QString();
}

bool isHttpUrl(const QUrl &url)
{
    return url.isValid() && !url.isEmpty()
        && (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0
            || url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0);
}

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
    const QString tail = QStringLiteral(".") + suffix;
    // 长度限制必须把扩展名算进去，否则 120 字符的主名再加后缀仍会超限。
    const int maxBase = qMax(1, 120 - tail.size());
    QString base = FileNameUtils::sanitizeComponent(name, QStringLiteral("download"),
                                                     maxBase);
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
        // 状态文件和实际临时文件不一致时不能盲目续传；必须真正截断，
        // 否则新响应比旧文件短时尾部会残留，最终长度校验还会通过。
        qCWarning(lcDownload) << "临时文件比续传点短，改为从头下载 actual=" << actual
                              << "resume=" << m_bytesWritten;
        if (!m_file->resize(0)) {
            fail(tr("无法清空临时文件：%1").arg(m_file->errorString()), true);
            return false;
        }
        m_bytesWritten = 0;
        m_resumeSegmentIndex = 0;
        m_file->seek(0);
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
    // 即使文件尚未打开，也必须清空续传计数；否则后续 startRequest 会拿旧偏移。
    if (m_file == nullptr) {
        m_bytesWritten = 0;
        m_resumeSegmentIndex = 0;
        return;
    }
    if (!m_file->resize(0)) {
        fail(tr("无法清空临时文件：%1").arg(m_file->errorString()));
        return;
    }
    m_file->seek(0);
    m_bytesWritten = 0;
    m_resumeSegmentIndex = 0;
}

void DownloadTask::setResumeState(qint64 bytesWritten, int segmentIndex, bool hls)
{
    m_resumeBytes = qMax<qint64>(0, bytesWritten);
    m_resumeSegmentIndex = qMax(0, segmentIndex);
    if (hls)
        m_isHls = true;
}

void DownloadTask::setResumeFingerprint(const QString &fingerprint)
{
    m_resumeFingerprint = fingerprint;
}

void DownloadTask::setOriginUrl(const QUrl &origin)
{
    if (origin.isValid() && !origin.host().isEmpty())
        m_originUrl = origin;
}

void DownloadTask::setAllowIncompletePlaylist(bool allow)
{
    m_allowIncomplete = allow;
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
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (!HttpClient::isHttpSuccess(reply)) {
        // 续传点恰好等于总长时服务器只能回 416。此时临时文件已经是完整文件，
        // 直接提交即可，不能把“只差改名”的任务永久判失败。
        if (status == 416 && m_bytesWritten > 0) {
            qint64 total = -1;
            if (parseUnsatisfiedRangeTotal(
                    reply->rawHeader(QByteArrayLiteral("Content-Range")), &total)
                && total == m_bytesWritten) {
                m_reply = nullptr;
                reply->deleteLater();
                m_expectedBytes = total;
                qCInfo(lcDownload) << "续传点已到文件末尾，直接提交临时文件" << total;
                commitFile();
                return;
            }
        }
        fail(HttpClient::describeError(reply));
        return;
    }

    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (isPlaylistReply(reply)) {
        beginPlaylistReply(reply);
        return;
    }

    const QString normalized = contentType.toLower().trimmed();
    if (normalized.startsWith(QLatin1String("text/plain"))) {
        // text/plain 既可能是无后缀签名的 m3u8，也可能是 MP3/字幕/错误页。
        // 先 peek 正文再决定，绝不能先改扩展名或直接当媒体写盘。
        QPointer<DownloadTask> self(this);
        connect(reply, &QNetworkReply::readyRead, this, [self, reply, normalized] {
            if (!self.isNull() && !self->m_done)
                self->resolveAmbiguousTextReply(reply, normalized);
        });
        connect(reply, &QNetworkReply::finished, this, [self, reply, normalized] {
            if (self.isNull() || self->m_done)
                return;
            self->resolveAmbiguousTextReply(reply, normalized);
        });
        QTimer::singleShot(0, this, [self, reply, normalized] {
            if (!self.isNull() && !self->m_done)
                self->resolveAmbiguousTextReply(reply, normalized);
        });
        resolveAmbiguousTextReply(reply, normalized);
        return;
    }

    if (!isMediaContentType(normalized)) {
        fail(tr("下载链接返回的是网页/接口数据（%1），不是音视频").arg(contentType),
             true);
        return;
    }
    beginDirectMediaReply(reply, normalized);
}

void DownloadTask::beginPlaylistReply(QNetworkReply *reply)
{
    m_isHls = true;
    m_expectedBytes = -1;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        onPlaylistFinished(reply);
    });
    // 播放列表不是媒体字节，必须在接收过程中就设上限；
    // 否则异常大的 m3u8 仍会先被 QNetworkReply 缓冲。
    HttpClient::limitReplySize(reply, NetLimits::kPlaylistBytes);
    if (reply->isFinished())
        onPlaylistFinished(reply);
}

void DownloadTask::resolveAmbiguousTextReply(QNetworkReply *reply,
                                             const QString &contentType)
{
    if (m_ambiguousTextResolved)
        return;

    // peek 不消费正文；确认不是播放列表后，原字节仍会交给 writeReplyData。
    const QByteArray prefix = reply->peek(512);
    // PlaylistSniffer 会剥 UTF-8 BOM 和前导空白；不能直接 startsWith，
    // 否则带 BOM 的 text/plain m3u8 会被误判成普通文本。
    if (PlaylistSniffer::bodyIsPlaylist(prefix)) {
        m_ambiguousTextResolved = true;
        beginPlaylistReply(reply);
        return;
    }

    // 正文还没到齐时继续等；至少拿到足够字节再判断，避免把短前缀误判成文本。
    if (prefix.size() < 64 && !reply->isFinished())
        return;

    m_ambiguousTextResolved = true;
    if (!looksLikeMediaPayload(prefix, reply->url())) {
        fail(tr("下载链接返回的是纯文本/网页数据，不是音视频"), true);
        return;
    }
    beginDirectMediaReply(reply, contentType);
}

void DownloadTask::beginDirectMediaReply(QNetworkReply *reply,
                                         const QString &contentType)
{
    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    const QByteArray contentRange = reply->rawHeader(QByteArrayLiteral("Content-Range"));
    const QByteArray encoding = reply->rawHeader(QByteArrayLiteral("Content-Encoding"));
    const bool identityEncoding =
        encoding.isEmpty() || encoding.compare("identity", Qt::CaseInsensitive) == 0;
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 空 Content-Type 或 text/plain 不能无条件放行；首块写入前用文件头魔数确认。
    m_verifyMediaMagic = contentType.isEmpty()
        || contentType.startsWith(QLatin1String("text/plain"));

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
            if (m_done)
                return;
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

    const QString fingerprint = computePlaylistFingerprint();
    emit playlistFingerprint(fingerprint);

    int resumeIndex = m_isHls ? m_resumeSegmentIndex : 0;
    // 续传必须属于同一份分片结构。旧状态没有指纹时不能冒险拼接；
    // CDN 重新切片/换码率后，即使分片数没变，旧前缀和新分片也可能不是同一时间线。
    if (resumeIndex > 0 && m_resumeFingerprint != fingerprint) {
        qCWarning(lcDownload) << "HLS 分片结构已变化，改为从头下载 resume=" << resumeIndex
                              << "fingerprintChanged=" << !m_resumeFingerprint.isEmpty();
        resetTargetFile();
        if (m_done)
            return;
        resumeIndex = 0;
        m_resumeSegmentIndex = 0;
    } else if (resumeIndex > m_segments.size()) {
        qCWarning(lcDownload) << "播放列表分片数少于续传序号，改为从头下载 resume="
                              << resumeIndex << "count=" << m_segments.size();
        resetTargetFile();
        if (m_done)
            return;
        resumeIndex = 0;
        m_resumeSegmentIndex = 0;
    }
    m_segmentIndex = resumeIndex;
    m_resumeFingerprint = fingerprint;

    // 只有正文确认是播放列表后才改扩展名，避免 text/plain 的普通文本/MP3
    // 先把目标路径改成 .ts 后又失败，留下错误状态。
    if (!m_playlistFileReady) {
        m_playlistFileReady = true;
        if (!switchToHlsFile())
            return;
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
    m_fragmentedMp4 = false;

    QUrl mediaBase = baseUrl;
    bool hasEndList = false;
    QString playlistType;
    QByteArray pendingRange;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-BASE:"))) {
            QString value = line.mid(QStringLiteral("#EXT-X-BASE:").size()).trimmed();
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"'))
                && value.endsWith(QLatin1Char('"'))) {
                value = value.mid(1, value.size() - 2);
            }
            const QUrl candidate = resolveUrl(mediaBase, value);
            if (!isHttpUrl(candidate)) {
                if (error != nullptr)
                    *error = tr("播放列表的基础地址无效");
                return false;
            }
            // #EXT-X-BASE 改变其后所有相对 URI 的解析基准；不更新的话
            // /hls/master.m3u8 下的相对分片会按错误的目录拼地址。
            mediaBase = candidate;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXT-X-ENDLIST"))) {
            hasEndList = true;
            continue;
        }
        if (line.startsWith(QLatin1String("#EXT-X-PLAYLIST-TYPE:"))) {
            playlistType =
                line.mid(QStringLiteral("#EXT-X-PLAYLIST-TYPE:").size()).trimmed().toUpper();
            continue;
        }
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
            const QString uri = uriAttribute(line);
            if (!uri.isEmpty()) {
                const QUrl mapUrl = resolveUrl(mediaBase, uri);
                if (!isHttpUrl(mapUrl)) {
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
                if (m_segments.size() >= NetLimits::kPlaylistSegments) {
                    if (error != nullptr)
                        *error = tr("分片数量超过 %1 上限").arg(NetLimits::kPlaylistSegments);
                    return false;
                }
                // EXT-X-MAP 也是独立请求，不能绕过总分片上限。
                m_fragmentedMp4 = true;
                m_segments.append({mapUrl, mapRange});
            }
            continue;
        }

        if (line.startsWith(QLatin1Char('#')))
            continue;

        const QUrl segmentUrl = resolveUrl(mediaBase, line);
        if (!isHttpUrl(segmentUrl)) {
            if (error != nullptr)
                *error = tr("分片地址无效：%1").arg(line);
            return false;
        }

        Segment segment;
        segment.url = segmentUrl;
        segment.range = pendingRange;
        pendingRange.clear();
        if (segmentUrl.path().endsWith(QLatin1String(".m4s"), Qt::CaseInsensitive)
            || segmentUrl.path().endsWith(QLatin1String(".m4f"), Qt::CaseInsensitive)) {
            m_fragmentedMp4 = true;
        }
        if (m_segments.size() >= NetLimits::kPlaylistSegments) {
            if (error != nullptr)
                *error = tr("分片数量超过 %1 上限").arg(NetLimits::kPlaylistSegments);
            return false;
        }
        m_segments.append(segment);
    }

    if (!hasEndList) {
        // 没有 ENDLIST 有两种可能：静态 VOD 服务器偷懒没写结束标记，
        // 或者直播/EVENT 列表仍在追加。前者由 PLAYLIST-TYPE:VOD 明确声明，
        // 可以安全地按当前分片下载；后者默认拒绝，避免静默产出残片。
        const bool declaredVod = playlistType == QLatin1String("VOD");
        if (!declaredVod && !m_allowIncomplete) {
            m_needsSnapshotConsent = true;
            if (error != nullptr) {
                *error = tr("播放列表没有 #EXT-X-ENDLIST，可能是直播/仍在更新的列表。"
                            "默认不下载；可在下载管理器中确认后按当前分片强制下载。");
            }
            emit snapshotConsentRequired(error != nullptr ? *error : QString());
            m_segments.clear();
            return false;
        }

        m_needsSnapshotConsent = false;
        const QString reason =
            declaredVod
                ? tr("服务器声明 #EXT-X-PLAYLIST-TYPE:VOD 但没有 #EXT-X-ENDLIST，"
                     "按当前分片下载；文件可能不完整。")
                : tr("已按当前分片强制下载；列表没有 #EXT-X-ENDLIST，"
                     "文件可能不完整，且不会跨启动续传。");
        emit snapshotAccepted(reason);
    }
    return true;
}

QString DownloadTask::computePlaylistFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    // 只取路径，不取会随 CDN 签名轮换的 query：同一份切片重新签名不应触发重下；
    // 分片路径/字节范围变化才代表容器结构变了。
    hash.addData(m_fragmentedMp4 ? "fmp4|" : "ts|");
    hash.addData(QByteArray::number(m_segments.size()));
    hash.addData("|");
    for (const Segment &segment : m_segments) {
        QUrl stable = segment.url;
        stable.setQuery(QString());
        stable.setFragment(QString());
        hash.addData(stable.toString(QUrl::FullyEncoded).toUtf8());
        hash.addData("|");
        hash.addData(segment.range);
        hash.addData("|");
    }
    return QString::fromLatin1(hash.result().toHex());
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

    if (m_verifyMediaMagic) {
        fail(tr("下载响应没有可识别的音视频数据"), true);
        return;
    }

    if (m_expectedBytes >= 0 && m_bytesWritten != m_expectedBytes) {
        fail(tr("下载长度不完整：期望 %1 字节，实际 %2 字节")
                 .arg(m_expectedBytes)
                 .arg(m_bytesWritten));
        return;
    }

    // 普通下载此前按秒节流 checkpoint；提交前补最后一段精确进度。
    emit checkpoint(m_bytesWritten, 0, -1);
    commitFile();
}

void DownloadTask::writeReplyData(QNetworkReply *reply)
{
    if (reply == nullptr || m_file == nullptr)
        return;

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return;

    if (m_verifyMediaMagic) {
        // Content-Type 为空/纯文本时，必须在落盘前用文件头确认，
        // 避免把无类型的 HTML 错误页保存成音视频。
        if (!looksLikeMediaPayload(chunk, reply->url())) {
            fail(tr("下载链接返回的内容无法识别为音视频（可能缺少 Content-Type）"), true);
            return;
        }
        m_verifyMediaMagic = false;
    }

    const qint64 remaining = m_maxBytes - m_bytesWritten;
    if (remaining <= 0) {
        fail(tr("文件超过 %1 MB 上限，已停止下载").arg(m_maxBytes / (1024 * 1024)));
        return;
    }
    // 超过上限时写入剩余额度再失败：临时文件至少停在真实上限，而不是
    // 把当前整块 readAll 丢掉、让进度停在上一次检查点。
    const QByteArray payload = chunk.size() > remaining ? chunk.left(int(remaining)) : chunk;
    const qint64 written = m_file->write(payload);
    if (written != payload.size()) {
        fail(tr("写入文件不完整：%1").arg(m_file->errorString()));
        return;
    }
    m_bytesWritten += payload.size();
    m_segmentBytes += payload.size();

    if (m_isHls) {
        // HLS 的续传点按分片边界记录，避免把半截分片写进状态。
        emit progress(m_segmentIndex, m_segments.size());
    } else {
        emit progress(m_bytesWritten, m_expectedBytes);
        // 普通下载每个 readyRead 都写状态会放大磁盘写；最多每秒一次，
        // 退出/完成时再补一次精确检查点。
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastCheckpointMs == 0 || now - m_lastCheckpointMs >= 1000) {
            emit checkpoint(m_bytesWritten, 0, -1);
            m_lastCheckpointMs = now;
        }
    }

    if (chunk.size() > remaining) {
        fail(tr("文件超过 %1 MB 上限，已停止下载").arg(m_maxBytes / (1024 * 1024)));
    }
}

bool DownloadTask::switchToHlsFile()
{
    // fMP4（EXT-X-MAP/m4s）合并出来是 MP4，不是 MPEG-TS；扩展名必须跟随容器。
    const QString desiredSuffix = m_fragmentedMp4 ? QStringLiteral("mp4")
                                                  : QStringLiteral("ts");
    if (m_filePath.endsWith(QLatin1Char('.') + desiredSuffix, Qt::CaseInsensitive)) {
        m_isHls = true;
        return true;
    }

    const int slash = m_filePath.lastIndexOf(QLatin1Char('/'));
    const int dot = m_filePath.lastIndexOf(QLatin1Char('.'));
    QString base = m_filePath;
    if (dot > slash && dot >= 0)
        base = m_filePath.left(dot);
    QString newPath = base + QLatin1Char('.') + desiredSuffix;
    if (m_pathResolver) {
        const QString resolved = m_pathResolver(newPath);
        if (!resolved.isEmpty())
            newPath = resolved;
    }

    const QString oldPartial = partialPath();
    const QString newPartial = newPath + QStringLiteral(".mvpdownload");
    const bool hadBytes = m_bytesWritten > 0;

    if (m_file != nullptr) {
        m_file->close();
        m_file->deleteLater();
        m_file = nullptr;
    }

    if (hadBytes) {
        // 已经下载了分片时不能直接删半成品；把临时文件连同字节数一起改名，
        // 续传序号仍然有效。只有初始探测阶段（0 字节）才允许直接丢弃旧空文件。
        if (!QFile::rename(oldPartial, newPartial)) {
            fail(tr("无法把 HLS 临时文件改名到 %1").arg(newPartial));
            return false;
        }
        m_resumeBytes = m_bytesWritten;
    } else {
        QFile::remove(oldPartial);
    }

    m_filePath = newPath;
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
        // 退出前补一次精确检查点；普通下载的 readyRead 检查点有 1 秒节流。
        emit checkpoint(m_bytesWritten, m_segmentIndex,
                        m_isHls ? m_segments.size() : -1);
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
    // text/plain 不再直接当播放列表；由 resolveAmbiguousTextReply 读正文确认。
    return PlaylistSniffer::metadataIndicatesPlaylist(reply->url(), type);
}

bool DownloadTask::isMediaContentType(const QString &contentType)
{
    const QString type = contentType.toLower().trimmed();
    if (type.isEmpty())
        return true; // 空类型由 writeReplyData 用文件头魔数二次确认
    // `application/mp4; codecs=avc1` 和 `audio/mpeg; charset=binary` 都合法；
    // 容器类型比较只看分号前的 MIME，不能让参数导致整段下载被误拒。
    const QString mime = type.section(QLatin1Char(';'), 0, 0).trimmed();
    if (mime.startsWith(QLatin1String("audio/"))
        || mime.startsWith(QLatin1String("video/"))
        || mime.contains(QLatin1String("octet-stream"))
        || mime.contains(QLatin1String("mpegurl"))
        || mime.contains(QLatin1String("ogg"))
        || mime.contains(QLatin1String("matroska"))
        || mime.contains(QLatin1String("x-flv"))
        || mime.contains(QLatin1String("quicktime"))
        || mime == QLatin1String("application/mp4")
        || mime == QLatin1String("application/x-m4a")
        || mime == QLatin1String("application/x-m4v")
        || mime == QLatin1String("application/vnd.rn-realmedia"))
        return true;
    // 网页、接口 JSON/XML、图片、PDF 都不是音视频，不能落成媒体文件。
    return false;
}

bool DownloadTask::isMediaExtension(const QUrl &url)
{
    static const QStringList suffixes = {
        QStringLiteral("mp3"),  QStringLiteral("mp4"),  QStringLiteral("m4a"),
        QStringLiteral("m4v"),  QStringLiteral("mov"),  QStringLiteral("flv"),
        QStringLiteral("ts"),   QStringLiteral("m4s"),  QStringLiteral("aac"),
        QStringLiteral("ogg"),  QStringLiteral("oga"),  QStringLiteral("opus"),
        QStringLiteral("wav"),  QStringLiteral("flac"), QStringLiteral("webm"),
        QStringLiteral("mkv"),  QStringLiteral("avi"),  QStringLiteral("wmv"),
        QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("m3u8"),
    };
    const QString suffix = QFileInfo(url.path()).suffix().toLower();
    return suffixes.contains(suffix);
}

bool DownloadTask::looksLikeMediaPayload(const QByteArray &data, const QUrl &url)
{
    if (data.isEmpty())
        return false;
    const QByteArray trimmed = data.trimmed();
    if (trimmed.startsWith("#EXTM3U") || trimmed.startsWith("<")
        || trimmed.startsWith("{") || trimmed.startsWith("[")) {
        return false;
    }
    if (data.startsWith("ID3") || data.startsWith("OggS") || data.startsWith("fLaC")
        || data.startsWith("RIFF") || data.startsWith("FLV")
        || data.startsWith("\x1A\x45\xDF\xA3")
        || data.startsWith("\x00\x00\x01\xBA")
        || (data.size() >= 5 && data.mid(4, 4) == "ftyp")
        || (data.size() >= 2 && uchar(data.at(0)) == 0xFF
            && (uchar(data.at(1)) & 0xF0) == 0xF0)
        || data.at(0) == char(0x47)) {
        return true;
    }
    // 有明确媒体后缀时允许未知魔数（部分 MPEG-PS/私有封装没有稳定文件头）；
    // 但没有后缀又无法识别魔数的一律拒绝，避免 HTML 错误页落盘。
    return isMediaExtension(url);
}

bool DownloadTask::parseUnsatisfiedRangeTotal(const QByteArray &value, qint64 *total)
{
    if (total == nullptr)
        return false;
    const QRegularExpressionMatch match =
        QRegularExpression(QStringLiteral("^bytes\\s+\\*/(\\d+)$"),
                           QRegularExpression::CaseInsensitiveOption)
            .match(QString::fromLatin1(value).trimmed());
    if (!match.hasMatch())
        return false;
    bool ok = false;
    const qint64 parsed = match.captured(1).toLongLong(&ok);
    if (!ok || parsed <= 0)
        return false;
    *total = parsed;
    return true;
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
