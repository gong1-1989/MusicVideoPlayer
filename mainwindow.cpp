#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QDebug>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)    
{
    ui->setupUi(this);
    setMinimumSize(1000,700);
    //初始化播放器
    m_player=new QMediaPlayer(this);
    m_audioOut=new QAudioOutput(this);
    m_net=new QNetworkAccessManager(this);
    m_player->setAudioOutput(m_audioOut);
    m_player->setVideoOutput(ui->videoWidget);
    m_searchReply=nullptr;
    m_downloadReply=nullptr;
    m_lrcReply=nullptr;
    m_downloadFile=nullptr;
    m_currentIndex=-1;
    m_currentType=Music;
    m_mode=ListLoop;
    m_lastLrcTime=-1;
    ui->slider_volume->setValue(60);
    m_audioOut->setVolume(0.6);
    ui->videoWidget->setShortcutEnabled(true);
    //绑定播放器信号
    connect(m_player,&QMediaPlayer::positionChanged,this,&MainWindow::onPlayerPositionChanged);
    connect(m_player,&QMediaPlayer::durationChanged,this,&MainWindow::onPlayerDurationChanged);
    connect(m_player,&QMediaPlayer::playbackStateChanged,this,&MainWindow::onPlayerStateChanged);
    connect(m_player,&QMediaPlayer::mediaStatusChanged,this,[this](QMediaPlayer::MediaStatus s){
        if(s==QMediaPlayer::EndOfMedia) onPlayerMediaEnd();
    });
    //搜索列表右键菜单
    ui->list_search->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->list_search,&QWidget::customContextMenuRequested,[this](const QPoint& p){
        QMenu menu;
        menu.addAction("播放",this,[this]{
            int r=ui->list_search->currentRow();
            if(r<0||r>=m_searchResults.size()) return;
            auto& s=m_searchResults[r];
            addToMediaList(s.type,s.name+"-"+s.artist,s.url,s.lrcUrl,true);
            playItem(m_currentType==s.type ? m_currentIndex+1 :0,s.type);
        });
        menu.addAction("加入音乐",this,[this]{
            int r=ui->list_search->currentRow();
            auto& s=m_searchResults[r];
            addToMediaList(Music,s.name+"-"+s.artist,s.url,s.lrcUrl,true);
        });
        menu.addAction("加入视频",this,[this]{
            int r=ui->list_search->currentRow();
            auto& s=m_searchResults[r];
            addToMediaList(Video,s.name+"-"+s.artist,s.url,s.lrcUrl,true);
        });
        menu.addAction("下载",this,[this]{
            int r=ui->list_search->currentRow();
            auto& s=m_searchResults[r];
            download(s.url,s.name+"-"+s.artist+(s.type==Video ? ".mp4" :" .mp3"));
        });
        menu.exec(ui->list_search->mapToGlobal(p));
    });
}

MainWindow::~MainWindow()
{
    delete m_player;
    delete m_audioOut;
    delete m_net;
    delete ui;

}

void MainWindow::keyPressEvent(QKeyEvent *event){
    if(event->key()==Qt::Key_Escape){
        if(ui->videoWidget->isFullScreen()){
            ui->videoWidget->setFullScreen(false);
            ui->videoWidget->setParent(this);
            ui->videoWidget->showNormal();
        }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::on_btn_search_clicked()
{
    ui->list_search->clear();
    m_searchResults.clear();
    QString key=ui->edit_search->text().trimmed();
    if(key.isEmpty()) return;
    ui->label_status->setText("搜索中...");
    QString url=QString("https://music.163.com/api/cloudsearch/pc?s=%1&type=1&offset=0&limit=20")
                      .arg(key.toUtf8().toPercentEncoding());
    QNetworkRequest req((QUrl(url)));
    req.setRawHeader("User_Agent","Mozilla/5.0(Windows NT 10.0; Win64; x64)"
                                   " AppleWebkit/537.36(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    req.setRawHeader("Referer","https://music.163.com");
    req.setRawHeader("Cookie","NMTid=000s5U");
    if(m_searchReply) m_searchReply->abort(),m_searchReply->deleteLater();
    m_searchReply=m_net->get(req);
    m_searchReply->setReadBufferSize(1024*1024*10);
    QTimer::singleShot(15000,m_searchReply,[this]{
        if(m_searchReply && m_searchReply->isRunning()){
            m_searchReply->abort();
            ui->label_status->setText("搜索超时，请重试");
        }
    });
    connect(m_searchReply,&QNetworkReply::finished,this,&MainWindow::onSearchFinished);
}

void MainWindow::on_btn_openMusic_clicked()
{
    auto fs=QFileDialog::getOpenFileNames(this,"音频","","*.mp3 *.wav *.m4a *.flac *.ogg");
    for(auto& f:fs){
        addToMediaList(Music,f.section('/',-1),f,"",false);
    }
}

void MainWindow::on_btn_openVideo_clicked()
{
    auto fs=QFileDialog::getOpenFileNames(this,"视频","","*.mp4 *.mkv *.avi *.mov *.flv *wmv");
    for(auto& f:fs){
        addToMediaList(Video,f.section('/',-1),f,"",false);
    }
}

void MainWindow::on_btn_prev_clicked()
{
    int cur=m_currentType==Music?m_musicList.size():m_videoList.size();
    int idx=(m_currentIndex-1+cur)%cur;
    playItem(idx,m_currentType);
}

void MainWindow::on_btn_play_clicked()
{
    if(m_player->playbackState()==QMediaPlayer::PlayingState)  m_player->pause();
    else m_player->play();
}

void MainWindow::on_btn_stop_clicked()
{
    m_player->stop();
}

void MainWindow::on_btn_next_clicked()
{
    int cnt=m_currentType==Music ? m_musicList.size() :m_videoList.size();
    if(cnt==0) return;
    int idx;
    if(m_mode==Random) idx=QRandomGenerator::global()->bounded(cnt);
    else idx=(m_currentIndex+1)%cnt;
    playItem(idx,m_currentType);
}

void MainWindow::on_btn_mode_clicked()
{
    if(m_mode==ListLoop){
        m_mode=SingleLoop;
        ui->btn_mode->setText("单曲循环");
    }else if(m_mode==SingleLoop){
        m_mode=Random;
        ui->btn_mode->setText("随机");
    }else{
        m_mode=ListLoop;
        ui->btn_mode->setText("列表循环");
    }
}

void MainWindow::on_btn_fullscreen_clicked()
{
    if(ui->videoWidget->isFullScreen()){
        ui->videoWidget->setFullScreen(false);
        ui->videoWidget->setParent(this);
        ui->videoWidget->showNormal();
    }else{
        ui->videoWidget->setParent(nullptr);
        ui->videoWidget->setFullScreen(true);
    }
}

void MainWindow::on_list_music_itemDoubleClicked(QListWidgetItem *item)
{
    playItem(ui->list_music->currentRow(),Music);
}

void MainWindow::on_list_video_itemDoubleClicked(QListWidgetItem *item)
{
    playItem(ui->list_video->currentRow(),Video);
}

void MainWindow::onPlayerPositionChanged(qint64 pos){
    ui->slider_progress->setValue(pos);
    ui->label_time->setText(fmtTime(pos)+" / "+fmtTime(m_player->duration()));
    updateLrc(pos);
}

void MainWindow::onPlayerDurationChanged(qint64 dur){
    ui->slider_progress->setRange(0,dur);
}

void MainWindow::onPlayerStateChanged(QMediaPlayer::PlaybackState state){
    ui->btn_play->setText(state==QMediaPlayer::PlayingState ? "暂停" : "播放");
}

void MainWindow::onPlayerMediaEnd(){
    if(m_mode==SingleLoop) m_player->setPosition(0),m_player->play();
    else on_btn_mode_clicked();
}

void MainWindow::onSearchFinished(){
    if(m_searchReply->error()!=QNetworkReply::NoError){
        ui->label_status->setText("搜索失败："+m_searchReply->errorString());
        m_searchReply->deleteLater();
        m_searchReply=nullptr;
        return;
    }
    QByteArray data=m_searchReply->readAll();
    m_searchReply->deleteLater();
    m_searchReply=nullptr;
    QJsonDocument doc=QJsonDocument::fromJson(data);
    if(!doc.isObject()){
        ui->label_status->setText("解析失败");
        return;
    }
    QJsonObject root=doc.object();
    QJsonObject result=root["result"].toObject();
    QJsonArray songs=result["songs"].toArray();
    if(songs.isEmpty()){
        ui->label_status->setText("未搜索到结果");
        return;
    }
    for(int i=0;i<songs.size();++i){
        QJsonValue v=songs.at(i);
        if(!v.isObject()) continue;
        auto o=v.toObject();
        SearchItem s;
        s.name=o["name"].toString();
        QJsonArray artists=o["ar"].toArray();
        if(!artists.isEmpty()){
            s.artist=artists[0].toObject()["name"].toString();
        }
        qint64 songId=o["id"].toVariant().toLongLong();
        s.url=QString("https//music.163.com/song/media/outer/url?id=%1.mp3").arg(songId);
        s.lrcUrl=QString("https//music.163.com/api/song/lyric?lv=-1&kv=-1&tv=-1&id=%1").arg(songId);
        s.type=s.url.contains("mp4") ? Video : Music;

        if(!s.url.isEmpty()){
            m_searchResults.append(s);
            ui->list_search->addItem(s.name+"-"+s.artist);
        }
    }
    ui->label_status->setText(QString("搜索完成，共%1首").arg(m_searchResults.size()));
}

void MainWindow::onDownloadProgress(qint64 recv,qint64 total){
    ui->label_status->setText(QString("下载%1%").arg(100*recv/total));
}

void MainWindow::onDownloadFinished(){
    m_downloadFile->close();
    ui->label_status->setText("下载完成");
}

void MainWindow::onDownloadReadyRead(){
    m_downloadFile->write(m_downloadReply->readAll());
}

void MainWindow::onLrcLoaded(){
    parseLrc(m_lrcReply->readAll());
}

void MainWindow::playItem(int index,MediaType type){
    m_currentType=type;
    m_currentIndex=index;
    MediaItem& item=type==Music ? m_musicList[index] :m_videoList[index];
    if(item.isOnline) {
        m_player->setSource(QUrl(item.url));
    }
    else m_player->setSource(QUrl::fromLocalFile(item.url));
    loadLrc(item.lrcUrl);
    m_player->play();
    ui->label_status->setText("正在播放："+item.name);
}

void MainWindow::addToMediaList(MediaType type,const QString& name,
                                const QString& url,const QString& lrcUrl,bool isOnline){
    MediaItem item{type,name,url,lrcUrl,isOnline};
    if(type==Music){
        for(auto it=m_musicList.begin();it!=m_musicList.end();it++){
            if(it->name==name) return;
        }
        m_musicList.append(item);
        ui->list_music->addItem(name);
    }else{
        for(auto it=m_videoList.begin();it!=m_videoList.end();it++){
            if(it->name==name) return;
        }
        m_videoList.append(item);
        ui->list_video->addItem(name);
    }
}

void MainWindow::download(const QString& url,const QString&saveName){
    QString path=QFileDialog::getSaveFileName(this,"保存",saveName);
    if(path.isEmpty()) return;
    m_downloadFile=new QFile(path);
    m_downloadFile->open(QIODevice::WriteOnly);
    m_downloadReply=m_net->get(QNetworkRequest(url));
    connect(m_downloadReply,&QNetworkReply::downloadProgress,this,&MainWindow::onDownloadProgress);
    connect(m_downloadReply,&QNetworkReply::readyRead,this,&MainWindow::onDownloadReadyRead);
    connect(m_downloadReply,&QNetworkReply::finished,this,&MainWindow::onDownloadFinished);
}

void MainWindow::loadLrc(const QString& url){
    m_lrcMap.clear();
    ui->list_lrc->clear();
    m_lastLrcTime=-1;
    if(url.isEmpty()) return;
    if(m_lrcReply) m_lrcReply->abort();
    m_lrcReply=m_net->get(QNetworkRequest(url));
    connect(m_lrcReply,&QNetworkReply::finished,this,&MainWindow::onLrcLoaded);
}

void MainWindow::parseLrc(const QByteArray& data){
    QString txt=QString::fromUtf8(data);
    QRegularExpression rx(R"(\[(\d+):(\d+)\.(\d+)])");////??
    for(auto line:txt.split('\n')){
        auto m=rx.match(line);
        if(!m.hasMatch()) continue;
        qint64 t=m.captured(1).toInt()*60000+m.captured(2).toInt()*1000+m.captured(3).toInt();
        m_lrcMap.insert(t,m.captured(4).trimmed());
    }
}

void MainWindow::updateLrc(qint64 pos){
    qint64 t=-1;
    for(auto k:m_lrcMap.keys()) if(k<=pos) t=k;
    if(t==-1||t==m_lastLrcTime) return;
    m_lastLrcTime=t;
    ui->list_lrc->clear();
    int cur=0,i=0;
    for(auto it=m_lrcMap.begin();it!=m_lrcMap.end();it++,i++){
        auto* item=new QListWidgetItem(it.value());
        item->setTextAlignment(Qt::AlignCenter);
        ui->list_lrc->addItem(item);
        if(it.key()==t) cur=i;
    }
    if(ui->list_lrc->item(cur)){
        ui->list_lrc->item(cur)->setForeground(Qt::cyan);
        ui->list_lrc->scrollToItem(ui->list_lrc->item(cur));
    }
}

QString MainWindow::fmtTime(qint64 ms){
    qint64 s=ms/1000;
    return QString("%1:%2").arg(s/60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'));
}

void MainWindow::on_slider_progress_valueChanged(int value)
{
    if(qAbs(m_player->position()-value)>100) m_player->setPosition(value);
}

void MainWindow::on_slider_volume_valueChanged(int value)
{
    double v=value/100.0;
    m_audioOut->setVolume(v);
}

