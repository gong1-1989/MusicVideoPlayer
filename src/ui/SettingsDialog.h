#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include "net/SourceConfig.h"

#include <QDialog>
#include <QHash>
#include <QVector>

class HttpClient;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class SourceRegistry;

QT_BEGIN_NAMESPACE
namespace Ui {
class SettingsDialog;
}
QT_END_NAMESPACE

/// 系统设置：外观主题、窗口尺寸、默认音量、搜索结果条数、下载上限、搜索源管理入口。
/// 所有可配置项都只在这里出现一次，值写入 AppSettings，调用方读回即可。
/// 界面由 SettingsDialog.ui 描述，这里只做数据填充与读写。
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(SourceRegistry *registry = nullptr, HttpClient *http = nullptr,
                            QWidget *parent = nullptr);
    ~SettingsDialog() override;

protected:
    void accept() override;

private:
    void bindUi();
    void loadValues();
    void refreshSourcesLabel();
    void openSourceManager();
    void openShortcutSettings();

    Ui::SettingsDialog *ui;
    SourceRegistry *m_registry = nullptr;
    HttpClient *m_http = nullptr;
    QVector<SourceConfig> m_stagedSources;
    bool m_sourcesDirty = false;

    QHash<QString, QString> m_stagedShortcuts;
    bool m_shortcutsDirty = false;

    QComboBox *m_theme = nullptr;
    QComboBox *m_fontSize = nullptr;
    QComboBox *m_windowSize = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_volumeValue = nullptr;
    QComboBox *m_searchLimit = nullptr;
    QComboBox *m_downloadLimit = nullptr;
    QLineEdit *m_downloadDir = nullptr;
    QPushButton *m_browseDownloadDir = nullptr;
    QCheckBox *m_removePlaybackCache = nullptr;
    QCheckBox *m_sidebarAutoHide = nullptr;
    QCheckBox *m_controlsAutoHide = nullptr;
    QLabel *m_sourcesLabel = nullptr;
    QPushButton *m_sourcesButton = nullptr;
};

#endif // SETTINGSDIALOG_H
