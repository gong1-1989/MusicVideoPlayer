#include "ui/ShortcutSettingsDialog.h"

#include "settings/AppShortcuts.h"
#include "ui/ThemeManager.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

ShortcutSettingsDialog::ShortcutSettingsDialog(const QHash<QString, QString> &bindings,
                                               QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("快捷键设置"));
    setObjectName(QStringLiteral("ShortcutSettingsDialog"));
    setModal(true);
    resize(760, 600);
    setMinimumSize(560, 420);

    m_defaults = AppShortcuts::defaultBindings();
    m_bindings = AppShortcuts::currentBindings();
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
        m_bindings.insert(it.key(), AppShortcuts::normalizeSequence(it.value()));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);

    auto *title = new QLabel(tr("自定义快捷键"), this);
    title->setObjectName(QStringLiteral("label_sectionTitle"));
    root->addWidget(title);

    auto *intro = new QLabel(
        tr("点击“快捷键”单元格后直接按下新的组合键；留空表示不绑定。"
           "播放 / 暂停、全屏 / 退出全屏是切换动作，只占一个快捷键。"),
        this);
    intro->setObjectName(QStringLiteral("label_hint"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("table_shortcuts"));
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels(
        QStringList{tr("动作"), tr("快捷键"), tr("缺省值"), tr("操作")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->setColumnWidth(1, 250);
    m_table->setColumnWidth(2, 110);
    m_table->setColumnWidth(3, 100);
    root->addWidget(m_table, 1);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("label_hint"));
    m_hint->setWordWrap(true);
    root->addWidget(m_hint);

    auto *buttons = new QHBoxLayout;
    auto *defaultsButton = new QPushButton(tr("全部恢复缺省值"), this);
    auto *cancelButton = new QPushButton(tr("取消"), this);
    auto *saveButton = new QPushButton(tr("保存"), this);
    saveButton->setDefault(true);
    buttons->addWidget(defaultsButton);
    buttons->addStretch(1);
    buttons->addWidget(cancelButton);
    buttons->addWidget(saveButton);
    root->addLayout(buttons);

    connect(defaultsButton, &QPushButton::clicked, this,
            &ShortcutSettingsDialog::resetAllToDefaults);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, &ShortcutSettingsDialog::accept);

    buildRows();
    refreshConflictHint();
    ThemeManager::instance().applyWindowChrome(this);
}

void ShortcutSettingsDialog::buildRows()
{
    const QVector<AppShortcuts::Action> &actions = AppShortcuts::actions();
    m_table->setRowCount(actions.size());

    for (int row = 0; row < actions.size(); ++row) {
        const AppShortcuts::Action &action = actions.at(row);
        m_table->setRowHeight(row, 42);

        auto *label = new QTableWidgetItem(action.label);
        if (action.toggle) {
            label->setToolTip(tr("强对立动作合并为一个切换动作，只绑定一个快捷键。"));
        }
        m_table->setItem(row, 0, label);

        auto *edit = new QKeySequenceEdit(m_table);
        edit->setMaximumSequenceLength(1);
        edit->setKeySequence(
            QKeySequence::fromString(m_bindings.value(action.id),
                                     QKeySequence::PortableText));
        auto *clearButton = new QToolButton(m_table);
        clearButton->setText(tr("清除"));
        clearButton->setToolTip(tr("取消这个动作的快捷键。"));
        connect(clearButton, &QToolButton::clicked, edit, &QKeySequenceEdit::clear);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this](const QKeySequence &) { refreshConflictHint(); });

        auto *editCell = new QWidget(m_table);
        auto *editLayout = new QHBoxLayout(editCell);
        editLayout->setContentsMargins(4, 3, 4, 3);
        editLayout->setSpacing(4);
        editLayout->addWidget(edit, 1);
        editLayout->addWidget(clearButton);
        m_table->setCellWidget(row, 1, editCell);
        m_editors.insert(action.id, edit);

        const QString defaultText = m_defaults.value(action.id);
        auto *defaultItem = new QTableWidgetItem(
            defaultText.isEmpty() ? tr("未设置") : defaultText);
        defaultItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 2, defaultItem);

        auto *resetButton = new QToolButton(m_table);
        resetButton->setText(tr("恢复"));
        resetButton->setToolTip(tr("把这个动作恢复成缺省快捷键：%1").arg(defaultText));
        connect(resetButton, &QToolButton::clicked, this, [this, action] {
            if (QKeySequenceEdit *edit = m_editors.value(action.id)) {
                edit->setKeySequence(
                    QKeySequence::fromString(m_defaults.value(action.id),
                                             QKeySequence::PortableText));
            }
        });
        auto *resetCell = new QWidget(m_table);
        auto *resetLayout = new QHBoxLayout(resetCell);
        resetLayout->setContentsMargins(4, 3, 4, 3);
        resetLayout->addWidget(resetButton);
        m_table->setCellWidget(row, 3, resetCell);
    }
}

void ShortcutSettingsDialog::resetAllToDefaults()
{
    for (const AppShortcuts::Action &action : AppShortcuts::actions()) {
        if (QKeySequenceEdit *edit = m_editors.value(action.id)) {
            edit->setKeySequence(
                QKeySequence::fromString(m_defaults.value(action.id),
                                         QKeySequence::PortableText));
        }
    }
    refreshConflictHint();
}

QHash<QString, QString> ShortcutSettingsDialog::collectBindings() const
{
    QHash<QString, QString> bindings;
    for (const AppShortcuts::Action &action : AppShortcuts::actions()) {
        QKeySequenceEdit *edit = m_editors.value(action.id);
        const QString sequence = edit != nullptr
                                     ? AppShortcuts::normalizeSequence(
                                           edit->keySequence().toString(QKeySequence::PortableText))
                                     : QString();
        bindings.insert(action.id, sequence);
    }
    return bindings;
}

bool ShortcutSettingsDialog::validateAndWarn()
{
    const QHash<QString, QString> bindings = collectBindings();
    const QVector<AppShortcuts::Conflict> duplicate =
        AppShortcuts::conflicts(bindings);
    if (duplicate.isEmpty())
        return true;

    QMessageBox::warning(
        this, tr("快捷键重复"),
        tr("同一个快捷键只能绑定一个动作。请先改掉下面这些重复项：\n\n%1\n\n"
           "播放 / 暂停、全屏 / 退出全屏已经是切换动作，不需要再分别绑定“播放”和“暂停”。")
            .arg(AppShortcuts::conflictMessage(duplicate)));
    return false;
}

void ShortcutSettingsDialog::refreshConflictHint()
{
    if (m_hint == nullptr)
        return;
    const QVector<AppShortcuts::Conflict> duplicate =
        AppShortcuts::conflicts(collectBindings());
    if (duplicate.isEmpty()) {
        m_hint->setText(tr("快捷键无重复；留空表示不绑定。"));
        m_hint->setStyleSheet(QString());
    } else {
        m_hint->setText(tr("快捷键重复：%1").arg(
            AppShortcuts::conflictMessage(duplicate).replace(QLatin1Char('\n'),
                                                             QStringLiteral("；"))));
        m_hint->setStyleSheet(QStringLiteral("color:#ff6b92;"));
    }
}

void ShortcutSettingsDialog::accept()
{
    if (!validateAndWarn())
        return;
    m_bindings = collectBindings();
    QDialog::accept();
}
