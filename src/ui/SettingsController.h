#ifndef SETTINGSCONTROLLER_H
#define SETTINGSCONTROLLER_H

#include <QObject>

class PlaybackController;
class SourceRegistry;
class HttpClient;
class QMainWindow;
class QSlider;

/// 设置面板依赖的控件：只给主窗与音量滑杆，其他布局仍归窗口。
struct SettingsUi
{
    QMainWindow *window = nullptr;
    QSlider *volumeSlider = nullptr;
};

/// 设置读取、保存、主题/窗口预设应用与设置对话框入口。
/// 本类只碰 AppSettings 与主窗几何，不直接改侧栏、歌词等面板；
/// 面板联动通过 applied() 交给窗口决定。
class SettingsController : public QObject
{
    Q_OBJECT

public:
    SettingsController(PlaybackController *player, SourceRegistry *registry, HttpClient *http,
                       const SettingsUi &ui, QObject *parent = nullptr);

    /// 启动时恢复音量、播放模式和窗口尺寸
    void restore();
    /// 关闭时保存音量、播放模式和“记住上次”的窗口几何
    void save();
    /// 设置被接受后重新应用主题、窗口尺寸和音量
    void apply();
    /// 打开设置对话框；接受后自动 apply()
    void openDialog();

signals:
    /// 设置已应用：窗口据此刷新侧栏可见性与歌词高亮
    void applied();

private:
    void applyWindowPreset();
    void resizeToPreset();

    PlaybackController *m_player = nullptr;
    SourceRegistry *m_registry = nullptr;
    HttpClient *m_http = nullptr;
    SettingsUi m_ui;
};

#endif // SETTINGSCONTROLLER_H
