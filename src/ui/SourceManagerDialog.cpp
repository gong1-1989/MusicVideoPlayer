#include "ui/SourceManagerDialog.h"

#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SearchProviderBase.h"
#include "net/SourceRegistry.h"
#include "ui/SourceProbeDialog.h"
#include "ui/ThemeManager.h"
#include "ui_SourceManagerDialog.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QGroupBox>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QUrl>

namespace {

QString headersToText(const QHash<QString, QString> &headers)
{
    QStringList lines;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        lines.append(QStringLiteral("%1: %2").arg(it.key(), it.value()));
    return lines.join(QLatin1Char('\n'));
}

QHash<QString, QString> textToHeaders(const QString &text)
{
    QHash<QString, QString> headers;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (!key.isEmpty())
            headers.insert(key, value);
    }
    return headers;
}

QString makeUniqueId(const QString &seed, const QVector<SourceConfig> &existing)
{
    QString base = seed.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("source");
    QString candidate = base;
    int suffix = 1;
    while (true) {
        bool taken = false;
        for (const SourceConfig &config : existing) {
            if (config.id == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return candidate;
        ++suffix;
        candidate = base + QLatin1Char('_') + QString::number(suffix);
    }
}

} // namespace

SourceManagerDialog::SourceManagerDialog(SourceRegistry *registry, HttpClient *http,
                                         QWidget *parent, bool deferCommit)
    : QDialog(parent)
    , ui(new Ui::SourceManagerDialog)
    , m_registry(registry)
    , m_http(http)
    , m_deferCommit(deferCommit)
{
    ui->setupUi(this);
    setWindowTitle(tr("搜索源管理"));
    setModal(true);

    // 表单内容比小屏高时右侧表单内部自己滚动，左侧操作区保持固定可见
    const QRect available = QGuiApplication::primaryScreen()
                                ? QGuiApplication::primaryScreen()->availableGeometry()
                                : QRect(0, 0, 1280, 800);
    // 中间区域可以被压缩，底部试搜/保存/取消始终留在屏幕内
    ui->bodyWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    ui->bodyWidget->setMinimumHeight(0);
    // 中间区域吃掉剩余高度，标题和底部试搜/保存栏保持固定
    ui->layout_root->setStretch(2, 1);
    setMaximumSize(available.width(), available.height() - 20);
    resize(qMin(900, available.width() - 40), qMin(640, available.height() - 60));
    // 小屏高度紧张时收紧外边距和行距，优先保证左侧操作按钮完整可见
    if (available.height() <= 800) {
        ui->layout_root->setContentsMargins(12, 8, 12, 4);
        ui->layout_root->setSpacing(6);
        ui->list_test->setMaximumHeight(56);
    }

    if (m_registry != nullptr)
        m_drafts = m_registry->sources();

    bindUi();
    reloadList(m_drafts.isEmpty() ? -1 : 0);

    // 原生标题栏跟着主题走
    ThemeManager::instance().applyWindowChrome(this);
}

SourceManagerDialog::~SourceManagerDialog()
{
    delete ui;
}

void SourceManagerDialog::setDraftSources(const QVector<SourceConfig> &sources)
{
    // 先把编辑区从旧草稿上摘下来，否则 reloadList() 会把旧表单写回新草稿第 0 行
    m_currentRow = -1;
    loadDraftToForm(SourceConfig());
    m_drafts = sources;
    reloadList(m_drafts.isEmpty() ? -1 : 0);
}

void SourceManagerDialog::bindUi()
{
    m_list = ui->list_sources;
    // 两行紧凑按钮占高较小，列表可以保留更多可见行；常规尺寸仍会自动撑满
    m_list->setMinimumHeight(140);
    ui->layout_body->setStretch(0, 0);
    ui->layout_body->setStretch(1, 1);
    // 源列表独自吃掉左栏剩余高度，底部操作按钮固定不动
    ui->layout_left->setStretch(0, 1);
    ui->leftBox->setMinimumWidth(240);
    ui->leftBox->setMaximumWidth(320);
    ui->scroll_form->setMinimumWidth(420);
    // .ui 里给编辑区写的初始尺寸只是设计时占位，运行时必须能被滚动区压缩
    ui->rightBox->setMinimumSize(0, 0);

    // 表单标签列统一宽度、左对齐，避免不同分组之间列宽跳动
    for (QLabel *label : findChildren<QLabel *>()) {
        if (!label->objectName().startsWith(QStringLiteral("label_caption")))
            continue;
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setMinimumWidth(64);
    }
    for (QGridLayout *grid : {ui->grid_basic, ui->grid_request, ui->grid_fields,
                              ui->grid_external}) {
        grid->setColumnMinimumWidth(0, 64);
        grid->setColumnStretch(1, 1);
    }
    m_testList = ui->list_test;
    m_testKeyword = ui->edit_testKeyword;
    m_btnTest = ui->btn_test;
    m_name = ui->edit_name;
    m_type = ui->combo_type;
    m_kind = ui->combo_kind;
    m_provider = ui->combo_provider;
    m_builtinRow = ui->builtinRow;
    m_siteUrlRow = ui->siteUrlRow;
    m_siteUrl = ui->edit_siteUrl;
    m_requestGroup = ui->group_request;
    m_fieldsGroup = ui->group_fields;
    m_filterGroup = ui->group_filter;
    m_externalGroup = ui->group_external;
    m_command = ui->edit_command;
    m_arguments = ui->edit_arguments;
    m_timeout = ui->spin_timeout;
    m_method = ui->combo_method;
    m_url = ui->edit_url;
    m_body = ui->text_body;
    m_headers = ui->text_headers;
    m_cookie = ui->edit_cookie;
    m_itemsPath = ui->edit_itemsPath;
    m_idField = ui->edit_idField;
    m_titleField = ui->edit_titleField;
    m_artistField = ui->edit_artistField;
    m_durationField = ui->edit_durationField;
    m_streamField = ui->edit_streamField;
    m_lyricField = ui->edit_lyricField;
    m_webField = ui->edit_webField;
    m_posterField = ui->edit_posterField;
    m_yearField = ui->edit_yearField;
    m_areaField = ui->edit_areaField;
    m_genreField = ui->edit_genreField;
    m_descriptionField = ui->edit_descriptionField;
    m_streamKind = ui->combo_streamKind;
    m_skipIf = ui->text_skipIf;
    m_detailGroup = ui->group_detail;
    m_detailUrl = ui->edit_detailUrl;
    m_detailItemsPath = ui->edit_detailItemsPath;
    m_detailStreamField = ui->edit_detailStreamField;
    m_btnRemove = ui->btn_remove;
    m_btnDuplicate = ui->btn_duplicate;

    // ---------------------------------------------------------- 下拉项
    m_type->addItem(tr("音乐"), QStringLiteral("music"));
    m_type->addItem(tr("视频"), QStringLiteral("video"));

    m_kind->addItem(tr("配置驱动（JSON 接口）"), QStringLiteral("config"));
    m_kind->addItem(tr("内置实现（需要签名）"), QStringLiteral("builtin"));
    m_kind->addItem(tr("外部解析器进程"), QStringLiteral("external"));

    m_provider->addItem(tr("网页型影视（内置抓取）"), QStringLiteral("maccms_web"));

    m_method->addItem(QStringLiteral("GET"), QStringLiteral("GET"));
    m_method->addItem(QStringLiteral("POST"), QStringLiteral("POST"));

    m_streamKind->addItem(tr("单条直链"), QStringLiteral("direct"));
    m_streamKind->addItem(tr("剧集列表（名称$地址#名称$地址）"), QStringLiteral("episodes"));

    m_timeout->setRange(1000, 60000);
    m_timeout->setSingleStep(500);
    m_timeout->setSuffix(tr(" 毫秒"));

    // 过滤规则是 JSON，用等宽字体看着清楚
    m_skipIf->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    connect(m_kind, &QComboBox::currentIndexChanged, this,
            [this](int) { updateKindUi(); });
    connect(m_provider, &QComboBox::currentIndexChanged, this,
            [this](int) { updateKindUi(); });

    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->btn_save, &QPushButton::clicked, this, &SourceManagerDialog::accept);
    connect(ui->btn_add, &QPushButton::clicked, this, &SourceManagerDialog::onAdd);
    connect(m_btnDuplicate, &QPushButton::clicked, this, &SourceManagerDialog::onDuplicate);
    connect(m_btnRemove, &QPushButton::clicked, this, &SourceManagerDialog::onRemove);
    connect(ui->btn_import, &QPushButton::clicked, this, &SourceManagerDialog::onImport);
    connect(ui->btn_export, &QPushButton::clicked, this, &SourceManagerDialog::onExport);
    connect(ui->btn_example, &QPushButton::clicked, this, &SourceManagerDialog::onAddExample);
    connect(ui->btn_addSite, &QPushButton::clicked, this, &SourceManagerDialog::onAddFromSite);
    connect(ui->btn_refresh, &QPushButton::clicked, this,
            &SourceManagerDialog::onRefreshSelected);
    ui->btn_refresh->setToolTip(tr("配置源：重新探测接口与字段映射；"
                                   "网页型影视源：体检站点地址（含跳转）并试搜验真"));
    connect(m_btnTest, &QPushButton::clicked, this, &SourceManagerDialog::onTest);

    connect(m_list, &QListWidget::currentRowChanged, this, &SourceManagerDialog::selectRow);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_updating || item == nullptr)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        const bool wantsEnabled = item->checkState() == Qt::Checked;
        for (SourceConfig &config : m_drafts) {
            if (config.id != id)
                continue;
            // 外部解析器启用即执行本机命令，必须在勾选当场确认；
            // 否则“保存”会静默把任意命令带进下一次搜索。
            if (wantsEnabled && config.kind == QLatin1String("external")) {
                const QMessageBox::StandardButton choice = QMessageBox::warning(
                    this, tr("启用外部解析器"),
                    tr("启用后搜索会直接启动本机程序：\n\n%1 %2\n\n只在你信任这个命令时启用。")
                        .arg(config.command, config.arguments),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (choice != QMessageBox::Yes) {
                    m_updating = true;
                    item->setCheckState(Qt::Unchecked);
                    m_updating = false;
                    config.enabled = false;
                    updateActions();
                    return;
                }
            }
            config.enabled = wantsEnabled;
        }
        updateActions();
    });
}

void SourceManagerDialog::reloadList(int targetRow)
{
    m_updating = true;
    m_list->clear();
    for (const SourceConfig &config : m_drafts) {
        QString text = config.name;
        text += QStringLiteral(" · ") + config.typeLabel();
        text += QStringLiteral(" · ") + config.kindLabel();
        if (!config.isValid())
            text += tr(" · 失效");
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, config.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(config.enabled ? Qt::Checked : Qt::Unchecked);
        const QString validationError = config.validationError();
        if (!validationError.isEmpty()) {
            item->setForeground(QColor(QStringLiteral("#ff6b81")));
            item->setToolTip(tr("失效：%1").arg(validationError));
        } else {
            // 体检失败（运行期才暴露的问题：站点下线、地址换了、播放全加密）
            const QString health = m_healthIssues.value(config.id);
            if (!health.isEmpty()) {
                item->setText(text + tr(" · 体检失败"));
                item->setForeground(QColor(QStringLiteral("#ff6b81")));
                item->setToolTip(tr("体检失败：%1").arg(health));
            }
        }
    }
    m_updating = false;

    if (m_drafts.isEmpty()) {
        m_currentRow = -1;
        loadDraftToForm(SourceConfig());
        updateActions();
        return;
    }
    const int row = qBound(0, targetRow, m_drafts.size() - 1);
    m_list->setCurrentRow(row);
    selectRow(row);
}

void SourceManagerDialog::selectRow(int row)
{
    if (m_updating)
        return;
    commitFormToDraft();
    m_currentRow = row;
    if (row >= 0 && row < m_drafts.size())
        loadDraftToForm(m_drafts.at(row));
    else
        loadDraftToForm(SourceConfig());
    updateActions();
}

void SourceManagerDialog::commitFormToDraft()
{
    if (m_currentRow < 0 || m_currentRow >= m_drafts.size() || m_updating)
        return;
    const SourceConfig edited = readForm();
    if (edited.id.isEmpty())
        return;
    m_drafts[m_currentRow] = edited;
}

void SourceManagerDialog::loadDraftToForm(const SourceConfig &config)
{
    m_updating = true;
    m_name->setText(config.name);
    m_type->setCurrentIndex(m_type->findData(config.mediaType == MediaType::Video
                                                 ? QStringLiteral("video")
                                                 : QStringLiteral("music")));
    m_method->setCurrentIndex(m_method->findData(config.method.toUpper()));
    m_kind->setCurrentIndex(qMax(0, m_kind->findData(config.kind)));
    m_provider->setCurrentIndex(qMax(0, m_provider->findData(config.provider)));
    m_command->setText(config.command);
    m_arguments->setText(config.arguments);
    m_timeout->setValue(config.timeoutMs);
    m_url->setText(config.urlTemplate);
    if (m_siteUrl != nullptr)
        m_siteUrl->setText(config.urlTemplate);
    m_body->setPlainText(config.bodyTemplate);
    m_headers->setPlainText(headersToText(config.headers));
    m_cookie->setText(config.cookie);
    m_itemsPath->setText(config.itemsPath);
    m_idField->setText(config.fields.value(QStringLiteral("id")));
    m_titleField->setText(config.fields.value(QStringLiteral("title")));
    m_artistField->setText(config.fields.value(QStringLiteral("artist")));
    m_durationField->setText(config.fields.value(QStringLiteral("durationMs")));
    m_streamField->setText(config.fields.value(QStringLiteral("streamUrl")));
    m_lyricField->setText(config.fields.value(QStringLiteral("lyricUrl")));
    m_webField->setText(config.fields.value(QStringLiteral("webUrl")));
    m_posterField->setText(config.fields.value(QStringLiteral("posterUrl")));
    m_yearField->setText(config.fields.value(QStringLiteral("year")));
    m_areaField->setText(config.fields.value(QStringLiteral("area")));
    m_genreField->setText(config.fields.value(QStringLiteral("genre")));
    m_descriptionField->setText(config.fields.value(QStringLiteral("description")));
    m_streamKind->setCurrentIndex(
        qMax(0, m_streamKind->findData(config.streamKind)));
    m_detailUrl->setText(config.detailUrlTemplate);
    m_detailItemsPath->setText(config.detailItemsPath);
    m_detailStreamField->setText(config.detailFields.value(QStringLiteral("streamUrl")));
    m_skipIf->setPlainText(config.skipIf.isEmpty()
                               ? QString()
                               : QString::fromUtf8(QJsonDocument(config.skipIf).toJson(
                                     QJsonDocument::Indented)));
            m_updating = false;
    updateKindUi();
}

void SourceManagerDialog::updateKindUi()
{
    if (m_kind == nullptr)
        return;
    const QString kind = m_kind->currentData().toString();
    const bool isConfig = kind == QLatin1String("config");
    const bool isBuiltin = kind == QLatin1String("builtin");
    const bool isExternal = kind == QLatin1String("external");
    const QString provider = m_provider != nullptr ? m_provider->currentData().toString()
                                                   : QString();

    m_builtinRow->setVisible(isBuiltin);
    // 网页型影视源没有签名，只需要一个站点首页地址
    const bool isWebSource = isBuiltin && provider == QLatin1String("maccms_web");
    if (m_siteUrlRow != nullptr)
        m_siteUrlRow->setVisible(isWebSource);
    m_requestGroup->setVisible(isConfig);
    m_fieldsGroup->setVisible(isConfig);
    m_filterGroup->setVisible(isConfig);
    m_externalGroup->setVisible(isExternal);
    if (m_detailGroup != nullptr)
        m_detailGroup->setVisible(isConfig);

    // 从其他类型切到外部解析器时，列表里的勾选状态同步为停用，
    // 避免界面上看起来已启用、保存后却悄悄变成停用的错位。
    if (isExternal && m_currentRow >= 0 && m_currentRow < m_drafts.size()
        && m_drafts.at(m_currentRow).kind != QLatin1String("external")) {
        m_drafts[m_currentRow].enabled = false;
        const QString id = m_drafts.at(m_currentRow).id;
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            if (item != nullptr && item->data(Qt::UserRole).toString() == id) {
                m_updating = true;
                item->setCheckState(Qt::Unchecked);
                m_updating = false;
                break;
            }
        }
    }
}

SourceConfig SourceManagerDialog::readForm() const
{
    SourceConfig config;
    if (m_currentRow >= 0 && m_currentRow < m_drafts.size())
        config = m_drafts.at(m_currentRow);

    config.name = m_name->text().trimmed();
    if (config.id.isEmpty())
        config.id = makeUniqueId(config.name.isEmpty() ? QStringLiteral("source") : config.name,
                                 m_drafts);
    config.mediaType = m_type->currentData().toString() == QLatin1String("video")
                           ? MediaType::Video
                           : MediaType::Music;
    const bool wasExternal = config.kind == QLatin1String("external");
    config.kind = m_kind->currentData().toString();
    config.provider = m_provider->currentData().toString();
    // 新改成外部解析器的源默认停用，必须到列表里勾选并确认命令后才能参与搜索。
    if (config.kind == QLatin1String("external") && !wasExternal)
        config.enabled = false;
    if (config.kind == QLatin1String("external")) {
        config.command = m_command->text().trimmed();
        config.arguments = m_arguments->text();
        config.timeoutMs = m_timeout->value();
    }
    config.method = m_method->currentData().toString();
    config.urlTemplate = m_url->text().trimmed();
    // 内置网页源只在“站点地址”一栏里编辑，别被隐藏的 JSON 地址栏覆盖
    if (config.kind == QLatin1String("builtin") && config.provider == QLatin1String("maccms_web")
        && m_siteUrl != nullptr)
        config.urlTemplate = m_siteUrl->text().trimmed();
    config.bodyTemplate = m_body->toPlainText();
    config.bodyIsJson = config.bodyTemplate.trimmed().startsWith(QLatin1Char('{'));
    config.headers = textToHeaders(m_headers->toPlainText());
    config.cookie = m_cookie->text().trimmed();
    config.itemsPath = m_itemsPath->text().trimmed();

    config.fields.clear();
    const auto put = [&config](const QString &key, const QString &value) {
        if (!value.trimmed().isEmpty())
            config.fields.insert(key, value.trimmed());
    };
    put(QStringLiteral("id"), m_idField->text());
    put(QStringLiteral("title"), m_titleField->text());
    put(QStringLiteral("artist"), m_artistField->text());
    put(QStringLiteral("durationMs"), m_durationField->text());
    put(QStringLiteral("streamUrl"), m_streamField->text());
    put(QStringLiteral("lyricUrl"), m_lyricField->text());
    put(QStringLiteral("webUrl"), m_webField->text());
    put(QStringLiteral("posterUrl"), m_posterField->text());
    put(QStringLiteral("year"), m_yearField->text());
    put(QStringLiteral("area"), m_areaField->text());
    put(QStringLiteral("genre"), m_genreField->text());
    put(QStringLiteral("description"), m_descriptionField->text());
    config.streamKind = m_streamKind->currentData().toString();

    // 二次解析：地址模板留空就当作不需要，其余两个字段一并清掉
    config.detailUrlTemplate = m_detailUrl->text().trimmed();
    config.detailItemsPath = config.detailUrlTemplate.isEmpty()
                                 ? QString()
                                 : m_detailItemsPath->text().trimmed();
    config.detailFields.clear();
    if (!config.detailUrlTemplate.isEmpty()) {
        const QString detailStream = m_detailStreamField->text().trimmed();
        if (!detailStream.isEmpty())
            config.detailFields.insert(QStringLiteral("streamUrl"), detailStream);
    }

    const QString skipText = m_skipIf->toPlainText().trimmed();
    if (skipText.isEmpty()) {
        config.skipIf = QJsonObject();
    } else {
        const QJsonDocument document = QJsonDocument::fromJson(skipText.toUtf8());
        config.skipIf = document.isObject() ? document.object() : QJsonObject();
    }
    return config;
}

void SourceManagerDialog::updateActions()
{
    const bool hasSelection = m_currentRow >= 0 && m_currentRow < m_drafts.size();
    const bool builtin = hasSelection && m_drafts.at(m_currentRow).builtin;
    m_btnRemove->setEnabled(hasSelection && !builtin);
    m_btnDuplicate->setEnabled(hasSelection);
    m_btnTest->setEnabled(hasSelection);
}

void SourceManagerDialog::onAdd()
{
    commitFormToDraft();

    SourceConfig draft;
    draft.id = makeUniqueId(QStringLiteral("source"), m_drafts);
    draft.name = tr("新搜索源");
    draft.enabled = true;
    draft.method = QStringLiteral("GET");
    m_drafts.append(draft);
    reloadList(m_drafts.size() - 1);
    m_name->setFocus();
}

void SourceManagerDialog::onAddExample()
{
    commitFormToDraft();

    // 影视采集接口的通用模板：苹果CMS/海螺那类 JSON 接口基本都是这套字段
    SourceConfig draft;
    draft.id = makeUniqueId(QStringLiteral("movie"), m_drafts);
    draft.name = tr("影视采集接口（示例）");
    draft.enabled = false;
    draft.mediaType = MediaType::Video;
    draft.kind = QStringLiteral("config");
    draft.method = QStringLiteral("GET");
    draft.urlTemplate =
        QStringLiteral("https://example.com/api.php/provide/vod/?ac=detail&wd={keyword}");
    draft.headers.insert(QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0"));
    draft.itemsPath = QStringLiteral("list");
    draft.streamKind = QStringLiteral("episodes");
    draft.fields.insert(QStringLiteral("id"), QStringLiteral("vod_id"));
    draft.fields.insert(QStringLiteral("title"), QStringLiteral("vod_name"));
    draft.fields.insert(QStringLiteral("year"), QStringLiteral("vod_year"));
    draft.fields.insert(QStringLiteral("area"), QStringLiteral("vod_area"));
    draft.fields.insert(QStringLiteral("genre"), QStringLiteral("vod_class"));
    draft.fields.insert(QStringLiteral("posterUrl"), QStringLiteral("vod_pic"));
    draft.fields.insert(QStringLiteral("description"), QStringLiteral("vod_content"));
    draft.fields.insert(QStringLiteral("streamUrl"), QStringLiteral("vod_play_url"));
    draft.fields.insert(QStringLiteral("webUrl"),
                        QStringLiteral("@https://example.com/detail/{id}"));
    draft.skipIf = QJsonObject{{QStringLiteral("any"),
                                QJsonArray{QJsonObject{
                                    {QStringLiteral("path"), QStringLiteral("vod_play_url")},
                                    {QStringLiteral("op"), QStringLiteral("empty")}}}}};

    m_drafts.append(draft);
    reloadList(m_drafts.size() - 1);
    m_url->setFocus();
}

void SourceManagerDialog::onAddFromSite()
{
    commitFormToDraft();

    // 类型默认跟着当前选中的源，用户也可以在探测窗口里自己改
    const MediaType type = (m_currentRow >= 0 && m_currentRow < m_drafts.size())
                               ? m_drafts.at(m_currentRow).mediaType
                               : MediaType::Video;

    SourceProbeDialog dialog(m_http, type, QString(), tr("从网站添加搜索源"), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    SourceConfig probed = dialog.probedConfig();
    if (!probed.isValid()) {
        QMessageBox::warning(this, tr("探测结果不完整"),
                             tr("没有拿到完整的标题/播放地址字段，请改用“配置源”手工填写字段映射。"));
        return;
    }
    probed.id = makeUniqueId(probed.id.isEmpty() ? QStringLiteral("site") : probed.id, m_drafts);
    probed.builtin = false;
    probed.enabled = true;
    m_drafts.append(probed);
    reloadList(m_drafts.size() - 1);
    QMessageBox::information(this, tr("源已添加"),
                             tr("“%1”已自动启用；点“保存”后立即参与搜索。").arg(probed.name));
}

void SourceManagerDialog::onRefreshSelected()
{
    if (m_currentRow < 0 || m_currentRow >= m_drafts.size()) {
        QMessageBox::information(this, tr("一键更新"), tr("先在左边列表里选中一个源。"));
        return;
    }
    commitFormToDraft();

    const SourceConfig draft = m_drafts.at(m_currentRow);
    // 网页型影视源没有可探测的字段映射，一键更新改成“站点体检”
    if (draft.kind == QLatin1String("builtin") && draft.provider == QLatin1String("maccms_web")) {
        refreshWebSource(draft, draft.urlTemplate);
        return;
    }
    if (draft.builtin) {
        QMessageBox::information(this, tr("内置源"),
                                 tr("内置源由程序实现，不能通过探测更新。"));
        return;
    }
    if (draft.kind != QLatin1String("config")) {
        QMessageBox::information(this, tr("外部解析器"),
                                 tr("只有配置驱动的源支持一键更新。"));
        return;
    }

    // urlTemplate 里带 {keyword}，探源只要域名，所以砍掉查询串
    QString base = draft.urlTemplate;
    const int queryPos = base.indexOf(QLatin1Char('?'));
    if (queryPos > 0)
        base.truncate(queryPos);
    const QUrl url(base);
    if (!url.isValid() || url.host().isEmpty()) {
        QMessageBox::information(this, tr("一键更新"),
                                 tr("这个源没有可探测的网址，没法自动更新。"));
        return;
    }

    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(url.host());
    origin.setPort(url.port());
    const QString originText = origin.toString() + QLatin1Char('/');

    SourceProbeDialog dialog(m_http, draft.mediaType, originText,
                             tr("一键更新：%1").arg(draft.name), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const SourceConfig probed = dialog.probedConfig();
    if (!probed.isValid())
        return;

    // 保留身份与开关状态，只替换探测出来的部分
    SourceConfig merged = draft;
    merged.mediaType = probed.mediaType;
    merged.kind = probed.kind;
    merged.method = probed.method;
    merged.urlTemplate = probed.urlTemplate;
    merged.headers = probed.headers;
    merged.itemsPath = probed.itemsPath;
    merged.streamKind = probed.streamKind;
    merged.fields = probed.fields;
    m_drafts[m_currentRow] = merged;
    reloadList(m_currentRow);
}

void SourceManagerDialog::refreshWebSource(const SourceConfig &draft, const QString &previousUrl)
{
    if (m_probeBusy)
        return;

    QString base = previousUrl.trimmed();
    m_testList->clear();
    if (base.isEmpty()) {
        m_testList->addItem(tr("这个源还没有站点地址，先在“站点地址”里填上。"));
        return;
    }
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    const QUrl root(base);
    if (!root.isValid() || root.host().isEmpty()) {
        m_testList->addItem(tr("站点地址不是有效网址：%1").arg(base));
        return;
    }

    // 关键词复用“试搜关键词”，留空就用一个常见片名，只用来确认站点还能搜到片
    QString keyword = m_testKeyword->text().trimmed();
    if (keyword.isEmpty())
        keyword = QStringLiteral("流浪地球");

    m_probeBusy = true;
    ui->btn_refresh->setEnabled(false);
    m_testList->addItem(tr("正在体检 %1 …").arg(base));

    const int row = m_currentRow;
    QHash<QString, QString> headers = draft.headers;
    // 体检以当前填写的域名为准：草稿里可能残留旧域名的 Referer，不覆盖会直接 403
    headers.insert(QStringLiteral("Referer"), base);

    QPointer<SourceManagerDialog> self(this);
    m_http->fetch(root, QByteArray(), headers,
                  [self, draft, row, base, keyword](QNetworkReply *reply) {
        if (self.isNull())
            return;

        QString currentBase = base;
        QStringList notes;
        if (reply == nullptr) {
            notes.append(self->tr("首页打不开（网络错误或站点已下线）"));
        } else {
            const QUrl finalUrl = reply->url();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
            notes.append(self->tr("首页响应 HTTP %1").arg(status));
            const QUrl baseUrl(base);
            if (status >= 200 && status < 400 && finalUrl.isValid() && !finalUrl.host().isEmpty()
                && finalUrl.host().compare(baseUrl.host(), Qt::CaseInsensitive) != 0) {
                // 发布页/换域名是这类站的常态：先记下候选新地址，验真通过才写回
                QUrl suggested = finalUrl;
                suggested.setQuery(QString());
                suggested.setPath(QStringLiteral("/"));
                currentBase = suggested.toString();
                notes.append(self->tr("检测到跳转：%1 → %2").arg(base, currentBase));
            }
        }

        // 用候选地址跑一次小样本搜索；成功才算体检通过
        SourceConfig probe;
        if (row >= 0 && row < self->m_drafts.size())
            probe = self->m_drafts.at(row);
        else
            probe = draft;
        probe.urlTemplate = currentBase;

        auto *provider = createSearchProvider(probe, self->m_http, self);
        connect(provider, &SearchProviderBase::finished, self,
                [self, provider, probe, row, base, currentBase, notes, keyword](
                    const QString &, const QVector<MediaItem> &items, int filtered,
                    const QString &error) {
                    if (self.isNull())
                        return;
                    provider->deleteLater();
                    self->m_probeBusy = false;
                    self->ui->btn_refresh->setEnabled(true);

                    QStringList lines = notes;
                    if (!error.isEmpty() && items.isEmpty()) {
                        lines.append(self->tr("试搜失败：%1").arg(error));
                    } else if (items.isEmpty()) {
                        lines.append(self->tr("试搜「%1」命中 0 条可用地址（过滤 %2 条）")
                                         .arg(keyword)
                                         .arg(filtered));
                    } else {
                        lines.append(self->tr("试搜「%1」命中 %2 条可用地址（过滤 %3 条）")
                                         .arg(keyword)
                                         .arg(items.size())
                                         .arg(filtered));
                        for (int i = 0; i < qMin(items.size(), 3); ++i)
                            lines.append(QStringLiteral("    ") + items.at(i).displayName());
                    }

                    const bool healthy = error.isEmpty() && !items.isEmpty();
                    const bool redirected = currentBase != base;
                    if (healthy) {
                        self->m_healthIssues.remove(probe.id);
                        if (redirected && row >= 0 && row < self->m_drafts.size()) {
                            // 跳转后的地址验真通过，写回草稿（点“保存”才落盘）
                            self->m_drafts[row].urlTemplate = currentBase;
                            // 域名已变，旧 Referer 必须覆盖；否则会带着旧站点的头去请求新站
                            self->m_drafts[row].headers.insert(QStringLiteral("Referer"),
                                                               currentBase);
                            self->m_siteUrl->setText(currentBase);
                            self->m_url->setText(currentBase);
                            lines.append(self->tr("地址已更新为 %1（保存后生效）").arg(currentBase));
                        } else if (!redirected) {
                            lines.append(self->tr("站点地址仍然有效。"));
                        }
                    } else {
                        self->m_healthIssues.insert(
                            probe.id,
                            error.isEmpty() ? self->tr("试搜 0 条可用地址") : error);
                        if (redirected)
                            lines.append(self->tr("跳转后的地址不可用，保留原地址 %1。").arg(base));
                    }

                    self->m_testList->clear();
                    for (int i = 0; i < lines.size(); ++i) {
                        auto *item = new QListWidgetItem(lines.at(i), self->m_testList);
                        if (!healthy && i == lines.size() - 1)
                            item->setForeground(QColor(QStringLiteral("#ff6b81")));
                    }
                    self->reloadList(row >= 0 ? row : self->m_currentRow);
                });
        provider->search(keyword, 3);
    });
}

void SourceManagerDialog::onDuplicate()
{
    if (m_currentRow < 0 || m_currentRow >= m_drafts.size())
        return;
    commitFormToDraft();

    SourceConfig copy = m_drafts.at(m_currentRow);
    copy.id = makeUniqueId(copy.id, m_drafts);
    copy.name = copy.name + tr("（副本）");
    copy.builtin = false;
    m_drafts.append(copy);
    reloadList(m_drafts.size() - 1);
}

void SourceManagerDialog::onRemove()
{
    if (m_currentRow < 0 || m_currentRow >= m_drafts.size())
        return;
    if (m_drafts.at(m_currentRow).builtin) {
        QMessageBox::information(this, tr("内置源"),
                                 tr("内置源不能删除，可以停用，或者用“复制”改成自己的配置。"));
        return;
    }
    const int row = m_currentRow;
    m_currentRow = -1; // 防止删除后 commitFormToDraft 写回已删除项
    m_drafts.removeAt(row);
    reloadList(row < m_drafts.size() ? row : m_drafts.size() - 1);
}

void SourceManagerDialog::onTest()
{
    if (m_currentRow < 0)
        return;

    const SourceConfig draft = readForm();
    if (!draft.isValid()) {
        QMessageBox::warning(this, tr("配置不完整"),
                             tr("配置驱动源至少要填：搜索地址、标题字段、播放地址字段；"
                                "外部解析器至少要填命令。"));
        return;
    }
    const QString keyword = m_testKeyword->text().trimmed();
    if (keyword.isEmpty()) {
        QMessageBox::information(this, tr("试搜"), tr("先在“试搜关键词”里填一个词。"));
        return;
    }

    commitFormToDraft();
    if (draft.kind == QLatin1String("external")) {
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this, tr("外部解析器会执行本机命令"),
            tr("试搜会直接启动本机程序：\n\n%1 %2\n\n只在你信任这个命令时继续。")
                .arg(draft.command)
                .arg(draft.arguments),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes)
            return;
    }
    m_testList->clear();
    m_testList->addItem(tr("正在搜索“%1”…").arg(keyword));
    m_btnTest->setEnabled(false);

    auto *provider = createSearchProvider(draft, m_http, this);
    connect(provider, &SearchProviderBase::finished, this,
            [this, provider](const QString &, const QVector<MediaItem> &items, int filtered,
                             const QString &error) {
                m_testList->clear();
                if (!error.isEmpty()) {
                    m_testList->addItem(tr("失败：%1").arg(error));
                } else if (items.isEmpty()) {
                    m_testList->addItem(tr("没有搜到可用结果（被过滤 %1 条）").arg(filtered));
                } else {
                    for (int i = 0; i < qMin(items.size(), 8); ++i) {
                        const MediaItem &item = items.at(i);
                        m_testList->addItem(tr("%1. %2  →  %3")
                                                .arg(QString::number(i + 1), item.displayName(),
                                                     item.url.toString()));
                    }
                    if (filtered > 0)
                        m_testList->addItem(tr("另有 %1 条被过滤规则跳过").arg(filtered));
                }
                m_btnTest->setEnabled(true);
                provider->deleteLater();
            });
    provider->search(keyword, 20);
}

void SourceManagerDialog::onImport()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("导入搜索源"), QString(),
                                                     tr("JSON (*.json)"));
    if (path.isEmpty())
        return;

    QVector<SourceConfig> imported;
    QString error;
    if (!readSourcesFromFile(&imported, path, &error)) {
        QMessageBox::warning(this, tr("导入失败"), error);
        return;
    }

    // 外部解析器会在本机执行命令；导入时把完整命令行摆给用户看，默认不继续。
    QStringList externalLines;
    for (const SourceConfig &config : imported) {
        if (config.kind == QLatin1String("external")) {
            externalLines.append(QStringLiteral("%1: %2 %3")
                                     .arg(config.name.isEmpty() ? config.id : config.name)
                                     .arg(config.command)
                                     .arg(config.arguments));
        }
    }
    if (!externalLines.isEmpty()) {
        const QString detail = externalLines.mid(0, 5).join(QLatin1Char('\n'));
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this, tr("外部解析器会执行本机命令"),
            tr("导入的配置包含 %1 个外部解析器，搜索时会直接启动本机程序：\n\n%2\n\n"
               "只导入你信任的配置。是否继续？")
                .arg(externalLines.size())
                .arg(detail),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes)
            return;
    }

    commitFormToDraft();
    int replaced = 0;
    int externalDisabled = 0;
    for (SourceConfig &config : imported) {
        // 导入配置不等于授权执行命令：外部解析器一律先停用，
        // 用户之后在列表里勾选时还会再看到一次完整命令行确认。
        if (config.kind == QLatin1String("external")) {
            config.enabled = false;
            ++externalDisabled;
        }
        bool merged = false;
        for (SourceConfig &existing : m_drafts) {
            if (existing.id == config.id) {
                existing = config;
                merged = true;
                ++replaced;
                break;
            }
        }
        if (!merged)
            m_drafts.append(config);
    }
    reloadList(0);
    QString detail = tr("新增/更新 %1 个源，覆盖同名 %2 个。").arg(imported.size()).arg(replaced);
    if (externalDisabled > 0) {
        detail += tr("\n\n其中 %1 个外部解析器已默认停用；"
                     "在列表里勾选时才会再确认命令。")
                      .arg(externalDisabled);
    }
    QMessageBox::information(this, tr("导入完成"), detail);
}

void SourceManagerDialog::onExport()
{
    commitFormToDraft();
    const QString path = QFileDialog::getSaveFileName(this, tr("导出搜索源"),
                                                     QStringLiteral("sources.json"),
                                                     tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    if (!writeSourcesToFile(m_drafts, path))
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件"));
}

bool SourceManagerDialog::saveAll()
{
    commitFormToDraft();
    if (m_registry == nullptr)
        return false;

    QVector<SourceConfig> finalSources = m_drafts;

    // 草稿里没有的内置源不能被“保存”误删；内置源只能停用。
    for (const SourceConfig &config : m_registry->sources()) {
        if (!config.builtin)
            continue;
        bool kept = false;
        for (const SourceConfig &draft : finalSources) {
            if (draft.id == config.id) {
                kept = true;
                break;
            }
        }
        if (!kept)
            finalSources.append(config);
    }

    // 延迟提交模式：只把草稿交给调用方，由设置页在“保存”时统一写盘
    if (m_deferCommit) {
        m_committedSources = finalSources;
        return true;
    }

    // 一次整表提交：要么全部写成功，要么保持原文件；不会出现删到一半/写一半。
    if (!m_registry->replaceAll(finalSources)) {
        QMessageBox::warning(this, tr("保存失败"),
                             m_registry->lastError().isEmpty()
                                 ? tr("无法写入 sources.json，请检查 Data 目录权限。")
                                 : m_registry->lastError());
        return false;
    }
    return true;
}

bool SourceManagerDialog::writeSourcesToFile(const QVector<SourceConfig> &sources,
                                             const QString &path)
{
    QJsonArray array;
    for (const SourceConfig &config : sources)
        array.append(sourceConfigToJson(config));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}

bool SourceManagerDialog::readSourcesFromFile(QVector<SourceConfig> *sources, const QString &path,
                                              QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = tr("无法读取文件");
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr)
            *error = tr("不是合法的 JSON：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonArray array = document.isArray() ? document.array() : QJsonArray();
    if (array.isEmpty()) {
        if (error != nullptr)
            *error = tr("文件里没有搜索源（应为 JSON 数组）");
        return false;
    }

    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        bool ok = false;
        const SourceConfig config = sourceConfigFromJson(value.toObject(), &ok);
        if (ok)
            sources->append(config);
    }
    if (sources->isEmpty()) {
        if (error != nullptr)
            *error = tr("没有解析出可用的源");
        return false;
    }
    return true;
}

void SourceManagerDialog::accept()
{
    if (!saveAll())
        return; // 写盘失败时保留对话框，用户可改权限/重试，不假装保存成功
    QDialog::accept();
}
