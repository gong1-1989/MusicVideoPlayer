#ifndef SOURCEPROBEDIALOG_H
#define SOURCEPROBEDIALOG_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"
#include "net/SourceProbe.h"

#include <QDialog>

class HttpClient;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;

QT_BEGIN_NAMESPACE
namespace Ui {
class SourceProbeDialog;
}
QT_END_NAMESPACE

/// “从网站添加 / 一键更新”的探测窗口：输入网址 → 选类型 → 探测 →
/// 展示推断出来的配置和试搜样例 → 用户确认后才返回配置。
/// 探测失败只提示原因、不允许保存。界面由 SourceProbeDialog.ui 描述。
class SourceProbeDialog : public QDialog
{
    Q_OBJECT

public:
    SourceProbeDialog(HttpClient *http, MediaType type, const QString &initialUrl,
                      const QString &titleText, QWidget *parent = nullptr);
    ~SourceProbeDialog() override;

    SourceConfig probedConfig() const { return m_result.config; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void bindUi();
    void startProbe();
    void onProbeFinished(const SourceProbe::Result &result);
    void reloadPathList();
    void addCustomPath();
    void removeCustomPath();
    QStringList customPaths() const;
    MediaType currentType() const;

    Ui::SourceProbeDialog *ui;
    HttpClient *m_http;
    SourceProbe::Prober *m_prober = nullptr;
    SourceProbe::Result m_result;
    bool m_busy = false;

    QLineEdit *m_url = nullptr;
    QComboBox *m_type = nullptr;
    QPushButton *m_btnProbe = nullptr;
    QLabel *m_status = nullptr;

    QLabel *m_nameLabel = nullptr;
    QLabel *m_interfaceLabel = nullptr;
    QLabel *m_pathLabel = nullptr;
    QLabel *m_fieldsLabel = nullptr;
    QLabel *m_kindLabel = nullptr;
    QListWidget *m_samples = nullptr;
    QPlainTextEdit *m_attempts = nullptr;
    QPlainTextEdit *m_error = nullptr;

    QListWidget *m_paths = nullptr;
    QLineEdit *m_customPath = nullptr;

    QPushButton *m_btnSave = nullptr;
};

#endif // SOURCEPROBEDIALOG_H
