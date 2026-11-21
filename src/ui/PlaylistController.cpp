#include "ui/PlaylistController.h"

#include "core/PlaybackController.h"
#include "core/PlaylistModel.h"
#include "core/PlaylistStore.h"
#include "settings/AppSettings.h"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QKeySequence>
#include <QListView>
#include <QLoggingCategory>
#include <QMenu>
#include <QModelIndex>
#include <QPoint>
#include <QShortcut>
#include <QTabWidget>
#include <QVector>
#include <QWidget>

Q_LOGGING_CATEGORY(lcPlaylist, "mvp.playlist")

namespace {

/// 旧版 QSettings 里的播放列表是 JSON 数组，迁移时逐条校验
QVector<MediaItem> playlistFromJson(const QByteArray &payload)
{
    QVector<MediaItem> items;
    if (payload.isEmpty())
        return items;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray())
        return items;

    const QJsonArray array = doc.array();
    items.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const MediaItem item = mediaItemFromJson(value.toObject());
        if (item.isValid())
            items.append(item);
    }
    return items;
}

} // namespace

PlaylistController::PlaylistController(PlaybackController *player, const PlaylistUi &ui,
                                       QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_ui(ui)
    , m_musicModel(new PlaylistModel(this))
    , m_videoModel(new PlaylistModel(this))
{
    Q_ASSERT(m_player != nullptr);
    Q_ASSERT(m_ui.musicView != nullptr);
    Q_ASSERT(m_ui.videoView != nullptr);
    Q_ASSERT(m_ui.tabs != nullptr);

    m_ui.musicView->setModel(m_musicModel);
    m_ui.videoView->setModel(m_videoModel);
    m_musicTabTitle = m_ui.tabs->tabText(0);
    m_videoTabTitle = m_ui.tabs->tabText(1);

    bindView(m_ui.musicView, m_musicModel, MediaType::Music);
    bindView(m_ui.videoView, m_videoModel, MediaType::Video);
}

PlaylistModel *PlaylistController::modelFor(MediaType type) const
{
    return type == MediaType::Video ? m_videoModel : m_musicModel;
}

PlaylistModel *PlaylistController::activeModel() const
{
    return m_ui.tabs->currentIndex() == 1 ? m_videoModel : m_musicModel;
}

QListView *PlaylistController::activeView() const
{
    return m_ui.tabs->currentIndex() == 1 ? m_ui.videoView : m_ui.musicView;
}

void PlaylistController::restore()
{
    QVector<MediaItem> music = PlaylistStore::load(MediaType::Music);
    QVector<MediaItem> video = PlaylistStore::load(MediaType::Video);

    // 只迁移一次：旧版 QSettings 里的列表写入 Data/playlists.json 后清键。
    // 迁移标记防止“用户清空列表”后下次启动又被旧键复活。
    if (!AppSettings::playlistsMigrated()) {
        bool canMarkMigrated = true;
        bool imported = false;
        if (music.isEmpty()) {
            const QVector<MediaItem> legacyMusic =
                playlistFromJson(AppSettings::playlist(MediaType::Music));
            if (!legacyMusic.isEmpty()) {
                music = legacyMusic;
                imported = true;
            }
        }
        if (video.isEmpty()) {
            const QVector<MediaItem> legacyVideo =
                playlistFromJson(AppSettings::playlist(MediaType::Video));
            if (!legacyVideo.isEmpty()) {
                video = legacyVideo;
                imported = true;
            }
        }
        if (imported) {
            canMarkMigrated = PlaylistStore::saveAll(music, video);
            if (canMarkMigrated)
                qCInfo(lcPlaylist) << "已把旧 QSettings 播放列表迁移到 Data/playlists.json";
            else
                qCWarning(lcPlaylist) << "播放列表迁移失败，保留旧 QSettings 数据待下次重试";
        }
        if (canMarkMigrated) {
            AppSettings::setPlaylistsMigrated(true);
            AppSettings::clearLegacyPlaylists();
        }
    }

    // 提示数量必须和模型实际显示的一致；旧列表里的重复条目先按同一规则去掉。
    music = dedupeMediaItems(music);
    video = dedupeMediaItems(video);
    m_musicModel->setItems(music);
    m_videoModel->setItems(video);

    if (!music.isEmpty() || !video.isEmpty()) {
        emit statusMessage(tr("已恢复上次的播放列表：音乐 %1 首，视频 %2 个")
                               .arg(music.size())
                               .arg(video.size()));
    }
}

bool PlaylistController::save()
{
    if (PlaylistStore::saveAll(m_musicModel->items(), m_videoModel->items())) {
        setDirty(false);
        return true;
    }

    qCWarning(lcPlaylist) << "播放列表保存失败";
    // 不丢用户刚做的修改：界面保留，页签标 *，右键菜单提供重试。
    setDirty(true);
    emit statusMessage(tr("播放列表保存失败：主/备用存储都未能写入（可能是磁盘空间不足或目录不可写）；修改仍保留，可在列表右键“重试保存”"));
    return false;
}

bool PlaylistController::retrySave()
{
    if (!m_dirty)
        return true;
    if (!PlaylistStore::saveAll(m_musicModel->items(), m_videoModel->items())) {
        qCWarning(lcPlaylist) << "播放列表重试保存仍失败";
        emit statusMessage(tr("播放列表仍未保存：主/备用存储都未能写入，修改继续保留"));
        return false;
    }
    setDirty(false);
    emit statusMessage(tr("播放列表已重新保存"));
    return true;
}

void PlaylistController::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    refreshTabTitles();
}

void PlaylistController::refreshTabTitles()
{
    const QString suffix = m_dirty ? tr(" *") : QString();
    m_ui.tabs->setTabText(0, m_musicTabTitle + suffix);
    m_ui.tabs->setTabText(1, m_videoTabTitle + suffix);
    const QString tip = m_dirty ? tr("有修改尚未保存，右键列表可重试保存") : QString();
    m_ui.tabs->setTabToolTip(0, tip);
    m_ui.tabs->setTabToolTip(1, tip);
}

void PlaylistController::ensurePlaylist()
{
    if (m_player->playlist() == nullptr) {
        m_player->setPlaylist(activeModel());
        return;
    }
    if (!m_player->hasCurrentItem() && m_player->playlist() != activeModel())
        m_player->setPlaylist(activeModel());
}

void PlaylistController::playItem(const MediaItem &item)
{
    if (!item.isValid())
        return;

    PlaylistModel *model = modelFor(item.type);
    const bool added = model->addItem(item); // 已存在时返回 false，不影响下面的定位
    const int row = model->indexOf(item);
    if (row < 0)
        return;

    if (added)
        save();

    m_player->setPlaylist(model);
    m_player->playRow(row);
    m_ui.tabs->setCurrentIndex(item.type == MediaType::Video ? 1 : 0);
}

void PlaylistController::addSearchItem(const MediaItem &item, MediaType type)
{
    MediaItem copy = item;
    copy.type = type;
    PlaylistModel *model = modelFor(type);
    if (!model->addItem(copy)) {
        emit statusMessage(type == MediaType::Music ? tr("音乐列表中已存在")
                                                    : tr("视频列表中已存在"));
        return;
    }
    if (save())
        emit statusMessage(type == MediaType::Music ? tr("已加入音乐列表")
                                                    : tr("已加入视频列表"));
}

void PlaylistController::addLocalFiles(MediaType type, const QStringList &files)
{
    PlaylistModel *model = modelFor(type);
    int added = 0;

    for (const QString &path : files) {
        const QFileInfo info(path);
        MediaItem item;
        item.type = type;
        // 列表标题不显示扩展名，避免 “xxx.mp3” 与在线条目的观感不一致
        item.title = info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName();
        item.url = QUrl::fromLocalFile(info.absoluteFilePath());
        item.online = false;

        // 同名 .lrc 作为本地歌词
        const QString lyricPath = info.absolutePath() + QLatin1Char('/')
                                + info.completeBaseName() + QStringLiteral(".lrc");
        if (QFileInfo::exists(lyricPath))
            item.lyricUrl = QUrl::fromLocalFile(lyricPath);

        if (model->addItem(item))
            ++added;
    }

    if (added > 0) {
        if (save())
            emit statusMessage(tr("已添加 %1 个文件").arg(added));
    } else {
        emit statusMessage(tr("列表中已存在相同文件"));
    }
    qCInfo(lcPlaylist) << "加载本地文件" << (type == MediaType::Music ? "音乐" : "视频") << "共"
                       << files.size() << "个，新增" << added << "个";
}

void PlaylistController::chooseLocalFiles(MediaType type)
{
    QWidget *parent = m_ui.tabs != nullptr ? m_ui.tabs->window() : nullptr;
    const QString filter = type == MediaType::Music
                               ? tr("音乐文件 (*.mp3 *.flac *.wav *.m4a *.aac *.ogg);;所有文件 (*)")
                               : tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv);;所有文件 (*)");
    const QStringList files = QFileDialog::getOpenFileNames(
        parent, type == MediaType::Music ? tr("选择音乐文件") : tr("选择视频文件"),
        AppSettings::lastDir(), filter);
    if (files.isEmpty())
        return;

    AppSettings::setLastDir(QFileInfo(files.first()).absolutePath());
    addLocalFiles(type, files);
}

void PlaylistController::bindView(QListView *view, PlaylistModel *model, MediaType type)
{
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view, model, type](const QPoint &pos) {
                showContextMenu(type, view, model, pos);
            });

    // 选中一行后按 Delete 直接移除
    auto *shortcut = new QShortcut(QKeySequence::Delete, view);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, [this, view, type] {
        const QModelIndex index = view->currentIndex();
        if (index.isValid())
            removeRow(type, index.row());
    });
}

void PlaylistController::removeRow(MediaType type, int row)
{
    PlaylistModel *model = modelFor(type);
    // 越界 row 不能先取 item，否则公开入口会把默认空条目当真实名字。
    if (row < 0 || row >= model->rowCount())
        return;

    const QString name = model->itemAt(row).displayName();
    const bool removingCurrent = m_player->playlist() == model && m_player->currentRow() == row;

    if (!model->removeRow(row))
        return;

    if (m_player->playlist() == model)
        m_player->onPlaylistRowRemoved(row);

    if (removingCurrent)
        emit playbackInvalidated();

    if (save()) {
        emit statusMessage(removingCurrent ? tr("已移除当前播放项：%1").arg(name)
                                           : tr("已从列表移除：%1").arg(name));
    }
}

void PlaylistController::clearPlaylist(MediaType type)
{
    PlaylistModel *model = modelFor(type);
    if (model->isEmpty())
        return;

    if (m_player->playlist() == model) {
        m_player->stop();
        m_player->setPlaylist(nullptr);
        emit playbackInvalidated();
    }

    model->clear();
    if (save())
        emit statusMessage(tr("已清空列表"));
}

void PlaylistController::showContextMenu(MediaType type, QListView *view, PlaylistModel *model,
                                         const QPoint &pos)
{
    const QModelIndex index = view->indexAt(pos);

    QMenu menu(view);
    if (m_dirty) {
        // 保存失败后修改还在，这里给一个明确的恢复入口，而不是让用户重做操作。
        QAction *retryAction = menu.addAction(tr("重试保存播放列表（未保存）"));
        connect(retryAction, &QAction::triggered, this, &PlaylistController::retrySave);
        menu.addSeparator();
    }
    QAction *playAction = nullptr;
    QAction *removeAction = nullptr;
    if (index.isValid()) {
        playAction = menu.addAction(tr("播放"));
        removeAction = menu.addAction(tr("从列表移除"));
        menu.addSeparator();
    }
    QAction *clearAction = menu.addAction(tr("清空列表"));
    clearAction->setEnabled(!model->isEmpty());
    menu.addSeparator();

    // 菜单跟着当前列表走：音乐列表只给音乐入口，视频列表只给视频入口
    QAction *addLocalAction = menu.addAction(type == MediaType::Music ? tr("加载本地音乐…")
                                                                     : tr("加载本地视频…"));

    QAction *chosen = menu.exec(view->viewport()->mapToGlobal(pos));
    if (chosen == nullptr)
        return;

    if (chosen == playAction) {
        playRow(type, index.row());
    } else if (chosen == removeAction) {
        removeRow(type, index.row());
    } else if (chosen == clearAction) {
        clearPlaylist(type);
    } else if (chosen == addLocalAction) {
        chooseLocalFiles(type);
    }
}

void PlaylistController::playRow(MediaType type, int row)
{
    m_player->setPlaylist(modelFor(type));
    m_player->playRow(row);
}
