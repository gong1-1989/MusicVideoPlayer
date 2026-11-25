/// @file SettingsController.cpp
/// @brief 设置的读取、回写与即时应用：播放模式/窗口预设/主题。
#include "ui/SettingsController.h"

#include "core/PlaybackController.h"
#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
#include "ui/SettingsDialog.h"
#include "ui/ThemeManager.h"

#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>

namespace {

/// 预设尺寸只是目标值：屏幕装不下时按可用工作区收一收，否则布局会被挤到裁切
QSize fitToAvailableScreen(const QSize &size)
{
    QSize target = size;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        target.setWidth(qMin(target.width(), available.width() - 24));
        target.setHeight(qMin(target.height(), available.height() - 32));
    }
    return target;
}

/// 当前屏幕的可用工作区（扣掉任务栏）
QSize availableWindowSize()
{
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        return QSize(available.width(), available.height());
    }
    return QSize(1280, 720);
}

} // namespace

/// @brief 构造控制器。
/// @param player 播放控制器，必须非空。
/// @param registry 搜索源注册表，必须非空。
/// @param http 搜索源管理复用的 HTTP 客户端，必须非空。
/// @param ui 主窗口相关控件集合。
SettingsController::SettingsController(PlaybackController *player, SourceRegistry *registry,
                                       HttpClient *http, const SettingsUi &ui, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_registry(registry)
    , m_http(http)
    , m_ui(ui)
{
    Q_ASSERT(m_player != nullptr);
    Q_ASSERT(m_registry != nullptr);
    Q_ASSERT(m_http != nullptr);
    Q_ASSERT(m_ui.window != nullptr);
}

/// @brief 启动时恢复音量、播放模式与窗口几何。
/// @note 音量只有播放控制条一个入口，这里仅把上次退出时保存的播放音量还原。
void SettingsController::restore()
{
    // 音量、播放模式、窗口尺寸全部来自设置，不再有散落的默认值
    m_player->setVolumePercent(AppSettings::playbackVolume());

    const int modeValue = AppSettings::playbackMode();
    m_player->setMode(static_cast<PlaybackController::Mode>(qBound(0, modeValue, 2)));

    applyWindowPreset();
}

/// @brief 按窗口预设调整尺寸并居中。
/// @note 只有“记住上次”沿用历史几何；自适应铺满可用区，固定预设按标准分辨率且超屏自动收紧。
void SettingsController::applyWindowPreset()
{
    const WindowPreset preset = AppSettings::windowPreset();

    // 记住上次：直接用上次的几何，不再套预设
    if (preset == WindowPreset::Last) {
        const QByteArray geometry = AppSettings::savedGeometry();
        if (!geometry.isEmpty())
            m_ui.window->restoreGeometry(geometry);
        return;
    }

    // 自适应 = 铺满可用工作区；固定预设 = 标准分辨率
    const QSize presetSize = preset == WindowPreset::Auto
                                 ? availableWindowSize()
                                 : AppSettings::presetWindowSize(preset);
    if (!presetSize.isValid())
        return;

    // 预设只是目标值：屏幕装不下时按可用工作区收一收
    const QSize target = fitToAvailableScreen(presetSize);
    m_ui.window->resize(target);

    // 自适应/固定尺寸都不沿用历史位置，启动时居中
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        m_ui.window->move(available.center() - QPoint(target.width() / 2, target.height() / 2));
    }
}

/// @brief 设置变更后立即套用窗口尺寸（不重定位）。
/// @note “记住上次”模式下不改尺寸，交给用户自己拉。
void SettingsController::resizeToPreset()
{
    // 尺寸实时跟着设置走：自适应铺开，固定预设按标准分辨率（超屏自动收）
    const WindowPreset preset = AppSettings::windowPreset();
    if (preset == WindowPreset::Last)
        return;

    const QSize target = preset == WindowPreset::Auto
                             ? fitToAvailableScreen(availableWindowSize())
                             : fitToAvailableScreen(AppSettings::presetWindowSize(preset));
    if (target.isValid())
        m_ui.window->resize(target);
}

/// @brief 退出或设置确认时回写播放音量、播放模式与窗口几何。
/// @note 播放列表已改为只写 Data/playlists.json，这里不再双写 QSettings，
///       否则删除后的列表会“复活”。
void SettingsController::save()
{
    AppSettings::setPlaybackVolume(m_player->volumePercent());
    AppSettings::setPlaybackMode(static_cast<int>(m_player->mode()));
    // 播放列表现只写 Data/playlists.json；QSettings 双写会让删除后的列表“复活”。

    // 只有"记住上次"才写几何，固定尺寸预设下写进去会互相打架
    if (AppSettings::windowPreset() == WindowPreset::Last)
        AppSettings::setSavedGeometry(m_ui.window->saveGeometry());
}

/// @brief 设置确认后即时生效：主题与窗口尺寸。
/// @note 音量只剩播放控制条一个入口，不再由设置页写回。
void SettingsController::apply()
{
    ThemeManager::instance().apply(AppSettings::themeId());
    resizeToPreset();

    // 侧栏 / 控制条的折叠策略、歌词高亮由窗口响应 applied() 处理
    emit applied();
}

/// @brief 打开模态设置窗口，用户确认后立即应用。
void SettingsController::openDialog()
{
    SettingsDialog dialog(m_registry, m_http, m_ui.window);
    if (dialog.exec() == QDialog::Accepted)
        apply();
}
