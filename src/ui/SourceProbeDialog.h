#ifndef SOURCEPROBEDIALOG_H
#define SOURCEPROBEDIALOG_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QByteArray>
#include <QDialog>
#include <QProcess>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class SourceProbeDialog;
}
QT_END_NAMESPACE

/// 新增/更新源时调用的外部解析器配置。
struct SourceParserSettings
{
    QString command;
    QString arguments;
    int timeoutMs = 120000;
};

/// “外部解析器探测”窗口：输入网站 → 调用配置的解析器 → 成功/失败 →
/// 用户确认后返回 SourceConfig。主程序不内置探测规则，只消费解析器 stdout。
class SourceProbeDialog : public QDialog
{
    Q_OBJECT

public:
    SourceProbeDialog(MediaType type, const QString &initialUrl, const QString &titleText,
                      const SourceParserSettings &parser, QWidget *parent = nullptr);
    ~SourceProbeDialog() override;

    SourceConfig probedConfig() const { return m_config; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void bindUi();
    void startProbe();
    void stopProcess();
    void readOutput();
    void finishProbe();
    void onProcessError(QProcess::ProcessError error);
    void showFailure(const QString &error, const QString &detail = QString());
    void showSuccess(const SourceConfig &config, const QJsonObject &root);
    MediaType currentType() const;
    QString reasonFromJson(const QJsonObject &root) const;
    SourceConfig configFromJsonObject(const QJsonObject &object, bool *ok) const;

    Ui::SourceProbeDialog *ui;
    SourceParserSettings m_parser;
    SourceConfig m_config;
    bool m_hasResult = false;
    bool m_busy = false;
    bool m_outputOverflow = false;

    QLineEdit *m_url = nullptr;
    QComboBox *m_type = nullptr;
    QPushButton *m_btnProbe = nullptr;
    QPushButton *m_btnSave = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_parserLabel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLabel *m_typeLabel = nullptr;
    QLabel *m_urlLabel = nullptr;
    QLabel *m_kindLabel = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;
    QByteArray m_output;
    QByteArray m_stderr;
};

#endif // SOURCEPROBEDIALOG_H
