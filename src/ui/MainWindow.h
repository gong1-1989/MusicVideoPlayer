#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "core/PlaybackController.h"
#include "lyrics/LrcParser.h"
#include "media/MediaItem.h"

#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QStringList>
#include <QVector>

class PlaylistModel;
class MusicSearchService;
class LyricsService;
class HttpClient;
class DownloadTask;
class QListView;
class QCloseEvent;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_btn_search_clicked();
    void on_btn_openMusic_clicked();
    void on_btn_openVideo_clicked();
    void on_btn_prev_clicked();
    void on_btn_play_clicked();
    void on_btn_stop_clicked();
    void on_btn_next_clicked();
    void on_btn_mode_clicked();
    void on_btn_fullscreen_clicked();

private:
    void setupConnections();
    void restoreSettings();
    void saveSettings();

    void addLocalFiles(MediaType type, const QStringList &files);
    void playSearchItem(const MediaItem &item);
    void downloadItem(const MediaItem &item);
    void showSearchContextMenu(const QPoint &pos);

    void toggleFullScreen();
    void updateLyrics(qint64 positionMs);
    void refreshModeButton();
    void ensurePlaylist();

    PlaylistModel *activeModel() const;
    QListView *activeView() const;

    Ui::MainWindow *ui;
    HttpClient *m_http;
    MusicSearchService *m_search;
    LyricsService *m_lyrics;
    PlaybackController *m_player;
    PlaylistModel *m_musicModel;
    PlaylistModel *m_videoModel;
    PlaylistModel *m_searchModel;

    QVector<LrcLine> m_lrcLines;
    int m_lrcIndex = -1;
    bool m_userSeeking = false;
    QString m_lastDir;
    QPointer<DownloadTask> m_download;
};

#endif // MAINWINDOW_H
