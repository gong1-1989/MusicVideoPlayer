#ifndef DOWNLOADTASK_H
#define DOWNLOADTASK_H

#include "net/NetLimits.h"
#include "stream/HlsPlaylist.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>

class HttpClient;
class QNetworkReply;

/// 单个下载任务：先写 `.mvpdownload` 临时文件，完成后改名到最终路径。
/// 普通地址支持 HTTP Range 续传；m3u8 会解析播放列表、顺序拉取分片并合并，
/// 重启时从已完成分片继续。AES-128 HLS 会自动抓取 KEY URI 并解密后落盘；
/// SAMPLE-AES 等其它加密方式仍明确失败，避免把密文写成损坏文件。
class DownloadTask : public QObject
{
    Q_OBJECT

public:
    static DownloadTask *start(HttpClient *http,
                               const QUrl &url,
                               const QString &filePath,
                               QObject *parent = nullptr);
    /// 带源请求头（Referer/Cookie）的下载，部分源缺头会直接 403
    static DownloadTask *start(HttpClient *http,
                               const QUrl &url,
                               const QString &filePath,
                               const QHash<QString, QString> &headers,
                               QObject *parent = nullptr);

    /// 在事件循环开始前设置续传点；bytesWritten 必须是完整前缀的字节数，
    /// segmentIndex 是 HLS 已完成分片数。
    void setResumeState(qint64 bytesWritten, int segmentIndex, bool hls = false);
    /// HLS 续传必须绑定上一次解析出的分片结构；空指纹表示旧状态，按不可续处理。
    void setResumeFingerprint(const QString &fingerprint);

    /// 源站地址只用于裁剪 Cookie/Authorization 的作用域，不是流地址本身。
    void setOriginUrl(const QUrl &origin);
    /// HLS 分片请求的本地化映射。播放代理已经缓存同一分片时，后台整段缓存
    /// 通过该回调把真实分片地址换成代理地址，让两条链路共用一次上游下载；
    /// 分片索引与续传指纹仍记录原始地址，避免代理 token 变化破坏续传。
    using SegmentUrlMapper = std::function<QUrl(const QUrl &)>;
    void setHlsSegmentUrlMapper(SegmentUrlMapper mapper);
    /// 主列表的 SESSION-KEY 需要传递给只拿到子列表的缓存任务；line 是原始
    /// `#EXT-X-SESSION-KEY`，baseUrl 是主列表最终地址，用于解析相对密钥地址。
    void setHlsSessionKey(const QString &line, const QUrl &baseUrl);
    /// 无 #EXT-X-ENDLIST 时是否允许按当前分片下载。默认只放行服务器显式声明
    /// PLAYLIST-TYPE:VOD 的列表；这里用于用户确认后的“强制下载当前分片”。
    void setAllowIncompletePlaylist(bool allow);
    /// 目标路径冲突时由下载管理器重新分配，绝不覆盖已有文件。
    void setPathResolver(std::function<QString(const QString &)> resolver);

    /// 去掉 Windows 非法字符并补扩展名
    static QString sanitizeFileName(const QString &name, const QString &suffix);
    /// URL 是否看起来是 HLS 播放列表（用于选默认扩展名/提示）
    static bool looksLikeHls(const QUrl &url);

    QString partialPath() const;
    bool isHls() const { return m_isHls; }

    /// 退出程序：中止请求但保留临时文件，下次启动续传，不当作失败
    void stopForExit();
    /// 用户移除任务：中止并删除临时文件
    void cancel();

    /// 单个文件允许落盘的最大字节数；默认 NetLimits::kDownloadBytes，
    /// 由调用方按设置页里的“单文件下载上限”覆盖。
    void setMaxBytes(qint64 bytes);

signals:
    /// 普通下载是总字节数；HLS 合并是“已完成分片数 / 总分片数”
    void progress(qint64 received, qint64 total);
    /// 每完成一个 HLS 分片或普通下载写入一段后更新；供下载管理器持久化续传点
    void checkpoint(qint64 bytesWritten, int segmentIndex, int segmentCount);
    /// HLS 分片已完整写入合并轨道文件；url/range 定位上游分片，offset/size
    /// 是合并文件里的切片，供播放代理直接读缓存。
    void segmentWritten(const QUrl &url, const QByteArray &range, qint64 offset, qint64 size);
    /// 成功解析媒体播放列表后发出结构指纹；续传前用它判断分片是否仍属于同一容器。
    void playlistFingerprint(const QString &fingerprint);
    /// HLS 续传点被清零（指纹变化/文件长度不足），旧分片索引必须同步丢弃。
    void targetResetted();
    /// 响应实际是 HLS 时，最终扩展名会从 .mp4/.mp3 改成 .ts
    void targetPathChanged(const QString &filePath, bool hls);
    /// 列表没有 ENDLIST 但已按 VOD 声明或用户授权继续下载；结果只是快照。
    void snapshotAccepted(const QString &reason);
    /// 列表没有 ENDLIST 且未声明 VOD，默认拒绝；界面可让用户确认后强制下载。
    void snapshotConsentRequired(const QString &reason);
    void succeeded(const QString &filePath);
    void failed(const QString &message);
    void finished();

private:
    /// HLS 分段会话：集中保存播放列表结构、密钥缓存和分片推进状态。
    /// 网络请求、文件写入与信号仍由 DownloadTask 统一负责。
    class HlsDownloadSession
    {
    public:
        /// KEY 字段与播放链路共用同一套解析结果，避免 METHOD/KEYFORMAT/IV
        /// 的兼容判断在下载和缓存两侧各写一份。
        using Key = HlsPlaylist::Key;

        struct Segment
        {
            QUrl url;
            QByteArray range;
            Key key;
        };

        enum class Snapshot
        {
            None,
            Accepted,
            ConsentRequired
        };

        struct ParseResult
        {
            bool ok = false;
            bool isMaster = false;
            QUrl variantUrl;
            QString error;
            Snapshot snapshot = Snapshot::None;
            QString snapshotReason;
        };

        /// 应用主列表带下来的 SESSION-KEY；解析失败按不支持记录。
        void applySessionKeyLine(const QString &line, const QUrl &baseUrl);
        ParseResult parsePlaylist(const QByteArray &body, const QUrl &baseUrl,
                                  bool allowIncomplete);
        QString fingerprint() const;
        void clearKeyMaterial();

        Key sessionKey;
        bool fragmentedMp4 = false;
        QVector<Segment> segments;
        int segmentIndex = 0;
        int resumeSegmentIndex = 0;
        bool playlistFileReady = false;
        int playlistHop = 0;
        QHash<QString, QByteArray> keyCache;
        QByteArray currentKey;
        Segment currentSegment;
        QByteArray segmentCipher;
        qint64 segmentBytes = 0;
        qint64 segmentExpected = -1;
        qint64 segmentFileOffset = 0;
        /// 当前是否已有分片请求正在写盘。退出续传只能记录到上一完整分片，
        /// 否则会把半片字节和“已完成分片数”一起持久化，重启后重复写入该片前缀。
        bool segmentInProgress = false;
    };

    DownloadTask(HttpClient *http,
                 const QUrl &url,
                 const QString &filePath,
                 const QHash<QString, QString> &headers,
                 QObject *parent);

    void startRequest();
    void startDirectRequest();
    void onHeaders(QNetworkReply *reply);
    void beginDirectMediaReply(QNetworkReply *reply, const QString &contentType);
    void beginPlaylistReply(QNetworkReply *reply);
    void resolveAmbiguousTextReply(QNetworkReply *reply, const QString &contentType);
    void onPlaylistFinished(QNetworkReply *reply);
    void startPlaylistFetch(const QUrl &url, int hop);
    void startNextSegment();
    void startSegmentRequest(const HlsDownloadSession::Segment &segment);
    void onSegmentFinished(QNetworkReply *reply);
    void onDirectFinished(QNetworkReply *reply);
    void writeReplyData(QNetworkReply *reply);
    bool writeDecryptedSegment(const HlsDownloadSession::Segment &segment);
    void fetchSegmentKey(const HlsDownloadSession::Segment &segment,
                         const std::function<void()> &ready);
    void onSegmentKeyFinished(QNetworkReply *reply,
                              const HlsDownloadSession::Segment &segment,
                              const QString &cacheKey, const std::function<void()> &ready);
    void clearKeyMaterial();
    bool openTargetFile();
    void resetTargetFile();
    /// 续传响应与本地进度不一致时清空临时文件并重试一次无 Range 请求。
    /// 只在第一次不一致时返回 true；再次失败交给调用方报错，避免无限重试。
    bool restartFromZeroAfterRangeMismatch(const QString &reason);
    void commitFile();
    void fail(const QString &message, bool removePartial = false);
    void finish();

    bool parsePlaylist(const QByteArray &body, const QUrl &baseUrl, QUrl *variantUrl,
                       bool *isMaster, QString *error);
    QString computePlaylistFingerprint() const;
    static bool isPlaylistReply(QNetworkReply *reply);
    static bool isMediaContentType(const QString &contentType);
    static bool looksLikeMediaPayload(const QByteArray &data, const QUrl &url);
    static bool isMediaExtension(const QUrl &url);
    static bool parseContentRange(const QByteArray &value, qint64 *start, qint64 *end,
                                  qint64 *total = nullptr);
    static bool parseUnsatisfiedRangeTotal(const QByteArray &value, qint64 *total);
    static qint64 expectedRangeBytes(const QByteArray &range);
    bool switchToHlsFile();

    HttpClient *m_http;
    SegmentUrlMapper m_segmentUrlMapper;
    QUrl m_url;
    QUrl m_originUrl;
    std::function<QString(const QString &)> m_pathResolver;
    QString m_filePath;
    QHash<QString, QString> m_headers;
    QPointer<QNetworkReply> m_reply;
    QPointer<QNetworkReply> m_keyReply;
    QFile *m_file = nullptr;
    bool m_done = false;

    qint64 m_bytesWritten = 0;
    qint64 m_expectedBytes = -1;
    /// 本次响应新写入的字节数；续传时 m_bytesWritten 含旧文件，不能用它判断
    /// “服务器这次是否真的返回了内容”。
    qint64 m_sessionBytesWritten = 0;
    qint64 m_maxBytes = NetLimits::kDownloadBytes;
    qint64 m_resumeBytes = 0;
    qint64 m_lastCheckpointMs = 0;

    /// HLS 播放列表、密钥与分片推进状态统一收在这里。
    HlsDownloadSession m_hls;

    bool m_isHls = false;
    bool m_ambiguousTextResolved = false;
    bool m_verifyMediaMagic = false;
    bool m_allowIncomplete = false;
    /// 本次任务是否已经从“续传响应不一致”回退到从头下载。
    bool m_restartedFromZero = false;
    QString m_resumeFingerprint;
};

#endif // DOWNLOADTASK_H
