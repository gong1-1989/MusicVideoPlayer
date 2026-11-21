#ifndef DOWNLOADTASK_H
#define DOWNLOADTASK_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QString>
#include <QUrl>
#include <QVector>

class HttpClient;
class QNetworkReply;

/// 单个下载任务：QSaveFile 原子落盘，失败/取消不留半截文件。
/// 普通地址按字节流写盘；m3u8 会解析播放列表、顺序拉取分片并合并到同一个文件。
/// 加密 HLS（AES-128/SAMPLE-AES）明确失败，不产出无法播放的半成品。
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

    /// 去掉 Windows 非法字符并补扩展名
    static QString sanitizeFileName(const QString &name, const QString &suffix);
    /// URL 是否看起来是 HLS 播放列表（用于选默认扩展名/提示）
    static bool looksLikeHls(const QUrl &url);

    void cancel();

signals:
    /// 普通下载是字节数；HLS 合并是“已完成分片数 / 总分片数”，UI 统一按百分比显示。
    void progress(qint64 received, qint64 total);
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
    void onHeaders(QNetworkReply *reply);
    void onPlaylistFinished(QNetworkReply *reply);
    void startPlaylistFetch(const QUrl &url, int hop);
    void startNextSegment();
    void onSegmentFinished(QNetworkReply *reply);
    void onDirectFinished(QNetworkReply *reply);
    void writeReplyData(QNetworkReply *reply);
    void commitFile();
    void fail(const QString &message);
    void finish();

    bool parsePlaylist(const QByteArray &body, const QUrl &baseUrl, QUrl *variantUrl,
                       bool *isMaster, QString *error);
    static bool isPlaylistReply(QNetworkReply *reply);
    static bool isMediaContentType(const QString &contentType);
    static bool parseContentRange(const QByteArray &value, qint64 *start, qint64 *end);
    static qint64 expectedRangeBytes(const QByteArray &range);
    bool switchToHlsFile();

    HttpClient *m_http;
    QUrl m_url;
    QUrl m_originUrl;
    QString m_filePath;
    QHash<QString, QString> m_headers;
    QPointer<QNetworkReply> m_reply;
    QPointer<QSaveFile> m_file;
    bool m_done = false;

    qint64 m_bytesWritten = 0;
    qint64 m_expectedBytes = -1;
    qint64 m_segmentBytes = 0;
    qint64 m_segmentExpected = -1;

    bool m_isHls = false;
    QVector<Segment> m_segments;
    int m_segmentIndex = 0;
    int m_playlistHop = 0;
    qint64 m_nextRangeOffset = 0;
};

#endif // DOWNLOADTASK_H
