#include "ui/PlaylistController.h"

#include "cache/MediaCache.h"
#include "cache/LyricsCache.h"
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
#include <QMessageBox>
#include <QModelIndex>
#include <QPoint>
#include <QShortcut>
#include <QTabWidget>
#include <QTimer>
#include <QVector>

#include <utility>
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

/// 试听条目只活在当前模型里。无论从哪个保存入口触发，落盘前都统一剔除，
/// 避免“点开听一下”在关闭程序或下一次增删时被顺手写成永久列表。
QVector<MediaItem> persistentItems(const QVector<MediaItem> &items)
{
    QVector<MediaItem> result;
    result.reserve(items.size());
    for (const MediaItem &item : items) {
        if (!item.previewOnly)
            result.append(item);
    }
    return result;
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

    // 视频进度已经写进列表条目；停止/切歌/退出会触发，但连续切歌时
    // 没必要每首都整份落盘，合并到 300ms 后保存一次。退出路径仍同步 save()。
    connect(m_player, &PlaybackController::currentItemPositionUpdated, this, [this](int) {
        if (m_player->playlist() == m_musicModel || m_player->playlist() == m_videoModel)
            scheduleSave();
    });
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
    const PlaylistStore::LoadResult loadResult = PlaylistStore::loadAll();
    QVector<MediaItem> music = loadResult.music;
    QVector<MediaItem> video = loadResult.video;
    m_storageBlocked = loadResult.storageBlocked;

    if (m_storageBlocked) {
        qCCritical(lcPlaylist) << "播放列表未能安全读取，禁止保存：" << loadResult.message;
        // 损坏文件已被改名备份时不能再提示“原文件未被覆盖”；否则用户会去
        // 原路径找一个已经不存在的文件。
        const QString detail = loadResult.corruptBackupPath.isEmpty()
            ? tr("\n\n当前列表为空，原文件未被覆盖。请退出程序，检查或恢复 "
                 "Data/playlists.json 后重启。")
            : tr("\n\n当前列表为空，原文件已备份为 %1。请检查备份后重启。")
                  .arg(loadResult.corruptBackupPath);
        QMessageBox::critical(m_ui.tabs, tr("播放列表未加载"), loadResult.message + detail);
    } else if (!loadResult.message.isEmpty()) {
        emit statusMessage(loadResult.message);
    }

    // 从成功快照恢复的内容需要写回主文件，因此在退出前标记为未保存。
    if (loadResult.recoveredFromBackup)
        setDirty(true);

    // 只迁移一次：旧版 QSettings 里的列表写入 Data/playlists.json 后清键。
    // 迁移标记防止“用户清空列表”后下次启动又被旧键复活。
    // 读取被冻结时不能迁移，否则迁移保存会覆盖读不了的原文件。
    if (!m_storageBlocked && !AppSettings::playlistsMigrated()) {
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
    const int loadedCount = music.size() + video.size();
    music = dedupeMediaItems(music);
    video = dedupeMediaItems(video);
    const bool dedupeChanged = music.size() + video.size() != loadedCount;
    m_musicModel->setItems(music);
    m_videoModel->setItems(video);
    if (dedupeChanged && !m_storageBlocked) {
        // 去重后的列表要立即回写；否则磁盘里仍留着重复项，下次启动又会“变多”。
        if (PlaylistStore::saveAll(persistentItems(music), persistentItems(video)))
            setDirty(false);
        else
            setDirty(true);
    }

    if (!m_storageBlocked && (!music.isEmpty() || !video.isEmpty())) {
        emit statusMessage(tr("已恢复上次的播放列表：音乐 %1 首，视频 %2 个")
                               .arg(music.size())
                               .arg(video.size()));
    }
}

void PlaylistController::scheduleSave()
{
    if (m_storageBlocked)
        return;
    const int generation = ++m_saveGeneration;
    QTimer::singleShot(300, this, [this, generation] {
        if (generation != m_saveGeneration)
            return;
        if (!save())
            qCWarning(lcPlaylist) << "视频进度写入播放列表失败，等待下次保存重试";
    });
}

bool PlaylistController::save()
{
    // 任何同步保存都让排队中的防抖保存失效。
    ++m_saveGeneration;
    if (m_storageBlocked) {
        qCWarning(lcPlaylist) << "播放列表未能安全读取，拒绝保存";
        emit statusMessage(tr("播放列表未能安全读取，已禁止保存以避免覆盖原文件；请检查 Data/playlists.json 后重启"));
        return false;
    }

    if (PlaylistStore::saveAll(persistentItems(m_musicModel->items()),
                               persistentItems(m_videoModel->items()))) {
        setDirty(false);
        return true;
    }

    qCWarning(lcPlaylist) << "播放列表保存失败";
    // 不丢用户刚做的修改：界面保留，页签标 *，右键菜单提供重试。
    setDirty(true);
    emit statusMessage(tr("播放列表保存失败：无法写入数据目录（可能是磁盘空间不足或目录不可写）；修改仍保留，可在列表右键“重试保存”"));
    return false;
}

bool PlaylistController::saveNow()
{
    if (m_storageBlocked)
        return save();
    if (!m_dirty) {
        emit statusMessage(tr("播放列表已是最新，没有需要保存的修改"));
        return true;
    }
    if (save()) {
        emit statusMessage(tr("播放列表已保存"));
        return true;
    }
    return false;
}

bool PlaylistController::retrySave()
{
    if (!m_dirty)
        return true;
    if (m_storageBlocked) {
        emit statusMessage(tr("播放列表未能安全读取，仍禁止保存；请检查 Data/playlists.json 后重启"));
        return false;
    }
    if (!PlaylistStore::saveAll(persistentItems(m_musicModel->items()),
                                persistentItems(m_videoModel->items()))) {
        qCWarning(lcPlaylist) << "播放列表重试保存仍失败";
        emit statusMessage(tr("播放列表仍未保存：数据目录写入失败，修改继续保留"));
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

    // 试听只是音乐搜索的临时语义：用户点视频结果是要正常观看，
    // 不应把它临时化、也不应在列表里显示“试听/不持久化”。
    if (item.type == MediaType::Video) {
        addSearchItem(item, MediaType::Video);
        const int row = m_videoModel->indexOf(item);
        if (row < 0)
            return;
        m_player->setPlaylist(m_videoModel);
        m_player->playRow(row);
        m_ui.tabs->setCurrentIndex(1);
        return;
    }

    PlaylistModel *model = modelFor(item.type);
    // 搜索结果试听只临时进模型；不能顺手 save()，否则用户只是点开听一下，
    // 列表文件就被悄悄改写。previewOnly 由所有保存入口统一过滤。
    MediaItem preview = item;
    preview.previewOnly = true;
    const int existing = model->indexOf(preview);
    if (existing >= 0) {
        const bool permanent = !model->itemAt(existing).previewOnly;
        // 同一条目重搜后可能拿到新的签名直链。试听必须刷新模型里的可变字段，
        // 否则 indexOf() 会定位到旧行并继续播放已经过期的地址。
        if (model->refreshItem(existing, preview) && permanent)
            save(); // 只刷新已持久化条目时写回列表；试听条目仍不落盘
    } else {
        model->addItem(preview);
    }
    const int row = model->indexOf(item);
    if (row < 0)
        return;

    m_player->setPlaylist(model);
    m_player->playRow(row);
    m_ui.tabs->setCurrentIndex(item.type == MediaType::Video ? 1 : 0);
}

void PlaylistController::addSearchItem(const MediaItem &item, MediaType type)
{
    MediaItem copy = item;
    copy.type = type;
    copy.previewOnly = false;
    PlaylistModel *model = modelFor(type);
    if (!model->addItem(copy)) {
        const int existing = model->indexOf(copy);
        if (existing >= 0) {
            const bool wasPreview = model->itemAt(existing).previewOnly;
            // 同一条内容可能先是试听条目，也可能已持久化但直链签名过期。
            // 统一刷新可变字段：前者提升为永久条目，后者写回新地址。
            if (model->refreshItem(existing, copy)) {
                if (save()) {
                    if (type == MediaType::Music) {
                        emit statusMessage(wasPreview ? tr("已加入音乐列表")
                                                      : tr("已更新音乐列表的播放地址"));
                    } else {
                        emit statusMessage(wasPreview ? tr("已加入视频列表")
                                                      : tr("已更新视频列表的播放地址"));
                    }
                }
                return;
            }
        }
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

bool PlaylistController::updateDownloadedItem(const MediaItem &source, const QString &localPath)
{
    if (localPath.isEmpty())
        return false;
    const QFileInfo info(localPath);
    // 只判断 exists 会把 0 字节的半成品/占位文件当成下载成功，随后播放必失败。
    if (!info.exists() || info.size() <= 0) {
        qCWarning(lcPlaylist) << "下载结果不是可用文件" << localPath
                              << (info.exists() ? info.size() : -1);
        return false;
    }

    const QString identity = mediaIdentity(source);
    const QUrl localUrl = QUrl::fromLocalFile(info.absoluteFilePath());
    bool changed = false;

    // 下载时的条目类型可能和实际加入的列表不同（例如把视频加入音乐列表）。
    // 两张列表都按来源身份查找，命中哪张就更新哪张。
    for (PlaylistModel *model : {m_musicModel, m_videoModel}) {
        for (int row = 0; row < model->size(); ++row) {
            MediaItem item = model->itemAt(row);
            if (mediaIdentity(item) != identity)
                continue;

            item.url = localUrl;
            item.online = false;
            item.sourceId.clear();
            item.webUrl = QUrl();
            item.sourceOrigin = QUrl();
            item.headers.clear();
            item.alternateUrls.clear();
            item.positionMs = 0;
            // 下载完成说明用户明确要保留这条内容，试听标记不再生效。
            item.previewOnly = false;
            if (model->updateItem(row, item)) {
                changed = true;
                qCInfo(lcPlaylist) << "下载完成后列表条目已改为本地来源" << item.title
                                   << localUrl.toLocalFile();
            }
        }
    }

    if (changed) {
        if (save())
            emit statusMessage(tr("下载完成，播放列表中的同源条目已更新为本地文件"));
    }
    return changed;
}

bool PlaylistController::promotePreviewRow(MediaType type, int row)
{
    PlaylistModel *model = modelFor(type);
    if (row < 0 || row >= model->rowCount())
        return false;
    MediaItem item = model->itemAt(row);
    if (!item.previewOnly)
        return true;

    // 试听条目原本被保存入口统一过滤；用户明确选择“保存到播放列表”后
    // 先清掉标记再保存，否则仍然会被 persistentItems 排除。
    item.previewOnly = false;
    if (!model->updateItem(row, item)) {
        emit statusMessage(tr("无法保存该条目：列表里已存在相同内容"));
        return false;
    }
    if (!save())
        return false;

    emit statusMessage(tr("已保存到播放列表：%1").arg(item.displayName()));
    return true;
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

void PlaylistController::removeCacheAfterPlayback(const MediaItem &item, int attempt)
{
    // 歌词和播放缓存同属一个条目：删除列表条目后两者都没有再命中的可能。
    // 歌词正文较小、不依赖播放器句柄，先删；失败也不阻塞播放缓存重试。
    if (!LyricsCache::removeForItem(item))
        qCWarning(lcPlaylist) << "歌词缓存删除失败：" << item.title;

    if (MediaCache::removeForItem(item))
        return;
    // attempt 从 0 开始，>=9 时共尝试 0..9 十次（约 4.5 秒），
    // 旧条件 >=10 实际会试 11 次，与注释“最多 5 秒”不符。
    if (attempt >= 9) {
        qCWarning(lcPlaylist) << "播放缓存删除失败，文件句柄可能仍被占用：" << item.title;
        return;
    }
    // 播放器停止后底层 FFmpeg demuxer 可能还会短暂持有文件句柄；
    // 500ms 一次、最多 5 秒，足够覆盖正常的释放时间。
    QTimer::singleShot(500, this, [this, item, attempt] {
        removeCacheAfterPlayback(item, attempt + 1);
    });
}

void PlaylistController::removeRow(MediaType type, int row)
{
    PlaylistModel *model = modelFor(type);
    // 越界 row 不能先取 item，否则公开入口会把默认空条目当真实名字。
    if (row < 0 || row >= model->rowCount())
        return;

    const MediaItem removedItem = model->itemAt(row);
    const QString name = removedItem.displayName();
    const bool removingCurrent = m_player->playlist() == model && m_player->currentRow() == row;

    if (!model->removeRow(row))
        return;

    if (m_player->playlist() == model)
        m_player->onPlaylistRowRemoved(row);

    // 播放器停止/清源后，代理线程可能仍为拖动保留缓存文件句柄；
    // 先请求释放，后面的删除重试才能真正删掉 Windows 上被占用的文件。
    m_player->releaseStreamCacheForItem(removedItem);

    if (removingCurrent)
        emit playbackInvalidated();

    // 缓存和播放列表条目一一对应：条目删除后缓存没有重新命中的可能，
    // 必须同步清理，否则会长期留下用户看不见的孤儿缓存。
    removeCacheAfterPlayback(removedItem);

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

    QVector<MediaItem> removedItems;
    removedItems.reserve(model->rowCount());
    for (int row = 0; row < model->rowCount(); ++row)
        removedItems.append(model->itemAt(row));

    if (m_player->playlist() == model) {
        // 仅 stop 不会释放 QMediaPlayer 持有的本地缓存文件句柄，Windows 上
        // 会导致清空列表后的缓存删除连续失败；按删除当前行的同一路径先停播
        // 并清空 source，再解除模型绑定。
        const int currentRow = m_player->currentRow();
        if (currentRow >= 0)
            m_player->onPlaylistRowRemoved(currentRow);
        else
            m_player->stop();
        m_player->setPlaylist(nullptr);
        emit playbackInvalidated();
    }

    for (const MediaItem &item : std::as_const(removedItems))
        m_player->releaseStreamCacheForItem(item);

    model->clear();
    for (const MediaItem &item : std::as_const(removedItems))
        removeCacheAfterPlayback(item);

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
    QAction *downloadAction = nullptr;
    QAction *savePreviewAction = nullptr;
    QAction *removeAction = nullptr;
    MediaItem selectedItem;
    if (index.isValid()) {
        selectedItem = model->itemAt(index.row());
        if (selectedItem.previewOnly) {
            savePreviewAction = menu.addAction(tr("保存到播放列表（临时条目）"));
            menu.addSeparator();
        }
        // 下载入口只对“还没有本地副本”的条目出现：网络直链和试听条目。
        // 本地试听条目已经有文件，只应“保存到播放列表”，不应再下载。
        if (selectedItem.online) {
            downloadAction = menu.addAction(tr("下载到本地…"));
            menu.addSeparator();
        }
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

    // menu.exec 会开嵌套事件循环，期间下载完成、自动刷新等都可能改动模型；
    // 原先缓存的 index/selectedItem 可能已指向别的条目或越界，必须按身份重新定位。
    auto rowForSelection = [model, &selectedItem]() -> int {
        if (!selectedItem.isValid())
            return -1;
        const QString identity = mediaIdentity(selectedItem);
        for (int row = 0; row < model->rowCount(); ++row) {
            if (mediaIdentity(model->itemAt(row)) == identity)
                return row;
        }
        return -1;
    };

    if (chosen == downloadAction) {
        emit downloadRequested(selectedItem);
    } else if (chosen == playAction) {
        const int row = rowForSelection();
        if (row >= 0)
            playRow(type, row);
        else
            emit statusMessage(tr("该条目已从列表移除"));
    } else if (chosen == savePreviewAction) {
        const int row = rowForSelection();
        if (row >= 0)
            promotePreviewRow(type, row);
        else
            emit statusMessage(tr("该条目已从列表移除"));
    } else if (chosen == removeAction) {
        const int row = rowForSelection();
        if (row >= 0)
            removeRow(type, row);
        else
            emit statusMessage(tr("该条目已从列表移除"));
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
