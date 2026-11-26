/// @file PlaylistController.cpp
/// @brief 音乐/视频两个播放列表的恢复、保存、导入、增删与右键菜单。
#include "ui/PlaylistController.h"

#include "cache/MediaCache.h"
#include "cache/LyricsCache.h"
#include "core/PlaybackController.h"
#include "core/PlaylistModel.h"
#include "core/PlaylistStore.h"
#include "core/PlaylistTreeModel.h"
#include "settings/AppSettings.h"

#include <QAbstractItemView>
#include <QAction>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
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
#include <QMouseEvent>
#include <QPoint>
#include <QRegularExpression>
#include <QShortcut>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
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

/// 各视频客户端把离线缓存封装成自己的私有格式，普通播放器无法直接解出正片。
/// 这里只负责识别和提示，不尝试解码或绕过平台保护。
const QStringList &proprietaryVideoSuffixes()
{
    static const QStringList suffixes = {
        QStringLiteral("bhd"),   // 暴风影音
        QStringLiteral("xv"),    // 迅雷影音 / 迅雷看看缓存
        QStringLiteral("xlmv"),  // 迅雷商城
        QStringLiteral("kux"),   // 优酷
        QStringLiteral("qsv"),   // 爱奇艺
        QStringLiteral("qlv"),   // 腾讯视频
        QStringLiteral("ifox"),  // 搜狐影音
        QStringLiteral("qmv"),   // 快播
        QStringLiteral("mgcf"),  // 芒果TV
        QStringLiteral("letv"),  // 乐视视频
        QStringLiteral("bdv"),   // 百度视频
    };
    return suffixes;
}

QString proprietaryPlayerForSuffix(const QString &suffix)
{
    static const QHash<QString, QString> players = {
        {QStringLiteral("bhd"), QStringLiteral("暴风影音")},
        {QStringLiteral("xv"), QStringLiteral("迅雷影音")},
        {QStringLiteral("xlmv"), QStringLiteral("迅雷影音")},
        {QStringLiteral("kux"), QStringLiteral("优酷")},
        {QStringLiteral("qsv"), QStringLiteral("爱奇艺")},
        {QStringLiteral("qlv"), QStringLiteral("腾讯视频")},
        {QStringLiteral("ifox"), QStringLiteral("搜狐影音")},
        {QStringLiteral("qmv"), QStringLiteral("快播")},
        {QStringLiteral("mgcf"), QStringLiteral("芒果TV")},
        {QStringLiteral("letv"), QStringLiteral("乐视视频")},
        {QStringLiteral("bdv"), QStringLiteral("百度视频")},
    };
    return players.value(suffix.toLower());
}

QString proprietaryVideoFilterPatterns()
{
    QStringList patterns;
    const QStringList suffixes = proprietaryVideoSuffixes();
    patterns.reserve(suffixes.size());
    for (const QString &suffix : suffixes)
        patterns.append(QStringLiteral("*.") + suffix);
    return patterns.join(QLatin1Char(' '));
}

/// 播放列表分组标题：优先用源解析给出的基础剧名；旧条目没有该字段时
/// 从“XXX（共 N 集）”里去掉计数后缀，避免折叠后重复显示两遍集数。
QString collectionTitleFor(const MediaItem &item)
{
    QString title = item.collectionTitle.trimmed();
    if (title.isEmpty())
        title = item.title;
    static const QRegularExpression count(
        QStringLiteral("\\s*[（(]\\s*共\\s*\\d+\\s*集\\s*[）)]\\s*$"));
    title.remove(count);
    return title.trimmed();
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
    // 视频模型保持扁平：播放、自动下一集、进度保存都按真实集行号工作；
    // 树模型只在其上折叠展示，不改变底层行号。
    m_videoTreeModel = new PlaylistTreeModel(m_videoModel, this);
    m_ui.videoView->setModel(m_videoTreeModel);
    // QTreeView 默认会保留一列表头；播放列表只有一列、不需要表头，
    // 显式隐藏可避免深色主题下顶部留出一条白色表头带。
    m_ui.videoView->setHeaderHidden(true);
    m_ui.videoView->header()->setVisible(false);
    m_ui.videoView->setRootIsDecorated(true);
    m_ui.videoView->setItemsExpandable(true);
    m_ui.videoView->setExpandsOnDoubleClick(false);
    m_ui.videoView->setUniformRowHeights(true);
    m_ui.videoView->setIndentation(16);
    m_ui.videoView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ui.videoView->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 剧头单击展开/收起需要看到整行点击位置；箭头点击仍由 QTreeView 原生处理。
    m_ui.videoView->viewport()->installEventFilter(this);
    m_musicTabTitle = m_ui.tabs->tabText(0);
    m_videoTabTitle = m_ui.tabs->tabText(1);

    bindView(m_ui.musicView, m_musicModel, MediaType::Music);
    bindView(m_ui.videoView, m_videoModel, MediaType::Video);

    // 树模型重建会丢失 QTreeView 的展开态；按用户此前展开的分组重新展开。
    connect(m_videoTreeModel, &PlaylistTreeModel::structureRebuilt, this, [this] {
        for (const QString &key : m_videoTreeModel->expandedGroupKeys()) {
            const QModelIndex group = m_videoTreeModel->indexForGroup(key);
            if (group.isValid())
                m_ui.videoView->setExpanded(group, true);
        }
    });
    connect(m_ui.videoView, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        m_videoTreeModel->setGroupExpanded(index.data(PlaylistTreeModel::GroupKeyRole).toString(),
                                           true);
    });
    connect(m_ui.videoView, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        m_videoTreeModel->setGroupExpanded(index.data(PlaylistTreeModel::GroupKeyRole).toString(),
                                           false);
    });

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

QAbstractItemView *PlaylistController::activeView() const
{
    return m_ui.tabs->currentIndex() == 1 ? static_cast<QAbstractItemView *>(m_ui.videoView)
                                         : static_cast<QAbstractItemView *>(m_ui.musicView);
}

int PlaylistController::activeSourceRow() const
{
    QAbstractItemView *view = activeView();
    if (view == nullptr)
        return -1;
    const QModelIndex current = view->currentIndex();
    if (!current.isValid())
        return -1;
    return sourceRowForIndex(m_ui.tabs->currentIndex() == 1 ? MediaType::Video : MediaType::Music,
                             current);
}

void PlaylistController::selectSourceRow(MediaType type, int row)
{
    if (type == MediaType::Music) {
        m_ui.musicView->setCurrentIndex(m_musicModel->index(row));
        m_ui.musicView->scrollTo(m_musicModel->index(row), QAbstractItemView::EnsureVisible);
        return;
    }

    const QModelIndex index = m_videoTreeModel->indexForSourceRow(row);
    if (!index.isValid())
        return;
    // 当前播放的集数必须可见：如果它被折叠在剧头下，先展开父节点。
    if (index.parent().isValid())
        m_ui.videoView->expand(index.parent());
    m_ui.videoView->setCurrentIndex(index);
    m_ui.videoView->scrollTo(index, QAbstractItemView::EnsureVisible);
}

/// @brief 从 Data/playlists.json 恢复两个列表，并按需迁移旧版 QSettings 数据。
/// @note 读取被冻结（storageBlocked）时不迁移、不回写，只提示用户修复原文件；
///       成功从备份恢复的内容会标记为未保存，退出前写回主文件。
///       恢复后会去重，去重结果变化时立即回写，避免下次启动又“变多”。
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

/// @brief 退出前保存“当时正在播放的条目”。
/// @note 用 mediaIdentity() 而不是行号：下次启动前列表可能被搜索/下载改写，
///       行号会漂移，身份仍能唯一定位同一条目。
void PlaylistController::rememberPlayingItem()
{
    if (m_player == nullptr || !m_player->hasCurrentItem())
        return;

    PlaylistModel *playingModel = m_player->playlist();
    MediaType type = MediaType::Music;
    if (playingModel == m_videoModel) {
        type = MediaType::Video;
    } else if (playingModel != m_musicModel) {
        // 搜索窗口的临时列表不属于播放列表，退出后不存在，记录也没有意义。
        return;
    }

    const MediaItem current = m_player->currentItem();
    // 试听条目只在当前会话存在，播放列表保存时会被过滤掉；不能拿它当恢复目标。
    if (current.previewOnly)
        return;
    const int row = playingModel->indexOf(current);
    if (row < 0)
        return;

    AppSettings::setLastPlayedItem(type == MediaType::Video ? 1 : 0,
                                  mediaIdentity(playingModel->itemAt(row)));
}

/// @brief 启动后恢复上次退出时正在播放的条目为默认选中项。
/// @note 只选中、不自动播放：沿用用户原有的启动行为，按播放键才起播。
void PlaylistController::restoreLastSelection()
{
    const int typeValue = AppSettings::lastPlayedItemType();
    const QString identity = AppSettings::lastPlayedItemIdentity();
    if (typeValue < 0 || identity.isEmpty())
        return;

    const MediaType type = typeValue == 1 ? MediaType::Video : MediaType::Music;
    PlaylistModel *model = modelFor(type);
    int row = -1;
    for (int i = 0; i < model->size(); ++i) {
        if (mediaIdentity(model->itemAt(i)) == identity) {
            row = i;
            break;
        }
    }
    if (row < 0)
        return;

    // 先切到条目所属页签，再选中；视频条目会展开剧集分组。
    m_ui.tabs->setCurrentIndex(type == MediaType::Video ? 1 : 0);
    selectSourceRow(type, row);
}

/// @brief 300ms 防抖保存，用 generation 丢弃过期回调；读取冻结时直接跳过。
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

/// @brief 立即把两个列表写入 Data/playlists.json。
/// @return 成功返回 true；读取被冻结或写盘失败返回 false，并保留 dirty 标记供重试。
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

/// @brief 用户主动保存（右键“保存列表”）：无修改时直接提示，不重复写盘。
/// @return 列表已是最新或写入成功返回 true。
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

/// @brief 保存失败后的重试入口；读取被冻结时仍拒绝写盘。
/// @return 无修改或重试成功返回 true。
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

/// @brief 更新未保存标记并刷新页签标题上的 "*"。
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
    addSearchItems({item}, type);
}

/// @brief 批量加入搜索结果：同一部剧的几十集只触发一次保存。
/// @note 单条仍沿用原来的“已存在则刷新地址/提升试听”语义；批量模式额外
///       汇总新增、刷新、重复数量，避免 129 集逐条写盘并覆盖状态栏 129 次。
void PlaylistController::addSearchItems(const QVector<MediaItem> &items, MediaType type)
{
    if (items.isEmpty())
        return;

    PlaylistModel *model = modelFor(type);
    int added = 0;
    int refreshed = 0;
    int skipped = 0;
    // 分组大小按本次整组加入的条目统计；右键“加入视频列表（共 N 集）”会把
    // 全部剧集一次传进来，播放列表据此在树里折叠成一行。
    QHash<QString, int> collectionCounts;
    for (const MediaItem &item : items) {
        if (type == MediaType::Video && !item.collectionKey.isEmpty())
            ++collectionCounts[item.collectionKey];
    }
    for (const MediaItem &original : items) {
        MediaItem copy = original;
        copy.type = type;
        copy.previewOnly = false;
        if (type == MediaType::Video && !copy.collectionKey.isEmpty()) {
            copy.collectionSize = collectionCounts.value(copy.collectionKey, 1);
            copy.collectionTitle = collectionTitleFor(copy);
        }

        if (model->addItem(copy)) {
            ++added;
            continue;
        }
        const int existing = model->indexOf(copy);
        if (existing >= 0 && model->refreshItem(existing, copy)) {
            ++refreshed;
            continue;
        }
        ++skipped;
    }

    if (added == 0 && refreshed == 0) {
        emit statusMessage(type == MediaType::Music ? tr("音乐列表中已存在")
                                                    : tr("视频列表中已存在"));
        return;
    }

    if (!save())
        return;

    if (items.size() == 1 && added == 1) {
        emit statusMessage(type == MediaType::Music ? tr("已加入音乐列表")
                                                    : tr("已加入视频列表"));
        return;
    }
    QString text = type == MediaType::Music
                       ? tr("已加入音乐列表：新增 %1 首").arg(added)
                       : tr("已加入视频列表：新增 %1 集").arg(added);
    if (refreshed > 0)
        text += type == MediaType::Music ? tr("，更新 %1 首").arg(refreshed)
                                         : tr("，更新 %1 集").arg(refreshed);
    if (skipped > 0)
        text += type == MediaType::Music ? tr("，跳过重复 %1 首").arg(skipped)
                                         : tr("，跳过重复 %1 集").arg(skipped);
    emit statusMessage(text);
}

/// @brief 逐个导入本地文件；专用/加密格式统一拦截并汇总提示。
/// @param type 目标列表类型，决定条目归入音乐或影视。
/// @param files 待导入的本地路径列表。
/// @note 标题去掉扩展名；同目录同名 .lrc 自动作为本地歌词；同名条目由模型去重。
///       被拦下的格式按客户端分组提示，并阻止整批静默失败。
void PlaylistController::addLocalFiles(MediaType type, const QStringList &files)
{
    PlaylistModel *model = modelFor(type);
    int added = 0;
    QStringList skippedFiles;
    QStringList skippedSuffixes;

    for (const QString &path : files) {
        const QFileInfo info(path);
        const QString suffix = info.suffix().toLower();
        const QString proprietaryPlayer = proprietaryPlayerForSuffix(suffix);
        if (!proprietaryPlayer.isEmpty()) {
            skippedFiles.append(info.fileName());
            if (!skippedSuffixes.contains(suffix))
                skippedSuffixes.append(suffix);
            continue;
        }

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

    if (!skippedFiles.isEmpty()) {
        QStringList players;
        for (const QString &suffix : skippedSuffixes) {
            const QString player = proprietaryPlayerForSuffix(suffix);
            if (!player.isEmpty() && !players.contains(player))
                players.append(player);
        }

        QWidget *parentWidget = qobject_cast<QWidget *>(parent());
        QMessageBox box(parentWidget);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(tr("播放器专用格式"));
        box.setText(tr("已跳过 %1 个专用格式文件").arg(skippedFiles.size()));
        box.setInformativeText(
            tr("这些文件是其他播放器的专用/加密缓存格式，当前播放器不保证能直接播放正片。\n"
               "请用对应客户端打开，或在客户端内转码/导出为 MP4、MKV 后再加入列表。\n"
               "涉及客户端：%1\n"
               "若文件只是改了后缀，可先改回 .mp4/.mkv 再试。")
                .arg(players.join(QStringLiteral("、"))));
        box.setDetailedText(skippedFiles.join(QLatin1Char('\n')));
        box.exec();
    }

    if (added > 0) {
        if (save()) {
            emit statusMessage(
                skippedFiles.isEmpty()
                    ? tr("已添加 %1 个文件").arg(added)
                    : tr("已添加 %1 个文件，跳过 %2 个专用格式文件")
                          .arg(added)
                          .arg(skippedFiles.size()));
        }
    } else if (!skippedFiles.isEmpty()) {
        emit statusMessage(tr("已跳过 %1 个专用格式文件").arg(skippedFiles.size()));
    } else {
        emit statusMessage(tr("列表中已存在相同文件"));
    }
    qCInfo(lcPlaylist) << "加载本地文件" << (type == MediaType::Music ? "音乐" : "视频") << "共"
                       << files.size() << "个，新增" << added << "个，跳过专用格式"
                       << skippedFiles.size() << "个";
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

/// @brief 弹出本地文件选择框并导入。
/// @param type 目标列表类型。
void PlaylistController::chooseLocalFiles(MediaType type)
{
    QWidget *parent = m_ui.tabs != nullptr ? m_ui.tabs->window() : nullptr;
    const QString filter = localFileDialogFilter(type);
    const QStringList files = QFileDialog::getOpenFileNames(
        parent, type == MediaType::Music ? tr("选择音乐文件") : tr("选择视频文件"),
        AppSettings::lastDir(), filter);
    if (files.isEmpty())
        return;

    AppSettings::setLastDir(QFileInfo(files.first()).absolutePath());
    addLocalFiles(type, files);
}

/// @brief 生成打开文件对话框的过滤器字符串。
/// @param type 目标列表类型。
/// @return 音乐只列常见音频后缀；影视先列常见视频后缀，再单列一组
///         “播放器专用格式”，让用户能看到文件，但导入时仍会被拦截提示。
QString PlaylistController::localFileDialogFilter(MediaType type)
{
    if (type == MediaType::Music) {
        return tr("音乐文件 (*.mp3 *.flac *.wav *.m4a *.m4b *.aac *.ogg *.opus *.wma *.ape "
                  "*.aiff *.aif *.amr *.ac3 *.mka *.caf *.wv);;所有文件 (*)");
    }

    // 专用格式单独列一组，用户能看见文件，但导入时会统一拦截并提示。
    const QString proprietaryFilter =
        tr("播放器专用格式 (%1);;").arg(proprietaryVideoFilterPatterns());
    return tr("视频文件 (*.mp4 *.m4v *.mkv *.avi *.mov *.flv *.wmv *.webm *.ts *.m2ts "
              "*.mts *.mpg *.mpeg *.3gp *.3g2 *.ogv *.rmvb *.rm *.vob *.asf *.divx "
              "*.f4v *.m2v);;%1所有文件 (*)")
        .arg(proprietaryFilter);
}

void PlaylistController::bindView(QAbstractItemView *view, PlaylistModel *model, MediaType type)
{
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view, model, type](const QPoint &pos) {
                showContextMenu(type, view, model, pos);
            });

    // 选中一行后按 Delete 直接移除；分组节点删除整部剧，单集删除单条。
    auto *shortcut = new QShortcut(QKeySequence::Delete, view);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, [this, view, type] {
        const QModelIndex index = view->currentIndex();
        if (index.isValid())
            removeIndex(type, index);
    });
}

int PlaylistController::sourceRowForIndex(MediaType type, const QModelIndex &index) const
{
    if (!index.isValid())
        return -1;
    if (type == MediaType::Video)
        return m_videoTreeModel != nullptr ? m_videoTreeModel->sourceRow(index) : -1;
    return index.row();
}

bool PlaylistController::isGroupIndex(MediaType type, const QModelIndex &index) const
{
    return type == MediaType::Video && m_videoTreeModel != nullptr && index.isValid()
           && m_videoTreeModel->isGroup(index);
}

bool PlaylistController::pointOnDisclosure(const QPoint &pos, const QModelIndex &index) const
{
    const QRect itemRect = m_ui.videoView->visualRect(index);
    if (!itemRect.isValid())
        return false;

    // 原生展开箭头画在条目矩形左侧的缩进区；条目矩形本身从文字列开始。
    // 不能用 QStyle::SE_TreeViewDisclosureItem：部分样式会把整个条目矩形当作命中区，
    // 导致点击剧头正文也被当成点击箭头而不再切换。
    return pos.x() < itemRect.left() && pos.x() >= itemRect.left() - m_ui.videoView->indentation();
}

/// @brief 单击剧头正文展开/收起；双击只切换一次且不再播放第 1 集。
/// @note 点击左侧原生箭头时不在这里重复切换，否则会和 QTreeView 默认行为抵消。
bool PlaylistController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_ui.videoView->viewport())
        return QObject::eventFilter(watched, event);

    QTreeView *view = m_ui.videoView;
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        m_skipGroupRelease = false;
        m_pressedGroupKey.clear();
        m_pressedOnDisclosure = false;
        if (mouse->button() == Qt::LeftButton) {
            const QPoint pos = mouse->position().toPoint();
            const QModelIndex index = view->indexAt(pos);
            if (isGroupIndex(MediaType::Video, index)) {
                m_pressedGroupKey = index.data(PlaylistTreeModel::GroupKeyRole).toString();
                m_pressedGroupPos = pos;
                m_pressedOnDisclosure = pointOnDisclosure(pos, index);
            }
        }
        break;
    }
    case QEvent::MouseButtonDblClick: {
        const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        if (isGroupIndex(MediaType::Video, view->indexAt(mouse->position().toPoint()))) {
            // 第一次单击的释放事件已经完成切换，双击的第二次释放不再切换。
            m_skipGroupRelease = true;
            m_pressedGroupKey.clear();
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            break;
        if (m_skipGroupRelease) {
            m_skipGroupRelease = false;
            m_pressedGroupKey.clear();
            break;
        }

        const QString key = m_pressedGroupKey;
        const QPoint pos = mouse->position().toPoint();
        const bool onDisclosure = m_pressedOnDisclosure;
        const QPoint pressPos = m_pressedGroupPos;
        m_pressedGroupKey.clear();
        m_pressedOnDisclosure = false;
        if (key.isEmpty() || onDisclosure)
            break;
        // 按住拖动（滚动/选择）不算点击，避免滚动列表时误折叠。
        if ((pos - pressPos).manhattanLength() > 4)
            break;

        const QModelIndex index = view->indexAt(pos);
        if (!isGroupIndex(MediaType::Video, index)
            || index.data(PlaylistTreeModel::GroupKeyRole).toString() != key) {
            break;
        }
        view->setExpanded(index, !view->isExpanded(index));
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

/// @brief 树/列表当前 index 的删除入口：视频分组节点删除整部剧。
void PlaylistController::removeIndex(MediaType type, const QModelIndex &index)
{
    if (!index.isValid())
        return;
    if (type == MediaType::Video && m_videoTreeModel != nullptr
        && m_videoTreeModel->isGroup(index)) {
        removeCollection(type, m_videoTreeModel->groupKey(index));
        return;
    }
    removeRow(type, sourceRowForIndex(type, index));
}

/// @brief 删除整部剧：逐集移除扁平模型行，最后统一保存一次。
void PlaylistController::removeCollection(MediaType type, const QString &key)
{
    if (type != MediaType::Video || key.isEmpty())
        return;
    PlaylistModel *model = m_videoModel;
    const QVector<int> rows = m_videoTreeModel->sourceRowsForGroup(key);
    if (rows.isEmpty())
        return;

    const QString name = m_videoTreeModel->indexForGroup(key).data(Qt::DisplayRole).toString();
    bool removedAny = false;
    // 从后往前删，前面行号不会因后面的删除而移动。
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int row = rows.at(i);
        const MediaItem item = model->itemAt(row);
        if (!item.isValid())
            continue;
        const bool removingCurrent = m_player->playlist() == model
                                     && m_player->currentRow() == row;
        if (!model->removeRow(row))
            continue;
        removedAny = true;
        if (m_player->playlist() == model)
            m_player->onPlaylistRowRemoved(row);
        m_player->releaseStreamCacheForItem(item);
        if (removingCurrent)
            emit playbackInvalidated();
        removeCacheAfterPlayback(item);
    }

    if (removedAny && save())
        emit statusMessage(tr("已从列表移除整部剧：%1").arg(name));
}

/// @brief 停止播放后清理条目的歌词缓存与播放缓存，文件被占用时重试。
/// @param item 被删除的媒体条目。
/// @param attempt 重试序号；达到 9 时共尝试 0..9 十次（约 4.5 秒）后放弃。
void PlaylistController::removeCacheAfterPlayback(const MediaItem &item, int attempt)
{
    // 歌词和播放缓存同属一个条目：删除列表条目后两者都没有再命中的可能。
    // 歌词正文较小、不依赖播放器句柄，先删；失败也不阻塞播放缓存重试。
    if (!LyricsCache::removeForItem(item))
        qCWarning(lcPlaylist) << "歌词缓存删除失败：" << item.title;

    if (MediaCache::removeForItem(item))
        return;
    // attempt 从 0 开始，>=9 时共尝试 0..9 十次（约 4.5 秒），避免无限重试。
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

/// @brief 移除指定行：先释放播放器与缓存句柄，再删条目和缓存并落盘。
/// @param type 列表类型。
/// @param row 行号；越界时直接返回。
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

/// @brief 清空整个列表，并同步释放句柄、删除全部条目缓存。
/// @param type 列表类型。
/// @note 正在播放的列表必须先停播并清空 source，再解除模型绑定，
///       否则 QMediaPlayer 持有缓存文件句柄会导致后续删除失败。
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

/// @brief 弹出列表右键菜单：重试保存、播放、试听转正式、删除、清空、导入本地文件。
/// @param type 列表类型。
/// @param view 触发菜单的视图。
/// @param model 该视图对应的模型。
/// @param pos 视口内点击位置。
void PlaylistController::showContextMenu(MediaType type, QAbstractItemView *view,
                                         PlaylistModel *model, const QPoint &pos)
{
    const QModelIndex index = view->indexAt(pos);
    const bool isVideoGroup =
        type == MediaType::Video && m_videoTreeModel != nullptr
        && m_videoTreeModel->isGroup(index);
    const QString selectedGroupKey =
        isVideoGroup ? m_videoTreeModel->groupKey(index) : QString();
    const int selectedGroupSize = isVideoGroup ? m_videoTreeModel->groupSize(index) : 0;

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
    const int selectedRow = sourceRowForIndex(type, index);
    if (index.isValid()) {
        selectedItem = model->itemAt(selectedRow);
        if (!isVideoGroup && selectedItem.previewOnly) {
            savePreviewAction = menu.addAction(tr("保存到播放列表（临时条目）"));
            menu.addSeparator();
        }
        // 下载入口只对“还没有本地副本”的条目出现：网络直链和试听条目。
        // 分组节点代表整部剧，不在这里替用户批量下载 25 集。
        if (!isVideoGroup && selectedItem.online) {
            downloadAction = menu.addAction(tr("下载到本地…"));
            menu.addSeparator();
        }
        playAction = menu.addAction(isVideoGroup ? tr("播放第 1 集") : tr("播放"));
        removeAction = menu.addAction(
            isVideoGroup ? tr("移除整部剧（共 %1 集）").arg(selectedGroupSize)
                         : tr("从列表移除"));
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
    // 下载完成会把条目从“在线 web/url 身份”改成“本地 file 身份”，此时原身份
    // 必然找不到；再用稳定的标题/歌手/类型在唯一匹配项上兜底。
    auto rowForSelection = [model, &selectedItem, selectedRow]() -> int {
        if (!selectedItem.isValid())
            return -1;
        const QString identity = mediaIdentity(selectedItem);
        for (int row = 0; row < model->rowCount(); ++row) {
            if (mediaIdentity(model->itemAt(row)) == identity)
                return row;
        }
        auto looksSame = [](const MediaItem &a, const MediaItem &b) {
            return a.type == b.type && a.title == b.title && a.artist == b.artist;
        };
        if (selectedRow >= 0 && selectedRow < model->rowCount()) {
            const MediaItem current = model->itemAt(selectedRow);
            // 只在“原来在线、现在已变成本地”这一种身份切换上接受原行。
            if (selectedItem.online && !current.online && looksSame(current, selectedItem))
                return selectedRow;
        }
        int candidate = -1;
        for (int row = 0; row < model->rowCount(); ++row) {
            const MediaItem current = model->itemAt(row);
            if (!looksSame(current, selectedItem))
                continue;
            if (candidate >= 0)
                return -1; // 同标题同歌手有多条，无法确定用户当初选的是哪条
            candidate = row;
        }
        return candidate;
    };

    if (chosen == downloadAction) {
        // 菜单打开期间下载可能已完成，把该条目改成在线→本地；不能再用
        // 菜单打开前缓存的 selectedItem 按旧网络地址重复下载。
        const int row = rowForSelection();
        if (row < 0) {
            emit statusMessage(tr("该条目已从列表移除"));
        } else {
            const MediaItem current = model->itemAt(row);
            if (!current.online) {
                emit statusMessage(tr("该条目已经是本地文件，无需再次下载"));
            } else {
                emit downloadRequested(current);
            }
        }
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
        if (isVideoGroup) {
            // 分组节点删除整部剧；key 在菜单打开期间保持稳定，树重建后仍能定位。
            removeCollection(type, selectedGroupKey);
            return;
        }
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

/// @brief 播放指定列表的指定行。
void PlaylistController::playRow(MediaType type, int row)
{
    m_player->setPlaylist(modelFor(type));
    m_player->playRow(row);
}
