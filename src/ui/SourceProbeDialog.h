#ifndef SOURCEPROBEDIALOG_H
#define SOURCEPROBEDIALOG_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QByteArray>
#include <QDialog>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
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

/// “外部解析器探测”窗口：支持一行一个站点批量探测，成功的行可编辑名称并勾选，
/// 确认后返回 SourceConfig 列表。主程序不内置探测规则，只消费解析器 stdout。
class SourceProbeDialog : public QDialog
{
    Q_OBJECT

public:
    SourceProbeDialog(MediaType type, const QString &initialUrl, const QString &titleText,
                      const SourceParserSettings &parser, QWidget *parent = nullptr,
                      const QString &mode = QStringLiteral("add"));
    ~SourceProbeDialog() override;

    QVector<SourceConfig> probedConfigs() const { return m_configs; }

protected:
    void accept() override;
    void showEvent(QShowEvent *event) override;

private:
    void bindUi();
    void startProbe();
    void startNextSite();
    void startSiteProbe(const QUrl &site);
    void finishCurrentProbe();
    void stopProcess();
    void readOutput();
    void onProcessError(QProcess::ProcessError error);
    void showFailure(const QString &error, const QString &detail = QString());
    void onTimeout();
    void updateResultActions();
    void setRowStatus(int row, const QString &status, const QString &detail = QString());
    MediaType currentType() const;
    QString reasonFromJson(const QJsonObject &root) const;
    SourceConfig configFromJsonObject(const QJsonObject &object, bool *ok) const;
    int addResultRow(const QString &siteText, const QString &status);

    Ui::SourceProbeDialog *ui;
    SourceParserSettings m_parser;
    QString m_mode;
    QVector<SourceConfig> m_configs;
    /// 每个结果行对应的成功配置；失败行不写入。
    QVector<int> m_siteRows;
    QVector<SourceConfig> m_rowConfigs;
    QVector<QUrl> m_sites;
    int m_nextSite = 0;
    bool m_busy = false;
    bool m_outputOverflow = false;
    bool m_updating = false;

    QPlainTextEdit *m_sitesEdit = nullptr;
    QComboBox *m_type = nullptr;
    QPushButton *m_btnProbe = nullptr;
    QPushButton *m_btnSave = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_parserLabel = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;
    QByteArray m_output;
    QByteArray m_stderr;
};

#endif // SOURCEPROBEDIALOG_H
