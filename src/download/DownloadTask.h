#ifndef DOWNLOADTASK_H
#define DOWNLOADTASK_H

#include "net/NetLimits.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVector>

class HttpClient;
class QNetworkReply;

/// 单个下载任务：先写 `.mvpdownload` 临时文件，完成后改名到最终路径。
/// 普通地址支持 HTTP Range 续传；m3u8 会解析播放列表、顺序拉取分片并合并，
/// 重启时从已完成分片继续。加密 HLS（AES-128/SAMPLE-AES）明确失败。
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

    /// 去掉 Windows 非法字符并补扩展名
    static QString sanitizeFileName(const QString &name, const QString &suffix);
    /// URL 是否看起来是 HLS 播放列表（用于选默认扩展名/提示）
    static bool looksLikeHls(const QUrl &url);

    QString targetPath() const { return m_filePath; }
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
    /// 响应实际是 HLS 时，最终扩展名会从 .mp4/.mp3 改成 .ts
    void targetPathChanged(const QString &filePath, bool hls);
    void succeeded(const QString &filePath);
    void failed(const QString &message);
    void finished();

private:
    DownloadTask(HttpClient *http,
                 const QUrl &url,
                 const QString &filePath,
                 const QHash<QString, QString> &headers,
                 QObject *parent);

    struct Segment
    {
        QUrl url;
        QByteArray range;
    };

    void startRequest();
    void startDirectRequest();
    void onHeaders(QNetworkReply *reply);
    void onPlaylistFinished(QNetworkReply *reply);
    void startPlaylistFetch(const QUrl &url, int hop);
    void startNextSegment();
    void onSegmentFinished(QNetworkReply *reply);
    void onDirectFinished(QNetworkReply *reply);
    void writeReplyData(QNetworkReply *reply);
    bool openTargetFile();
    void resetTargetFile();
    void commitFile();
    void fail(const QString &message, bool removePartial = false);
    void finish();

    bool parsePlaylist(const QByteArray &body, const QUrl &baseUrl, QUrl *variantUrl,
                       bool *isMaster, QString *error);
    static bool isPlaylistReply(QNetworkReply *reply);
    static bool isMediaContentType(const QString &contentType);
    static bool parseContentRange(const QByteArray &value, qint64 *start, qint64 *end,
                                  qint64 *total = nullptr);
    static qint64 expectedRangeBytes(const QByteArray &range);
    bool switchToHlsFile();

    HttpClient *m_http;
    QUrl m_url;
    QUrl m_originUrl;
    QString m_filePath;
    QHash<QString, QString> m_headers;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    bool m_done = false;

    qint64 m_bytesWritten = 0;
    qint64 m_expectedBytes = -1;
    qint64 m_segmentBytes = 0;
    qint64 m_segmentExpected = -1;
    qint64 m_maxBytes = NetLimits::kDownloadBytes;
    qint64 m_resumeBytes = 0;

    bool m_isHls = false;
    QVector<Segment> m_segments;
    int m_segmentIndex = 0;
    int m_resumeSegmentIndex = 0;
    int m_playlistHop = 0;
    qint64 m_nextRangeOffset = 0;
};

#endif // DOWNLOADTASK_H
