/// @file SourceParserSettingsDialog.cpp
/// @brief 外部解析器配置窗口的实现：命令、参数模板、超时与协议说明。
#include "ui/SourceParserSettingsDialog.h"

#include "ui/ThemeManager.h"
#include "ui_SourceParserSettingsDialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>

SourceParserSettingsDialog::SourceParserSettingsDialog(
    const SourceParserSettings &settings, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SourceParserSettingsDialog)
{
    ui->setupUi(this);
    setModal(true);
    setWindowTitle(tr("外部解析器配置"));

    ui->edit_command->setText(settings.command);
    ui->edit_arguments->setText(settings.arguments);
    ui->spin_timeout->setValue(qBound(5000, settings.timeoutMs, 600000));

    // 卡片用的是统一 settingsCard 样式，显式开启 styled background 才会绘制。
    ui->card_protocol->setAttribute(Qt::WA_StyledBackground, true);
    ui->card_protocol->style()->unpolish(ui->card_protocol);
    ui->card_protocol->style()->polish(ui->card_protocol);

    connect(ui->btn_browse, &QPushButton::clicked, this,
            &SourceParserSettingsDialog::browseCommand);
    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->btn_save, &QPushButton::clicked, this, &QDialog::accept);

    ThemeManager::instance().applyWindowChrome(this);
}

SourceParserSettingsDialog::~SourceParserSettingsDialog()
{
    delete ui;
}

SourceParserSettings SourceParserSettingsDialog::parserSettings() const
{
    SourceParserSettings settings;
    settings.command = ui->edit_command->text().trimmed();
    settings.arguments = ui->edit_arguments->text().trimmed();
    settings.timeoutMs = qBound(5000, ui->spin_timeout->value(), 600000);
    return settings;
}

void SourceParserSettingsDialog::browseCommand()
{
    const QString current = ui->edit_command->text().trimmed();
    const QString startDir = current.isEmpty()
        ? QCoreApplication::applicationDirPath()
        : QFileInfo(current).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择外部解析器"), startDir,
        tr("可执行文件 (*.exe);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    ui->edit_command->setText(QDir::toNativeSeparators(path));
}

void SourceParserSettingsDialog::accept()
{
    if (ui->edit_command->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("缺少解析器"),
                             tr("请先填写外部解析器命令，或选择可执行文件。"));
        return;
    }
    QDialog::accept();
}
