#include "ui/SettingsDialog.h"

#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
#include "ui/SourceManagerDialog.h"
#include "ui/ThemeManager.h"
#include "ui_SettingsDialog.h"

#include <QGuiApplication>
#include <QLabel>
#include <QLoggingCategory>
#include <QPushButton>
#include <QScreen>
#include <QSlider>

Q_LOGGING_CATEGORY(lcSettings, "mvp.settings")

SettingsDialog::SettingsDialog(SourceRegistry *registry, HttpClient *http, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
    , m_registry(registry)
    , m_http(http)
{
    ui->setupUi(this);
    setWindowTitle(tr("系统设置"));
    setModal(true);
    setMinimumWidth(720);

    const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                : QRect(0, 0, 1280, 800);
    resize(qMin(820, available.width() - 40), qMin(620, available.height() - 60));

    bindUi();
    loadValues();

    // 模态窗口的原生标题栏同样要跟着主题
    ThemeManager::instance().applyWindowChrome(this);
}

SettingsDialog::~SettingsDialog()
{
    delete ui;
}

void SettingsDialog::bindUi()
{
    m_theme = ui->combo_theme;
    m_windowSize = ui->combo_windowSize;
    m_volume = ui->slider_volume;
    m_volumeValue = ui->label_volumeValue;
    m_searchLimit = ui->combo_searchLimit;
    m_sidebarAutoHide = ui->checkbox_sidebar;
    m_controlsAutoHide = ui->checkbox_controls;
    m_sourcesLabel = ui->label_sources;
    m_sourcesButton = ui->btn_sources;

    // ---------------------------------------------------------- 外观
    for (const ThemeInfo &theme : ThemeManager::instance().themes())
        m_theme->addItem(theme.name, theme.id);

    // ---------------------------------------------------------- 窗口
    int presetCount = 0;
    const WindowPresetEntry *presets = windowPresetTable(&presetCount);
    for (int i = 0; i < presetCount; ++i) {
        const WindowPresetEntry &entry = presets[i];
        QString label;
        switch (entry.preset) {
        case WindowPreset::Auto:
            label = tr("自适应屏幕（推荐）");
            break;
        case WindowPreset::P720:
            label = tr("%1 × %2（720p）").arg(entry.size.width()).arg(entry.size.height());
            break;
        case WindowPreset::P768:
            label = tr("%1 × %2（笔记本常见）").arg(entry.size.width()).arg(entry.size.height());
            break;
        case WindowPreset::P1080:
            label = tr("%1 × %2（1080p）").arg(entry.size.width()).arg(entry.size.height());
            break;
        case WindowPreset::Last:
            label = tr("记住上次关闭时的大小");
            break;
        default:
            label = tr("%1 × %2").arg(entry.size.width()).arg(entry.size.height());
            break;
        }
        m_windowSize->addItem(label, QString::fromLatin1(entry.id));
    }

    // ---------------------------------------------------------- 播放与搜索
    m_volume->setRange(0, 100);
    for (int limit : {10, 20, 30, 50})
        m_searchLimit->addItem(tr("%1 条").arg(limit), limit);
    m_searchLimit->setToolTip(
        tr("网页型影视源单次最多抓取 6 条；内置/配置型音频源按这里的条数返回。"));

    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_volumeValue->setText(QStringLiteral("%1%").arg(value));
    });
    connect(m_sourcesButton, &QPushButton::clicked, this,
            &SettingsDialog::openSourceManager);
    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->btn_save, &QPushButton::clicked, this, &SettingsDialog::accept);
}

void SettingsDialog::loadValues()
{    const int themeIndex = m_theme->findData(AppSettings::themeId());
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

    m_sidebarAutoHide->setChecked(AppSettings::sidebarAutoHide());
    m_controlsAutoHide->setChecked(AppSettings::controlsAutoHide());

    refreshSourcesLabel();
}

void SettingsDialog::refreshSourcesLabel()
{
    if (m_sourcesLabel == nullptr)
        return;
    if (m_registry == nullptr) {
        m_sourcesLabel->setText(tr("未连接源仓库"));
        m_sourcesButton->setEnabled(false);
        return;
    }
    m_sourcesLabel->setText(tr("已启用 %1 个源（共 %2 个）")
                                .arg(m_registry->enabledCount())
                                .arg(m_registry->sources().size()));
    m_sourcesButton->setToolTip(
        tr("源管理里的“保存”会立即写入 sources.json；即使本页点“取消”也不会回滚源改动。"));
}

void SettingsDialog::openSourceManager()
{
    if (m_registry == nullptr)
        return;
    SourceManagerDialog dialog(m_registry, m_http, this);
    if (dialog.exec() == QDialog::Accepted)
        refreshSourcesLabel();
}

void SettingsDialog::accept()
{
    AppSettings::setThemeId(m_theme->currentData().toString());
    AppSettings::setWindowPresetId(m_windowSize->currentData().toString());
    AppSettings::setDefaultVolume(m_volume->value());
    AppSettings::setSearchLimit(m_searchLimit->currentData().toInt());
    AppSettings::setSidebarAutoHide(m_sidebarAutoHide->isChecked());
    AppSettings::setControlsAutoHide(m_controlsAutoHide->isChecked());

    qCInfo(lcSettings) << "系统设置已保存：主题" << m_theme->currentData().toString()
                       << "窗口" << m_windowSize->currentData().toString() << "音量"
                       << m_volume->value() << "单源条数" << m_searchLimit->currentData().toInt();
    QDialog::accept();
}
