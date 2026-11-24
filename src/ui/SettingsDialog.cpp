/// @file SettingsDialog.cpp
/// @brief 系统设置页的控件绑定、草稿提交与保存流程实现。
#include "ui/SettingsDialog.h"

#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "settings/AppShortcuts.h"
#include "settings/WindowPreset.h"
#include "ui/ShortcutSettingsDialog.h"
#include "ui/SourceManagerDialog.h"
#include "ui/ThemeManager.h"
#include "ui_SettingsDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QTimer>

Q_LOGGING_CATEGORY(lcSettings, "mvp.settings")

/// @brief 构造设置页：初始化卡片样式、按屏幕尺寸设限、绑定并载入当前值。
/// @param registry 源注册表，可为空；为空时源管理入口禁用。
/// @param http 共享 HTTP 客户端，源管理窗口试搜用。
/// @param parent 父窗口。
SettingsDialog::SettingsDialog(SourceRegistry *registry, HttpClient *http, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
    , m_registry(registry)
    , m_http(http)
{
    ui->setupUi(this);
    // 卡片在滚动容器里时，半透明背景需要显式开启 styled background 才会绘制
    QFrame *cards[] = {ui->card_appearance, ui->card_layout, ui->card_window,
                       ui->card_behavior, ui->card_sources};
    for (QFrame *card : cards) {
        card->setAttribute(Qt::WA_StyledBackground, true);
        // setWidget 会在 retranslateUi 之前先 polish 子控件，此时对象名还是旧值。
        // 重新 polish 一次，让 QFrame#settingsCard 的全局样式真正命中。
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    }

    setWindowTitle(tr("系统设置"));
    setModal(true);
    // 版本号以 main.cpp 的 setApplicationVersion 为准，避免关于卡和构建配置各写一份。
    if (!QCoreApplication::applicationVersion().isEmpty())
        ui->label_aboutVersion->setText(QCoreApplication::applicationVersion());

    const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                : QRect(0, 0, 1280, 800);
    // 720 是舒适宽度，不是硬下限；窄屏上必须让窗口还能完整落在屏幕内。
    setMinimumWidth(qMin(720, qMax(320, available.width() - 20)));
    // 滚动区允许被压缩到 0，否则布局最小高度会把窗口顶出屏幕，底部按钮再次不可达
    ui->scroll_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    ui->scroll_content->setMinimumHeight(0);
    // 滚动区独占根布局的剩余高度，标题保持贴顶，不被拉成居中
    ui->layout_root->setStretch(1, 1);
    setMaximumSize(available.width(), available.height() - 20);
    resize(qMin(820, available.width() - 40), qMin(620, available.height() - 60));
    m_columns = ui->layout_columns;
    updateColumnLayout();

    // 首次打开时焦点落在首个输入控件会把滚动区带到中间，显示时强制回到顶部
    QTimer::singleShot(0, this, [this] {
        ui->scroll_content->verticalScrollBar()->setValue(0);
    });

    bindUi();
    loadValues();

    // 模态窗口的原生标题栏同样要跟着主题
    ThemeManager::instance().applyWindowChrome(this);
}

/// @brief 销毁 UI；子窗口由 Qt 父子关系回收。
SettingsDialog::~SettingsDialog()
{
    delete ui;
}

/// @brief 窗口尺寸变化后重新评估单列/双列布局。
void SettingsDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    updateColumnLayout();
}

/// @brief 根据字号和窗口宽度决定左右双列或上下单列。
/// @note 大字号/窄窗口下双列最小宽度会超出滚动区，必须改成单列才能滚到全部设置。
void SettingsDialog::updateColumnLayout()
{
    if (m_columns == nullptr)
        return;

    // 150% 字体时左右两列的总最小宽度会超过滚动区，右列被裁掉；
    // 窄窗口同理。这里改成上下铺开，让全部设置都能纵向滚动到。
    const bool single = AppSettings::uiFontSizePx() > 13 || width() < 760;
    if (single == m_singleColumn)
        return;

    m_singleColumn = single;
    m_columns->setDirection(single ? QBoxLayout::TopToBottom
                                   : QBoxLayout::LeftToRight);
    if (single) {
        ui->spacer_left->changeSize(0, 0, QSizePolicy::Minimum,
                                    QSizePolicy::Minimum);
    } else {
        ui->spacer_left->changeSize(20, 40, QSizePolicy::Minimum,
                                    QSizePolicy::Expanding);
    }
    m_columns->invalidate();
}

/// @brief 绑定所有设置控件与按钮信号。
void SettingsDialog::bindUi()
{
    m_theme = ui->combo_theme;
    m_fontSize = ui->combo_fontSize;
    m_windowSize = ui->combo_windowSize;
    m_volume = ui->slider_volume;
    m_volumeValue = ui->label_volumeValue;
    m_searchLimit = ui->combo_searchLimit;
    m_downloadLimit = ui->combo_downloadLimit;
    m_downloadDir = ui->edit_downloadDir;
    m_browseDownloadDir = ui->btn_browseDownloadDir;
    m_removePlaybackCache = ui->checkbox_removePlaybackCache;
    m_sidebarAutoHide = ui->checkbox_sidebar;
    m_controlsAutoHide = ui->checkbox_controls;
    m_sourcesLabel = ui->label_sources;
    m_sourcesButton = ui->btn_sources;

    // ---------------------------------------------------------- 外观
    for (const ThemeInfo &theme : ThemeManager::instance().themes())
        m_theme->addItem(theme.name, theme.id);

    for (int pixels : {10, 11, 12, 13, 14, 15, 16, 18, 20})
        m_fontSize->addItem(tr("%1 px").arg(pixels), pixels);
    m_fontSize->setToolTip(
        tr("真实像素字号，默认 13 px；列表、设置页和音乐动感视图都会一起调整。"));

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
        tr("所有搜索源都按这里的条数抓取；条数越大，单源耗时越长。界面提供 10/20/30/50 条，"
           "配置文件里的其它值会作为“当前配置”保留。"));

    for (int limitMb : {2048, 4096, 8192, 16384}) {
        const QString label = limitMb >= 1024
            ? tr("%1 GB").arg(limitMb / 1024)
            : tr("%1 MB").arg(limitMb);
        m_downloadLimit->addItem(label, limitMb);
    }
    m_downloadLimit->setToolTip(
        tr("单个文件超过该体积会停止下载并提示失败，避免异常大文件占满磁盘。"));

    m_downloadDir->setToolTip(
        tr("下载任务直接保存到这个目录，不再弹路径选择；同名文件会自动加序号。"));

    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_volumeValue->setText(QStringLiteral("%1%").arg(value));
    });
    connect(m_sourcesButton, &QPushButton::clicked, this,
            &SettingsDialog::openSourceManager);
    connect(ui->btn_shortcuts, &QPushButton::clicked, this,
            &SettingsDialog::openShortcutSettings);
    connect(m_browseDownloadDir, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择默认下载目录"), m_downloadDir->text());
        if (!dir.isEmpty()) {
            m_downloadDir->setText(QDir::cleanPath(dir));
            m_downloadDir->setCursorPosition(0);
            m_downloadDir->setToolTip(
                tr("当前目录：%1\n下载任务直接保存到这个目录，不再弹路径选择；同名文件会自动加序号。")
                    .arg(QDir::cleanPath(dir)));
        }
    });
    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->btn_save, &QPushButton::clicked, this, &SettingsDialog::accept);
}

/// @brief 把当前配置载入控件。
/// @note 遇到不在预设列表里的历史值会临时追加“当前配置”项，避免保存时被静默改掉。
void SettingsDialog::loadValues()
{    const int themeIndex = m_theme->findData(AppSettings::themeId());
    if (themeIndex >= 0)
        m_theme->setCurrentIndex(themeIndex);

    const int fontSize = AppSettings::uiFontSizePx();
    int fontIndex = m_fontSize->findData(fontSize);
    if (fontIndex < 0) {
        m_fontSize->addItem(tr("%1 px（当前配置）").arg(fontSize), fontSize);
        fontIndex = m_fontSize->count() - 1;
    }
    m_fontSize->setCurrentIndex(fontIndex);

    const int windowIndex = m_windowSize->findData(AppSettings::windowPresetId());
    if (windowIndex >= 0)
        m_windowSize->setCurrentIndex(windowIndex);

    m_volume->setValue(AppSettings::defaultVolume());
    m_volumeValue->setText(QStringLiteral("%1%").arg(m_volume->value()));

    const int limitIndex = m_searchLimit->findData(AppSettings::searchLimit());
    if (limitIndex >= 0) {
        m_searchLimit->setCurrentIndex(limitIndex);
    } else {
        // 兼容手工改过配置或旧版本的值：不要静默落回 10 条，
        // 否则用户一保存就把真实配置改掉了。
        const int current = AppSettings::searchLimit();
        m_searchLimit->addItem(tr("%1 条（当前配置）").arg(current), current);
        m_searchLimit->setCurrentIndex(m_searchLimit->count() - 1);
    }

    const int downloadLimit = AppSettings::downloadLimitMb();
    int downloadIndex = m_downloadLimit->findData(downloadLimit);
    if (downloadIndex < 0) {
        const QString label = (downloadLimit >= 1024 && downloadLimit % 1024 == 0)
            ? tr("%1 GB（当前配置）").arg(downloadLimit / 1024)
            : tr("%1 MB（当前配置）").arg(downloadLimit);
        m_downloadLimit->addItem(label, downloadLimit);
        downloadIndex = m_downloadLimit->count() - 1;
    }
    if (downloadIndex >= 0)
        m_downloadLimit->setCurrentIndex(downloadIndex);

    m_downloadDir->setText(AppSettings::downloadDir());
    m_downloadDir->setCursorPosition(0);
    m_downloadDir->setToolTip(
        tr("当前目录：%1\n下载任务直接保存到这个目录，不再弹路径选择；同名文件会自动加序号。")
            .arg(AppSettings::downloadDir()));

    m_removePlaybackCache->setChecked(AppSettings::removePlaybackCacheAfterDownload());
    m_removePlaybackCache->setToolTip(
        tr("默认开启。删除 Data/media-cache 中该条目的播放缓存，同时删除 "
           "Data/lyrics-cache 里的对应歌词；下载文件本身不受影响。"));

    m_sidebarAutoHide->setChecked(AppSettings::sidebarAutoHide());
    m_controlsAutoHide->setChecked(AppSettings::controlsAutoHide());

    refreshSourcesLabel();
}

/// @brief 刷新“已启用 x 个源（共 y 个）”标签和悬停说明。
void SettingsDialog::refreshSourcesLabel()
{
    if (m_sourcesLabel == nullptr)
        return;
    if (m_registry == nullptr) {
        m_sourcesLabel->setText(tr("未连接源仓库"));
        m_sourcesButton->setEnabled(false);
        return;
    }

    const QVector<SourceConfig> sources =
        m_sourcesDirty ? m_stagedSources : m_registry->sources();
    int enabled = 0;
    for (const SourceConfig &config : sources) {
        if (config.enabled && config.isValid())
            ++enabled;
    }
    m_sourcesLabel->setText(tr("已启用 %1 个源（共 %2 个）").arg(enabled).arg(sources.size()));
    m_sourcesButton->setToolTip(
        tr("源管理里的“保存”只改本页草稿；点本页“保存”才写入 sources.json，点“取消”会丢弃。"));
}

/// @brief 打开源管理窗口，只接收草稿，不立即写盘。
void SettingsDialog::openSourceManager()
{
    if (m_registry == nullptr)
        return;

    // 延迟提交：源管理只返回草稿，设置页确认后才真正写盘，取消可整体回滚
    SourceManagerDialog dialog(m_registry, this, true);
    if (m_sourcesDirty)
        dialog.setDraftSources(m_stagedSources);
    if (dialog.exec() == QDialog::Accepted) {
        m_stagedSources = dialog.committedSources();
        m_sourcesDirty = true;
        refreshSourcesLabel();
    }
}

/// @brief 打开快捷键设置窗口，只接收草稿绑定。
void SettingsDialog::openShortcutSettings()
{
    // 和搜索源一样只改本页草稿；系统设置页“保存”才真正写 QSettings，
    // 点“取消”时快捷键修改也会一起丢弃。
    const QHash<QString, QString> draft =
        m_shortcutsDirty ? m_stagedShortcuts : AppShortcuts::currentBindings();
    ShortcutSettingsDialog dialog(draft, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_stagedShortcuts = dialog.bindings();
        m_shortcutsDirty = true;
    }
}

/// @brief 确认保存：先创建/校验下载目录，再提交源配置、设置与快捷键。
/// @note 下载目录是唯一可能失败的外部副作用，必须先校验，避免先写源/音量后
///       目录失败导致无法整体回滚；源配置写失败则停留在本页。
void SettingsDialog::accept()
{
    // 先校验唯一可能失败的外部副作用（下载目录），再提交源配置和设置。
    // 目录失败时不会留下已写一半的源/音量设置。
    QString downloadDir = m_downloadDir->text().trimmed();
    if (downloadDir.isEmpty())
        downloadDir = AppSettings::downloadDir();
    QDir dir(downloadDir);
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        QMessageBox::warning(
            this, tr("下载目录不可用"),
            tr("无法创建目录：\n%1\n\n请换一个可写目录。").arg(dir.absolutePath()));
        return;
    }

    // 目录校验通过后再提交源草稿；写盘失败就留在设置页，不能把其余设置当作已保存。
    if (m_sourcesDirty) {
        if (m_registry == nullptr || !m_registry->replaceAll(m_stagedSources)) {
            QMessageBox::warning(
                this, tr("保存失败"),
                m_registry == nullptr
                    ? tr("未连接源仓库，无法保存搜索源。")
                    : (m_registry->lastError().isEmpty()
                           ? tr("无法写入 sources.json，请检查 Data 目录权限。")
                           : m_registry->lastError()));
            return;
        }
        m_sourcesDirty = false;
    }

    AppSettings::setThemeId(m_theme->currentData().toString());
    AppSettings::setUiFontSizePx(m_fontSize->currentData().toInt());
    AppSettings::setWindowPresetId(m_windowSize->currentData().toString());
    AppSettings::setDefaultVolume(m_volume->value());
    AppSettings::setSearchLimit(m_searchLimit->currentData().toInt());
    AppSettings::setDownloadLimitMb(m_downloadLimit->currentData().toInt());

    AppSettings::setDownloadDir(dir.absolutePath());
    AppSettings::setRemovePlaybackCacheAfterDownload(m_removePlaybackCache->isChecked());
    AppSettings::setSidebarAutoHide(m_sidebarAutoHide->isChecked());
    AppSettings::setControlsAutoHide(m_controlsAutoHide->isChecked());
    if (m_shortcutsDirty) {
        AppShortcuts::storeBindings(m_stagedShortcuts);
        m_shortcutsDirty = false;
    }

    qCInfo(lcSettings) << "系统设置已保存：主题" << m_theme->currentData().toString()
                       << "字号" << m_fontSize->currentData().toInt() << "px"
                       << "窗口" << m_windowSize->currentData().toString() << "音量"
                       << m_volume->value() << "单源条数" << m_searchLimit->currentData().toInt()
                       << "下载上限" << m_downloadLimit->currentData().toInt() << "MB"
                       << "下载后清缓存" << m_removePlaybackCache->isChecked();
    QDialog::accept();
}
