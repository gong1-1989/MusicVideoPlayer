#ifndef LYRICSSERVICE_H
#define LYRICSSERVICE_H

#include "lyrics/LrcParser.h"

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVector>

class HttpClient;
class QNetworkReply;

/// 歌词获取：在线接口返回的是 JSON 包裹的 LRC 文本，这里统一拆包后交给 LrcParser；
/// 本地文件直接按字节解析。所有回调都用 sender() 校验，避免切歌时串数据。
class LyricsService : public QObject
{
    Q_OBJECT

public:
    explicit LyricsService(HttpClient *http, QObject *parent = nullptr);

    void load(const QUrl &url);
    void loadLocalFile(const QString &filePath);
    void cancel();

signals:
    void lyricsReady(const QVector<LrcLine> &lines);
    void lyricsFailed(const QString &message);

private:
    void onFinished(int generation);

    HttpClient *m_http;
    QPointer<QNetworkReply> m_reply;
    int m_generation = 0;
};

#endif // LYRICSSERVICE_H
