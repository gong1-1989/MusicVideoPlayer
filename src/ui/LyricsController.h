#ifndef LYRICSCONTROLLER_H
#define LYRICSCONTROLLER_H

#include "lyrics/LrcParser.h"

#include <QObject>
#include <QStringList>
#include <QVector>

class HttpClient;
class LyricsService;
struct MediaItem;

/// 歌词加载与当前行定位。控制器只产出纯数据信号，
/// 由主窗把歌词交给音乐视觉层绘制，不再依赖任何列表面板控件。
class LyricsController : public QObject
{
    Q_OBJECT

public:
    explicit LyricsController(HttpClient *http, QObject *parent = nullptr);

    void setCurrentItem(const MediaItem &item);
    void updatePosition(qint64 positionMs);
    void clear();
    void refreshHighlight();

signals:
    void linesLoaded(const QStringList &lines);
    void currentIndexChanged(int index);
    void cleared();

private:
    LyricsService *m_service;
    QVector<LrcLine> m_lines;
    int m_index = -1;
    qint64 m_lastPosition = 0;
};

#endif // LYRICSCONTROLLER_H
