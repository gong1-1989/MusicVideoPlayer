#ifndef SOURCEPARSERSETTINGSDIALOG_H
#define SOURCEPARSERSETTINGSDIALOG_H

#include "ui/SourceProbeDialog.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QSpinBox;

QT_BEGIN_NAMESPACE
namespace Ui {
class SourceParserSettingsDialog;
}
QT_END_NAMESPACE

/// 外部解析器配置窗口：命令、参数模板、单站点超时，以及占位符/协议说明。
class SourceParserSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SourceParserSettingsDialog(const SourceParserSettings &settings,
                                        QWidget *parent = nullptr);
    ~SourceParserSettingsDialog() override;

    SourceParserSettings parserSettings() const;

protected:
    void accept() override;

private slots:
    void browseCommand();

private:
    Ui::SourceParserSettingsDialog *ui;
};

#endif // SOURCEPARSERSETTINGSDIALOG_H
