#ifndef LYRICSCONTROLLER_H
#define LYRICSCONTROLLER_H

#include "lyrics/LrcParser.h"

#include <QObject>
#include <QVector>

class HttpClient;
class LyricsService;
class QListWidget;
class QWidget;
struct MediaItem;

/// 歌词视图所需的最小控件集合，避免控制器反向依赖整个 Ui::MainWindow。
struct LyricsUi
{
    QWidget *panel = nullptr;
    QListWidget *list = nullptr;
};

/// 歌词加载、当前行高亮与滚动、歌词栏显隐。主窗只负责转发播放进度和切歌事件。
class LyricsController : public QObject
{
    Q_OBJECT

public:
    LyricsController(HttpClient *http, const LyricsUi &ui, QObject *parent = nullptr);

    void setCurrentItem(const MediaItem &item);
    void updatePosition(qint64 positionMs);
    void showPanel();
    void hidePanel();
    void clear();
    void refreshHighlight();

private:
    void applyHighlight();

    LyricsUi m_ui;
    LyricsService *m_service;
    QVector<LrcLine> m_lines;
    int m_index = -1;
    qint64 m_lastPosition = 0;
};

#endif // LYRICSCONTROLLER_H
