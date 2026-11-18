#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

class QComboBox;
class QLabel;
class QSlider;

/// 系统设置：外观主题、窗口尺寸、默认音量、搜索结果条数。
/// 所有可配置项都只在这里出现一次，值写入 AppSettings，调用方读回即可。
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

protected:
    void accept() override;

private:
    void buildUi();
    void loadValues();

    QComboBox *m_theme = nullptr;
    QComboBox *m_windowSize = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_volumeValue = nullptr;
    QComboBox *m_searchLimit = nullptr;
};

#endif // SETTINGSDIALOG_H
