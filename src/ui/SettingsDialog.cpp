#include "ui/SettingsDialog.h"

#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
#include "ui/ThemeManager.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QFrame *makeCard(const QString &title, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    card->setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 16);
    layout->setSpacing(10);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("label_sectionTitle"));
    layout->addWidget(titleLabel);
    return card;
}

QFormLayout *cardForm(QFrame *card)
{
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    if (auto *layout = qobject_cast<QVBoxLayout *>(card->layout()))
        layout->addLayout(form);
    return form;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("SettingsDialog"));
    setWindowTitle(tr("系统设置"));
    setModal(true);
    setMinimumWidth(520);

    buildUi();
    loadValues();
}

void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(14);

    auto *title = new QLabel(tr("系统设置"), this);
    title->setObjectName(QStringLiteral("label_sectionTitle"));
    title->setStyleSheet(QStringLiteral("font-size:16px;"));
    root->addWidget(title);

    // ---------------------------------------------------------- 外观
    QFrame *appearance = makeCard(tr("外观"), this);
    QFormLayout *appearanceForm = cardForm(appearance);

    m_theme = new QComboBox(appearance);
    for (const ThemeInfo &theme : ThemeManager::instance().themes())
        m_theme->addItem(theme.name, theme.id);
    appearanceForm->addRow(tr("主题"), m_theme);

    auto *themeHint = new QLabel(tr("切换后立即生效，无需重启。"), appearance);
    themeHint->setObjectName(QStringLiteral("label_hint"));
    appearanceForm->addRow(QString(), themeHint);
    root->addWidget(appearance);

    // ---------------------------------------------------------- 窗口
    QFrame *windowCard = makeCard(tr("窗口尺寸"), this);
    QFormLayout *windowForm = cardForm(windowCard);

    m_windowSize = new QComboBox(windowCard);
    int presetCount = 0;
    const WindowPresetEntry *presets = windowPresetTable(&presetCount);
    for (int i = 0; i < presetCount; ++i) {
        const WindowPresetEntry &entry = presets[i];
        const QString label =
            entry.preset == WindowPreset::Last
                ? tr("记住上次关闭时的大小")
                : tr("%1 × %2").arg(entry.size.width()).arg(entry.size.height());
        m_windowSize->addItem(label, QString::fromLatin1(entry.id));
    }
    windowForm->addRow(tr("启动尺寸"), m_windowSize);

    auto *windowHint = new QLabel(tr("选固定尺寸后，下次启动按该尺寸打开窗口。"), windowCard);
    windowHint->setObjectName(QStringLiteral("label_hint"));
    windowForm->addRow(QString(), windowHint);
    root->addWidget(windowCard);

    // ---------------------------------------------------------- 播放与搜索
    QFrame *behavior = makeCard(tr("播放与搜索"), this);
    QFormLayout *behaviorForm = cardForm(behavior);

    auto *volumeRow = new QWidget(behavior);
    auto *volumeLayout = new QHBoxLayout(volumeRow);
    volumeLayout->setContentsMargins(0, 0, 0, 0);
    volumeLayout->setSpacing(10);
    m_volume = new QSlider(Qt::Horizontal, volumeRow);
    m_volume->setRange(0, 100);
    m_volume->setMinimumWidth(180);
    m_volumeValue = new QLabel(volumeRow);
    m_volumeValue->setMinimumWidth(36);
    m_volumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeLayout->addWidget(m_volume, 1);
    volumeLayout->addWidget(m_volumeValue);
    behaviorForm->addRow(tr("启动音量"), volumeRow);

    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_volumeValue->setText(QStringLiteral("%1%").arg(value));
    });

    m_searchLimit = new QComboBox(behavior);
    for (int limit : {10, 20, 30, 50})
        m_searchLimit->addItem(tr("%1 条").arg(limit), limit);
    behaviorForm->addRow(tr("单次搜索条数"), m_searchLimit);

    root->addWidget(behavior);
    root->addStretch(1);

    // ---------------------------------------------------------- 按钮
    auto *buttons = new QDialogButtonBox(this);
    auto *cancelButton = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    auto *saveButton = buttons->addButton(tr("保存"), QDialogButtonBox::AcceptRole);
    saveButton->setObjectName(QStringLiteral("btn_applyPrimary"));
    saveButton->setDefault(true);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, &SettingsDialog::accept);
    root->addWidget(buttons);
}

void SettingsDialog::loadValues()
{
    const int themeIndex = m_theme->findData(AppSettings::themeId());
    if (themeIndex >= 0)
        m_theme->setCurrentIndex(themeIndex);

    const int windowIndex = m_windowSize->findData(AppSettings::windowPresetId());
    if (windowIndex >= 0)
        m_windowSize->setCurrentIndex(windowIndex);

    m_volume->setValue(AppSettings::defaultVolume());
    m_volumeValue->setText(QStringLiteral("%1%").arg(m_volume->value()));

    const int limitIndex = m_searchLimit->findData(AppSettings::searchLimit());
    if (limitIndex >= 0)
        m_searchLimit->setCurrentIndex(limitIndex);
}

void SettingsDialog::accept()
{
    AppSettings::setThemeId(m_theme->currentData().toString());
    AppSettings::setWindowPresetId(m_windowSize->currentData().toString());
    AppSettings::setDefaultVolume(m_volume->value());
    AppSettings::setSearchLimit(m_searchLimit->currentData().toInt());

    QDialog::accept();
}
