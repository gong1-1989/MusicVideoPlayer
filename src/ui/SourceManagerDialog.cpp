/// @file SourceManagerDialog.cpp
/// @brief 搜索源列表、新增/更新与外部解析器配置。
#include "ui/SourceManagerDialog.h"

#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "ui/SourceProbeDialog.h"
#include "ui/ThemeManager.h"
#include "ui_SourceManagerDialog.h"

#include <QAction>
#include <QDateTime>
#include <QGuiApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSpinBox>
#include <QUrl>

SourceManagerDialog::SourceManagerDialog(SourceRegistry *registry, QWidget *parent,
                                         bool deferCommit)
    : QDialog(parent)
    , ui(new Ui::SourceManagerDialog)
    , m_registry(registry)
    , m_deferCommit(deferCommit)
{
    ui->setupUi(this);
    setModal(true);

    const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                : QRect(0, 0, 1280, 800);
    setMaximumSize(available.width(), available.height() - 20);
    resize(qMin(760, available.width() - 40), qMin(600, available.height() - 60));
    if (available.height() <= 800) {
        ui->layout_root->setContentsMargins(12, 8, 12, 6);
        ui->layout_root->setSpacing(6);
        ui->list_sources->setMinimumHeight(140);
    }

    if (m_registry != nullptr)
        m_drafts = m_registry->sources();

    bindUi();
    reloadList(m_drafts.isEmpty() ? -1 : 0);
    ThemeManager::instance().applyWindowChrome(this);
}

SourceManagerDialog::~SourceManagerDialog()
{
    delete ui;
}

void SourceManagerDialog::bindUi()
{
    m_list = ui->list_sources;
    m_command = ui->edit_command;
    m_arguments = ui->edit_arguments;
    m_timeout = ui->spin_timeout;
    m_btnAdd = ui->btn_add;
    m_btnRefresh = ui->btn_refresh;

    m_command->setText(AppSettings::sourceParserCommand());
    m_arguments->setText(AppSettings::sourceParserArguments());
    m_timeout->setRange(5000, 600000);
    m_timeout->setSingleStep(5000);
    m_timeout->setValue(AppSettings::sourceParserTimeoutMs());

    connect(m_btnAdd, &QPushButton::clicked, this, &SourceManagerDialog::onAdd);
    connect(m_btnRefresh, &QPushButton::clicked, this,
            &SourceManagerDialog::onRefreshSelected);
    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->btn_save, &QPushButton::clicked, this, &SourceManagerDialog::accept);
    connect(m_list, &QListWidget::currentRowChanged, this,
            &SourceManagerDialog::selectRow);
    connect(m_list, &QListWidget::itemChanged, this,
            &SourceManagerDialog::onItemChanged);
    connect(m_list, &QListWidget::customContextMenuRequested, this,
            &SourceManagerDialog::onListContextMenu);
}

void SourceManagerDialog::setDraftSources(const QVector<SourceConfig> &sources)
{
    m_drafts = sources;
    reloadList(m_drafts.isEmpty() ? -1 : 0);
}

void SourceManagerDialog::reloadList(int targetRow)
{
    m_updating = true;
    m_list->clear();
    for (const SourceConfig &config : m_drafts) {
        QStringList tags;
        if (config.builtin)
            tags.append(tr("内置"));
        tags.append(config.mediaType == MediaType::Video ? tr("视频") : tr("音乐"));
        const QString text = QStringLiteral("%1  ·  %2")
                                 .arg(config.name, tags.join(QStringLiteral(" / ")));
        QListWidgetItem *item = new QListWidgetItem(text, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(config.enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, config.id);
        QString tooltip = QStringLiteral("%1\n%2")
                              .arg(config.id, config.kindLabel());
        if (!config.urlTemplate.isEmpty())
            tooltip += QLatin1Char('\n') + config.urlTemplate;
        item->setToolTip(tooltip);
    }
    m_updating = false;

    if (targetRow >= 0 && targetRow < m_list->count())
        m_list->setCurrentRow(targetRow);
    else if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateActions();
}

void SourceManagerDialog::selectRow(int)
{
    updateActions();
}

int SourceManagerDialog::currentIndex() const
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_drafts.size())
        return -1;
    return row;
}

void SourceManagerDialog::updateActions()
{
    const int index = currentIndex();
    const bool canRefresh = index >= 0 && !m_drafts.at(index).builtin;
    m_btnRefresh->setEnabled(canRefresh);
    m_btnRefresh->setToolTip(canRefresh
                                 ? tr("重新调用外部解析器探测选中源，确认后覆盖配置")
                                 : tr("内置源由程序维护，不能通过解析器覆盖"));
}

void SourceManagerDialog::onItemChanged(QListWidgetItem *item)
{
    if (m_updating || item == nullptr)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    int index = -1;
    for (int i = 0; i < m_drafts.size(); ++i) {
        if (m_drafts.at(i).id == id) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;

    const bool wantsEnabled = item->checkState() == Qt::Checked;
    if (wantsEnabled && !m_drafts.at(index).enabled
        && m_drafts.at(index).kind == QLatin1String("external")) {
        const auto answer = QMessageBox::question(
            this, tr("启用外部解析器源"),
            tr("启用后，搜索时会在本机执行这个源配置的外部命令。确认启用“%1”吗？")
                .arg(m_drafts.at(index).name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            m_updating = true;
            item->setCheckState(Qt::Unchecked);
            m_updating = false;
            return;
        }
    }
    if (m_drafts.at(index).enabled == wantsEnabled)
        return;
    m_drafts[index].enabled = wantsEnabled;
    ui->label_status->setText(tr("已修改启用状态，保存后生效。"));
}

void SourceManagerDialog::onAdd()
{
    if (m_command->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("缺少解析器"),
                             tr("请先填写外部解析器命令，再新增源。"));
        return;
    }

    SourceProbeDialog dialog(MediaType::Music, QString(), tr("新增搜索源"),
                             parserSettings(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    SourceConfig config = dialog.probedConfig();
    config.id = uniqueId(config.id);
    config.enabled = true;
    config.builtin = false;
    m_drafts.append(config);
    reloadList(m_drafts.size() - 1);
    ui->label_status->setText(tr("已添加“%1”，保存后生效。").arg(config.name));
}

void SourceManagerDialog::onRefreshSelected()
{
    const int index = currentIndex();
    if (index < 0)
        return;
    if (m_drafts.at(index).builtin) {
        QMessageBox::information(this, tr("内置源"),
                                 tr("内置源由程序维护，不能通过外部解析器覆盖。"));
        return;
    }
    if (m_command->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("缺少解析器"),
                             tr("请先填写外部解析器命令，再更新源。"));
        return;
    }

    const SourceConfig existing = m_drafts.at(index);
    QUrl site(existing.urlTemplate);
    QString initialUrl;
    if (site.isValid() && !site.host().isEmpty())
        initialUrl = site.scheme() + QStringLiteral("://") + site.host()
            + QStringLiteral("/");

    SourceProbeDialog dialog(existing.mediaType, initialUrl,
                             tr("更新搜索源：%1").arg(existing.name),
                             parserSettings(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    SourceConfig config = dialog.probedConfig();
    config.id = existing.id;
    config.enabled = existing.enabled;
    config.builtin = false;
    m_drafts[index] = config;
    reloadList(index);
    ui->label_status->setText(tr("已更新“%1”，保存后生效。").arg(config.name));
}

void SourceManagerDialog::onRemoveSelected()
{
    const int index = currentIndex();
    if (index < 0)
        return;
    if (m_drafts.at(index).builtin) {
        QMessageBox::information(this, tr("内置源"), tr("内置源不能删除。"));
        return;
    }
    if (QMessageBox::question(this, tr("删除搜索源"),
                              tr("确定删除“%1”吗？").arg(m_drafts.at(index).name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
    m_drafts.removeAt(index);
    reloadList(qMin(index, m_drafts.size() - 1));
    ui->label_status->setText(tr("已删除源，保存后生效。"));
}

void SourceManagerDialog::onListContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (item != nullptr)
        m_list->setCurrentItem(item);
    const int index = currentIndex();
    QMenu menu(this);
    QAction *removeAction = menu.addAction(tr("删除所选源"));
    removeAction->setEnabled(index >= 0 && !m_drafts.at(index).builtin);
    connect(removeAction, &QAction::triggered, this, &SourceManagerDialog::onRemoveSelected);
    menu.exec(m_list->mapToGlobal(pos));
}

QString SourceManagerDialog::uniqueId(const QString &preferred) const
{
    QString base = preferred.trimmed().toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9_\\-]+")), QStringLiteral("_"));
    while (base.startsWith(QLatin1Char('_')))
        base.remove(0, 1);
    while (base.endsWith(QLatin1Char('_')))
        base.chop(1);
    if (base.isEmpty())
        base = QStringLiteral("source");

    auto taken = [this](const QString &candidate) {
        for (const SourceConfig &config : m_drafts) {
            if (config.id == candidate)
                return true;
        }
        return false;
    };
    if (!taken(base))
        return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const QString candidate = base + QLatin1Char('_') + QString::number(suffix);
        if (!taken(candidate))
            return candidate;
    }
    return base + QStringLiteral("_") + QString::number(QDateTime::currentMSecsSinceEpoch());
}

SourceParserSettings SourceManagerDialog::parserSettings() const
{
    SourceParserSettings settings;
    settings.command = m_command->text().trimmed();
    settings.arguments = m_arguments->text().trimmed();
    settings.timeoutMs = m_timeout->value();
    return settings;
}

void SourceManagerDialog::saveParserSettings() const
{
    AppSettings::setSourceParserCommand(m_command->text().trimmed());
    AppSettings::setSourceParserArguments(m_arguments->text().trimmed());
    AppSettings::setSourceParserTimeoutMs(m_timeout->value());
}

bool SourceManagerDialog::saveAll()
{
    if (m_deferCommit) {
        m_committedSources = m_drafts;
        return true;
    }
    if (m_registry == nullptr) {
        m_committedSources = m_drafts;
        return true;
    }
    if (!m_registry->replaceAll(m_drafts)) {
        QMessageBox::warning(this, tr("保存失败"), m_registry->lastError());
        return false;
    }
    m_committedSources = m_registry->sources();
    return true;
}

void SourceManagerDialog::accept()
{
    saveParserSettings();
    if (!saveAll())
        return;
    QDialog::accept();
}
