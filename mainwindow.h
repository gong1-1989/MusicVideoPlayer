#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QFileDialog>
#include <QMap>
#include <QListWidget>
#include <QTabWidget>
#include <QList>
#include <QByteArray>
#include <QPoint>
#include <QMenu>
#include <QWidget>
#include <QUrl>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QKeyEvent>
#include <QTimer>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum PlayMode{ ListLoop,SingleLoop,Random};
    enum MediaType{ Music, Video};
    struct MediaItem{
        MediaType type;
        QString name;
        QString url;
        QString lrcUrl;
        bool isOnline;
    };
    struct SearchItem{
        QString name;
        QString artist;
        QString url;
        QString lrcUrl;
        MediaType type;
    };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

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
    void on_list_music_itemDoubleClicked(QListWidgetItem *item);
    void on_list_video_itemDoubleClicked(QListWidgetItem *item);
    void onPlayerPositionChanged(qint64 pos);
    void onPlayerDurationChanged(qint64 dur);
    void onPlayerStateChanged(QMediaPlayer::PlaybackState state);
    void onPlayerMediaEnd();
    void onSearchFinished();
    void onDownloadProgress(qint64 recv,qint64 total);
    void onDownloadFinished();
    void onDownloadReadyRead();
    void onLrcLoaded();
    void on_slider_progress_valueChanged(int value);
    void on_slider_volume_valueChanged(int value);

private:
    void playItem(int index,MediaType type);
    void addToMediaList(MediaType type,const QString& name,const QString& url,const QString& lrcUrl,bool isOnline);
    void download(const QString& url,const QString&saveName);
    void loadLrc(const QString& url);
    void parseLrc(const QByteArray& data);
    void updateLrc(qint64 pos);
    void keyPressEvent(QKeyEvent *event) override;
    QString fmtTime(qint64 ms);
    Ui::MainWindow *ui;
    QMediaPlayer *m_player;
    QAudioOutput *m_audioOut;
    QNetworkAccessManager *m_net;
    QNetworkReply *m_searchReply;
    QNetworkReply *m_lrcReply;
    QNetworkReply *m_downloadReply;
    QFile *m_downloadFile;
    QList<MediaItem> m_musicList;
    QList<MediaItem> m_videoList;
    QList<SearchItem> m_searchResults;
    MediaType m_currentType;
    int m_currentIndex;
    PlayMode m_mode;
    QMap<qint64,QString> m_lrcMap;
    qint64 m_lastLrcTime;
};
#endif // MAINWINDOW_H
