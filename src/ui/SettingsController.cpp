#include "ui/SettingsController.h"

#include "core/PlaybackController.h"
#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
#include "ui/SettingsDialog.h"
#include "ui/ThemeManager.h"

#include <QGuiApplication>
#include <QMainWindow>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>

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
    Q_ASSERT(m_ui.volumeSlider != nullptr);
}

void SettingsController::restore()
{
    // 音量、播放模式、窗口尺寸全部来自设置，不再有散落的默认值
    const int initialVolume = AppSettings::defaultVolume();
    m_player->setVolumePercent(initialVolume);
    m_appliedDefaultVolume = initialVolume;
    m_ui.volumeSlider->setValue(m_player->volumePercent());

    const int modeValue = AppSettings::playbackMode();
    m_player->setMode(static_cast<PlaybackController::Mode>(qBound(0, modeValue, 2)));

    applyWindowPreset();
}

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

void SettingsController::save()
{
    AppSettings::setDefaultVolume(m_player->volumePercent());
    AppSettings::setPlaybackMode(static_cast<int>(m_player->mode()));
    // 播放列表现只写 Data/playlists.json；QSettings 双写会让删除后的列表“复活”。

    // 只有"记住上次"才写几何，固定尺寸预设下写进去会互相打架
    if (AppSettings::windowPreset() == WindowPreset::Last)
        AppSettings::setSavedGeometry(m_ui.window->saveGeometry());
}

void SettingsController::apply()
{
    ThemeManager::instance().apply(AppSettings::themeId());
    resizeToPreset();

    // 设置页的“默认音量”只在当前音量仍是上一次默认值时即时生效。
    // 用户在播放/设置页手动调过音量后，从设置页返回不应被默认值打回去。
    const int newDefault = AppSettings::defaultVolume();
    if (m_appliedDefaultVolume < 0 || m_player->volumePercent() == m_appliedDefaultVolume)
        m_player->setVolumePercent(newDefault);
    m_appliedDefaultVolume = newDefault;
    {
        QSignalBlocker blocker(m_ui.volumeSlider);
        m_ui.volumeSlider->setValue(m_player->volumePercent());
    }

    // 侧栏 / 控制条的折叠策略、歌词高亮由窗口响应 applied() 处理
    emit applied();
}

void SettingsController::openDialog()
{
    SettingsDialog dialog(m_registry, m_http, m_ui.window);
    if (dialog.exec() == QDialog::Accepted)
        apply();
}
