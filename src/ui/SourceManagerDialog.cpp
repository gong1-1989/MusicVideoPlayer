/// @file SourceManagerDialog.cpp
/// @brief 搜索源管理对话框实现：表单/草稿双向同步、试搜验证、站点更新与导入导出。
#include "ui/SourceManagerDialog.h"

#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "net/SearchCoordinator.h"
#include "net/SearchProviderBase.h"
#include "net/SourceRegistry.h"
#include "ui/SourceProbeDialog.h"
#include "ui/ThemeManager.h"
#include "ui_SourceManagerDialog.h"
#include "util/HeaderPolicy.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QFontDatabase>
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
#include <QSignalBlocker>
#include <QUrl>
#include <QUrlQuery>

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

bool urlHasSecretQuery(const QString &urlText)
{
    const QUrl url(urlText);
    if (!url.isValid())
        return false;
    static const QStringList secretKeys = {
        QStringLiteral("api_key"), QStringLiteral("apikey"), QStringLiteral("key"),
        QStringLiteral("token"), QStringLiteral("access_token"), QStringLiteral("auth"),
        QStringLiteral("authorization"), QStringLiteral("password"), QStringLiteral("passwd"),
        QStringLiteral("secret"), QStringLiteral("sign"), QStringLiteral("signature")};
    const QUrlQuery query(url);
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        if (secretKeys.contains(item.first.toLower()))
            return true;
    }
    return false;
}

QString skipIfSyntaxError(const QString &text)
{
    if (text.trimmed().isEmpty())
        return QString();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError)
        return QObject::tr("过滤规则不是有效 JSON：%1").arg(error.errorString());
    if (!document.isObject())
        return QObject::tr("过滤规则必须是 JSON 对象，例如 {\"name\":\"广告\"}");
    return QString();
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
    const QRect available = screen() != nullptr ? screen()->availableGeometry()
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
    configureHoverHelp();
    reloadList(m_drafts.isEmpty() ? -1 : 0);

    // 原生标题栏跟着主题走
    ThemeManager::instance().applyWindowChrome(this);
}

SourceManagerDialog::~SourceManagerDialog()
{
    delete ui;
}

/// @brief 用外部草稿替换当前列表，用于延迟提交模式再次打开。
void SourceManagerDialog::setDraftSources(const QVector<SourceConfig> &sources)
{
    // 先把编辑区从旧草稿上摘下来，否则 reloadList() 会把旧表单写回新草稿第 0 行
    m_currentRow = -1;
    loadDraftToForm(SourceConfig());
    m_drafts = sources;
    reloadList(m_drafts.isEmpty() ? -1 : 0);
}

/// @brief 绑定界面信号，并把表单编辑实时写回当前草稿。
void SourceManagerDialog::bindUi()
{
    m_list = ui->list_sources;
    // 两行紧凑按钮占高较小，列表可以保留更多可见行；常规尺寸仍会自动撑满
    m_list->setMinimumHeight(140);
    ui->layout_body->setStretch(0, 0);
    ui->layout_body->setStretch(1, 1);
    // 源列表独自吃掉左栏剩余高度，底部操作按钮固定不动
    ui->layout_left->setStretch(0, 1);
    // 左栏/表单的“舒适宽度”在窄屏或大字体下必须让位，否则窗口会被最小宽度顶出屏幕。
    const int availableWidth = screen() != nullptr ? screen()->availableGeometry().width()
                                                   : 1280;
    const int leftMin = qMin(240, qMax(120, availableWidth / 4));
    ui->leftBox->setMinimumWidth(leftMin);
    ui->leftBox->setMaximumWidth(qMax(leftMin, qMin(320, availableWidth / 3)));
    ui->scroll_form->setMinimumWidth(
        qMin(420, qMax(220, availableWidth - leftMin - 48)));
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
    m_kind->addItem(tr("内置实现"), QStringLiteral("builtin"));
    m_kind->addItem(tr("外部解析器进程"), QStringLiteral("external"));

    m_provider->addItem(tr("网页型影视（内置抓取）"), QStringLiteral("maccms_web"));

    m_method->addItem(QStringLiteral("GET"), QStringLiteral("GET"));
    m_method->addItem(QStringLiteral("POST"), QStringLiteral("POST"));

    m_streamKind->addItem(tr("单条直链"), QStringLiteral("direct"));
    m_streamKind->addItem(tr("剧集列表（名称$地址#名称$地址）"), QStringLiteral("episodes"));

    m_timeout->setRange(0, 60000);
    m_timeout->setSingleStep(500);
    m_timeout->setSuffix(tr(" 毫秒"));
    m_timeout->setSpecialValueText(tr("自动"));

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
                                   "预置配置源（如网易云）：按当前配置试搜验证；"
                                   "网页型影视源：体检站点地址（含跳转）并试搜验真"));
    connect(m_btnTest, &QPushButton::clicked, this, &SourceManagerDialog::onTest);

    connect(m_list, &QListWidget::currentRowChanged, this, &SourceManagerDialog::selectRow);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_updating || item == nullptr)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        const bool wantsEnabled = item->checkState() == Qt::Checked;
        // 外部解析器启用即执行本机命令，必须在勾选当场确认；否则“保存”会
        // 静默把任意命令带进下一次搜索。
        int externalIndex = -1;
        if (wantsEnabled) {
            for (int i = 0; i < m_drafts.size(); ++i) {
                if (m_drafts.at(i).id == id
                    && m_drafts.at(i).kind == QLatin1String("external")) {
                    externalIndex = i;
                    break;
                }
            }
        }
        if (externalIndex >= 0) {
            const SourceConfig external = m_drafts.at(externalIndex);
            const QMessageBox::StandardButton choice = QMessageBox::warning(
                this, tr("启用外部解析器"),
                tr("启用后搜索会直接启动本机程序：\n\n%1 %2\n\n只在你信任这个命令时启用。")
                    .arg(external.command, external.arguments),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (choice != QMessageBox::Yes) {
                // 确认框会跑嵌套事件循环，站点体检/试搜回调可能已经 reloadList，
                // 原来的 item/config 引用都可能失效；按 id 重新找回再复位。
                m_updating = true;
                for (int i = 0; i < m_list->count(); ++i) {
                    QListWidgetItem *current = m_list->item(i);
                    if (current != nullptr
                        && current->data(Qt::UserRole).toString() == id) {
                        current->setCheckState(Qt::Unchecked);
                        break;
                    }
                }
                m_updating = false;
                for (SourceConfig &config : m_drafts) {
                    if (config.id == id) {
                        config.enabled = false;
                        break;
                    }
                }
                updateActions();
                return;
            }
        }
        for (SourceConfig &config : m_drafts) {
            if (config.id == id) {
                config.enabled = wantsEnabled;
                break;
            }
        }
        updateActions();
    });
}

/// @brief 为各输入框设置悬停说明，解释字段含义与留空后的行为。
void SourceManagerDialog::configureHoverHelp()
{
    auto help = [](QWidget *widget, const QString &text) {
        if (widget != nullptr)
            widget->setToolTip(text);
    };
    // 标签和输入框用同一段说明：鼠标停在字段名上也能看到解释。
    auto pair = [&](QLabel *label, QWidget *editor, const QString &text) {
        help(label, text);
        help(editor, text);
    };

    // ---------------------------------------------------------- 左侧列表与操作
    help(ui->list_sources,
         tr("勾选表示启用这个搜索源；选中一行后，在右侧编辑它的地址和字段。"
            "红色条目表示配置失效或站点体检失败，鼠标停在条目上可看具体原因。"));
    help(ui->btn_add, tr("新建一个空白搜索源，自己填写请求地址和字段对应。"));
    help(ui->btn_duplicate, tr("复制当前选中的源，适合在已有配置上改出另一个站点。"));
    help(ui->btn_remove, tr("删除当前选中的源；内置源不能删除，只能停用。"));
    help(ui->btn_addSite, tr("只填一个网站首页地址：优先探测 JSON 采集接口；"
                            "如果是公开 MacCMS/网页型影视站，会自动转为内置网页抓取。"
                            "音乐源通常仍需要接口地址或账号/API key。"));
    help(ui->btn_import, tr("从 JSON 配置文件导入搜索源；如含外部解析器命令，"
                           "导入时会先展示命令并默认停用。"));
    help(ui->btn_export, tr("把当前全部搜索源导出为 JSON，便于备份或换机导入。"));
    help(ui->btn_example, tr("插入一份影视采集接口的空白模板，填上自己的接口地址即可。"));

    // ---------------------------------------------------------- 基本信息
    help(ui->group_basic,
         tr("搜索源的基本身份：名称只用于显示；媒体类型决定结果进入音乐还是影视列表。"));
    pair(ui->label_captionName, ui->edit_name,
         tr("搜索源显示名称，可任意填写，只影响列表和搜索结果里的来源标签。"));
    pair(ui->label_captionType, ui->combo_type,
         tr("这个源返回的内容类型：音乐结果进入音乐列表，视频结果进入影视列表。"));
    pair(ui->label_captionKind, ui->combo_kind,
         tr("配置驱动：用 JSON 接口地址和字段映射搜索；内置实现：使用程序自带解析；"
            "外部解析器：调用本机程序并读取它输出的 JSON。"));
    pair(ui->label_captionProvider, ui->combo_provider,
         tr("内置网页型影视解析方式，只需要站点首页地址，不需要专属签名。"));
    help(ui->label_builtinHint,
         tr("网页型影视源只解析公开页面，当前只返回影视/视频结果，不处理登录或加密播放。"));
    pair(ui->label_captionSiteUrl, ui->edit_siteUrl,
         tr("要抓取的网站首页地址，例如 https://example.com/；程序会从首页和常见搜索路径探测接口。"));
    pair(ui->label_captionTimeout, ui->spin_timeout,
         tr("单次请求的超时时间，单位毫秒；选“自动”时由程序按网络情况决定。"));

    // ---------------------------------------------------------- 请求
    help(ui->group_request,
         tr("描述一次搜索请求：请求方式、地址、请求体、请求头和结果数组所在位置。"));
    pair(ui->label_captionMethod, ui->combo_method,
         tr("GET 把参数拼在地址里；POST 把参数放在请求体里发送。"));
    pair(ui->label_captionUrl, ui->edit_url,
         tr("搜索接口地址。可用占位符：{keyword} 替换成关键词，{limit} 替换成期望条数。"
            "例如 https://api.example.com/search?q={keyword}&n={limit}"));
    pair(ui->label_captionBody, ui->text_body,
         tr("仅 POST 使用：填写表单或 JSON 请求体，可使用 {keyword} 和 {limit} 占位符；"
            "GET 时留空。"));
    pair(ui->label_captionHeaders, ui->text_headers,
         tr("每行一个“键: 值”，例如 Referer: https://example.com/。"
            "需要登录态的 Cookie 建议填到下面的 Cookie 栏。"));
    pair(ui->label_captionCookie, ui->edit_cookie,
         tr("可选。接口需要登录时填写 Cookie 原文，例如 token=xxx；"
            "会随请求发送，也会写入本地源配置。"));
    pair(ui->label_captionItems, ui->edit_itemsPath,
         tr("结果数组在响应 JSON 里的路径，用点号分隔，例如 data.list。"
            "留空表示响应根节点本身就是数组。"));

    // ---------------------------------------------------------- 字段对应
    help(ui->group_fields,
         tr("把接口返回 JSON 里的字段名映射到播放器需要的信息；右侧填接口里的键名，"
            "不是填具体内容。"));
    pair(ui->label_captionId, ui->edit_idField,
         tr("条目唯一 id 的字段名，用于拼接详情或播放地址；接口没有 id 时可留空。"));
    pair(ui->label_captionTitle, ui->edit_titleField,
         tr("歌名/视频名的字段名，通常必填，例如 name、title、songname。"));
    pair(ui->label_captionArtist, ui->edit_artistField,
         tr("歌手/作者的字段名，可留空；例如 artist、singer、author。"));
    pair(ui->label_captionDuration, ui->edit_durationField,
         tr("时长字段名，单位按接口原值；如果接口返回秒，程序会按数值判断，可留空。"));
    pair(ui->label_captionStream, ui->edit_streamField,
         tr("播放直链字段名，例如 url、play_url。需要自己拼地址时写 @https://host/play?id={id}，"
            "开头的 @ 表示这是地址模板而不是 JSON 字段名。"));
    pair(ui->label_captionStreamKind, ui->combo_streamKind,
         tr("单条直链：一个条目一个播放地址；剧集列表：一个条目里包含多集，"
            "格式为“名称$地址#名称$地址”。"));
    pair(ui->label_captionLyric, ui->edit_lyricField,
         tr("歌词地址字段名，可留空；支持 LRC 文本或歌词接口地址。"));
    pair(ui->label_captionWeb, ui->edit_webField,
         tr("原网页地址字段名，可留空；用于显示来源或在浏览器中打开原页面。"));
    pair(ui->label_captionPoster, ui->edit_posterField,
         tr("海报/封面地址字段名，可留空。"));
    pair(ui->label_captionYear, ui->edit_yearField,
         tr("上映/发行年份字段名，可留空。"));
    pair(ui->label_captionArea, ui->edit_areaField,
         tr("地区字段名，可留空。"));
    pair(ui->label_captionGenre, ui->edit_genreField,
         tr("题材/分类字段名，可留空。"));
    pair(ui->label_captionDesc, ui->edit_descriptionField,
         tr("简介字段名，可留空；结果列表里长文本会自动截断。"));

    // ---------------------------------------------------------- 二次解析
    help(ui->group_detail,
         tr("可选。搜索接口只返回 id、不返回直链时，用 id 再请求一次详情接口换出播放地址。"));
    pair(ui->label_captionDetailUrl, ui->edit_detailUrl,
         tr("详情接口地址模板，用 {id} 代表条目 id，例如 https://api.example.com/track/{id}。"));
    pair(ui->label_captionDetailPath, ui->edit_detailItemsPath,
         tr("详情响应里播放信息所在路径，例如 data；留空表示根节点。"));
    pair(ui->label_captionDetailStream, ui->edit_detailStreamField,
         tr("详情响应里的直链字段名，例如 url、stream.url；支持点号路径。"));

    // ---------------------------------------------------------- 过滤 / 外部解析器
    help(ui->group_filter,
         tr("可选。命中规则的条目会在搜索结果里被跳过；规则使用 JSON 数组描述。"));
    help(ui->text_skipIf,
         tr("填写 JSON 过滤规则，例如按字段等于某值或缺少播放地址时跳过；"
            "格式可参考输入框里的示例，留空表示不过滤。"));
    help(ui->group_external,
         tr("可选。配置无法描述签名或加密时，改为启动本机程序，让程序输出 JSON 结果。"));
    pair(ui->label_captionCommand, ui->edit_command,
         tr("要启动的程序：可填 python、node，或程序目录下的脚本、绝对路径的 exe。"
            "启用外部解析器前会再次确认命令。"));
    pair(ui->label_captionArguments, ui->edit_arguments,
         tr("传给程序的参数，可使用 {keyword} 和 {limit} 占位符，"
            "例如 --keyword={keyword} --limit={limit}。"));
    help(ui->label_externalHint,
         tr("外部解析器必须把 JSON 结果打印到标准输出；程序只读取 stdout，"
            "报错请写到 stderr。"));

    // ---------------------------------------------------------- 底部试搜与保存
    help(ui->edit_testKeyword,
         tr("输入关键词后点“试搜当前源”，用当前表单里的配置实际请求一次，先验证再保存。"));
    help(ui->btn_test,
         tr("用当前表单配置试搜，不会修改已保存的源；结果会显示在下方列表。"));
    help(ui->list_test,
         tr("试搜结果：显示标题、来源和是否有可播放直链；双击或选中后可查看详情。"));
    help(ui->btn_cancel, tr("放弃本次所有未保存的修改并关闭窗口。"));
    help(ui->btn_save,
         tr("把左侧列表和右侧表单的修改写入搜索源配置；关闭窗口后立即生效。"));
}

/// @brief 重建左侧源列表并尽量保持选中行。
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

/// @brief 选中指定行并把该源载入表单。
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

/// @brief 把当前表单内容写回草稿（切换行/保存前调用）。
void SourceManagerDialog::commitFormToDraft()
{
    if (m_currentRow < 0 || m_currentRow >= m_drafts.size() || m_updating)
        return;
    const SourceConfig edited = readForm();
    if (edited.id.isEmpty())
        return;
    m_drafts[m_currentRow] = edited;
}

/// @brief 把指定源填入表单，并刷新类型相关控件的可见状态。
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

/// @brief 按源类型（内置/接口/网页/外部程序）切换表单分组的可见性。
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

    // 当前唯一的内置网页源只返回影视结果；类型固定为视频，避免列表里
    // 显示“音乐·内置”，实际搜到的却全是视频。
    if (isWebSource && m_type != nullptr) {
        const int videoIndex = m_type->findData(QStringLiteral("video"));
        if (videoIndex >= 0 && m_type->currentIndex() != videoIndex)
            m_type->setCurrentIndex(videoIndex);
        m_type->setEnabled(false);
        m_type->setToolTip(tr("网页型影视源只支持视频类型。"));
    } else if (m_type != nullptr) {
        m_type->setEnabled(true);
        m_type->setToolTip(QString());
    }

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

/// @brief 从表单读出完整源配置。
/// @return 校验后的配置；必填缺失时返回空 id 的配置，由保存流程提示。
SourceConfig SourceManagerDialog::readForm()
{
    SourceConfig config;
    if (m_currentRow >= 0 && m_currentRow < m_drafts.size())
        config = m_drafts.at(m_currentRow);

    const QString originalKind = config.kind;
    const QString originalProvider = config.provider;
    config.name = m_name->text().trimmed();
    if (config.id.isEmpty())
        config.id = makeUniqueId(config.name.isEmpty() ? QStringLiteral("source") : config.name,
                                 m_drafts);
    config.mediaType = m_type->currentData().toString() == QLatin1String("video")
                           ? MediaType::Video
                           : MediaType::Music;
    const bool wasBuiltin = config.builtin;
    const bool wasExternal = config.kind == QLatin1String("external");
    config.kind = m_kind->currentData().toString();
    // provider 只属于内置实现。配置型/外部解析器写 provider=maccms_web
    // 会让后续重建 provider 时走错分支。
    config.provider = config.kind == QLatin1String("builtin")
                          ? m_provider->currentData().toString()
                          : QString();
    // 预置源不能借表单改掉实现类型：SourceRegistry 会按预置配置覆盖，
    // UI 先把类型锁回原值，避免表单显示与保存结果不一致。
    // 注意 builtin 表示“受保护的预置源”，不等于 kind=builtin：网易云就是
    // kind=config + builtin=true，不能因此被强制改成内置实现。
    if (wasBuiltin && config.kind != originalKind) {
        config.kind = originalKind;
        // provider 也必须锁回预置源的原始实现；否则网易云这类
        // kind=config 的预置源会残留一个 maccms_web provider。
        config.provider = originalProvider;
        if (m_kind->findData(config.kind) >= 0) {
            QSignalBlocker blocker(m_kind);
            m_kind->setCurrentIndex(m_kind->findData(config.kind));
        }
        if (m_provider->findData(config.provider) >= 0) {
            QSignalBlocker blocker(m_provider);
            m_provider->setCurrentIndex(m_provider->findData(config.provider));
        }
        // 让字段分组/类型控件跟随被锁回的 kind 立即复位，而不是等下一次
        // 切换才刷新；这一步只做 UI 同步，不改已保存的 draft。
        updateKindUi();
    }
    config.builtin = wasBuiltin;
    if (config.kind == QLatin1String("builtin")
        && config.provider == QLatin1String("maccms_web")) {
        config.mediaType = MediaType::Video;
    }
    // 新改成外部解析器的源默认停用，必须到列表里勾选并确认命令后才能参与搜索。
    if (config.kind == QLatin1String("external") && !wasExternal)
        config.enabled = false;
    if (config.kind == QLatin1String("external")) {
        config.command = m_command->text().trimmed();
        config.arguments = m_arguments->text();
    }
    // 超时对配置型/网页型/外部解析器统一生效；0 表示自动预算。
    config.timeoutMs = m_timeout->value();
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

/// @brief 按当前草稿与忙碌标记刷新按钮可用状态。
void SourceManagerDialog::updateActions()
{
    const bool hasSelection = m_currentRow >= 0 && m_currentRow < m_drafts.size();
    const bool builtin = hasSelection && m_drafts.at(m_currentRow).builtin;
    // 试搜和站点体检共用右侧结果区；任一在途时都必须禁用发起入口，
    // 否则用户可以在体检过程中再点试搜，两个回调互相覆盖结果列表。
    const bool busy = m_testBusy || m_probeBusy;
    m_btnRemove->setEnabled(hasSelection && !builtin);
    m_btnDuplicate->setEnabled(hasSelection);
    m_btnTest->setEnabled(hasSelection && !busy);
    if (ui->btn_refresh != nullptr)
        ui->btn_refresh->setEnabled(hasSelection && !busy);
}

/// @brief 新增一个空白源草稿。
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

/// @brief 添加一条带示例字段的源，方便用户照着改。
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

/// @brief 根据输入的站点地址智能生成一条网页影视源。
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
                             tr("没有拿到完整的标题/播放地址字段。"
                                "如果是公开影视网页站，请用“内置实现 → 网页型影视”添加；"
                                "否则请改用“配置源”手工填写字段映射。"));
        return;
    }
    probed.id = makeUniqueId(probed.id.isEmpty() ? QStringLiteral("site") : probed.id, m_drafts);
    probed.builtin = false;
    probed.enabled = true;
    m_drafts.append(probed);
    reloadList(m_drafts.size() - 1);
    const bool webSource = probed.kind == QLatin1String("builtin")
                           && probed.provider == QLatin1String("maccms_web");
    QMessageBox::information(this, tr("源已添加"),
                             webSource
                                 ? tr("“%1”已识别为网页型影视源并自动启用；点“保存”后立即参与搜索。")
                                       .arg(probed.name)
                                 : tr("“%1”已自动启用；点“保存”后立即参与搜索。").arg(probed.name));
}

/// @brief 刷新选中源：预置源改试搜验证，网页源走一键更新，其余重新探查。
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
    if (draft.builtin && draft.kind != QLatin1String("builtin")) {
        // 网易云这类预置配置源：搜索地址、字段和过滤规则由程序维护，
        // 不能像普通源那样从站点首页重新探测；更新改成按当前配置试搜验真。
        verifyProtectedSource(draft);
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
    if (!probed.isValid()) {
        QMessageBox::warning(this, tr("一键更新失败"),
                             tr("探测结果不完整，未覆盖原来的字段映射。\n\n"
                                "请确认站点仍可访问，或改用“配置源”手工填写字段映射。"));
        return;
    }

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

/// @brief 预置配置源的“更新”改为按当前配置试搜验真，不覆盖程序维护的字段。
void SourceManagerDialog::verifyProtectedSource(const SourceConfig &draft)
{
    // 试搜和站点体检都会写同一块结果区；两种请求必须互斥。
    if (m_testBusy || m_probeBusy) {
        QMessageBox::information(this, tr("正在验证"),
                                 tr("上一次试搜或更新检查还没结束，请稍等。"));
        return;
    }

    // 预置源没有站点首页可供重新探测，但必须能确认“当前配置仍然可用”。
    // 用一次真实试搜代替探测，成功就明确报通过，不再弹“一键更新失败”。
    QString keyword = m_testKeyword->text().trimmed();
    if (keyword.isEmpty()) {
        keyword = draft.mediaType == MediaType::Music ? QStringLiteral("晴天")
                                                      : QStringLiteral("流浪地球");
        m_testKeyword->setText(keyword);
    }

    m_testBusy = true;
    m_testList->clear();
    m_testList->addItem(tr("正在验证预置源“%1”…").arg(draft.name));
    m_btnTest->setEnabled(false);
    updateActions();

    SearchProviderBase *provider = createSearchProvider(draft, m_http, this);
    if (provider == nullptr) {
        m_testBusy = false;
        updateActions();
        m_testList->clear();
        m_testList->addItem(tr("失败：无法创建搜索器"));
        QMessageBox::warning(this, tr("更新检查失败"), tr("无法创建这个源的搜索器。"));
        return;
    }

    connect(provider, &SearchProviderBase::finished, this,
            [this, provider, draft, keyword](const QString &, const QVector<MediaItem> &items,
                                            int filtered, const QString &error) {
                m_testBusy = false;
                updateActions();
                m_testList->clear();
                if (!error.isEmpty()) {
                    m_testList->addItem(tr("失败：%1").arg(error));
                    QMessageBox::warning(
                        this, tr("更新检查失败"),
                        tr("“%1”按当前配置试搜没有通过：\n\n%2\n\n"
                           "如果正常搜索仍可用，可先核对试搜关键词和网络状态。")
                            .arg(draft.name, error));
                } else if (items.isEmpty()) {
                    m_testList->addItem(tr("没有搜到可用结果（被过滤 %1 条）").arg(filtered));
                    QMessageBox::warning(
                        this, tr("更新检查"),
                        tr("“%1”当前配置能请求，但试搜“%2”没有可用结果（被过滤 %3 条）。")
                            .arg(draft.name, keyword, QString::number(filtered)));
                } else {
                    for (int i = 0; i < qMin(items.size(), 5); ++i) {
                        const MediaItem &item = items.at(i);
                        m_testList->addItem(tr("%1. %2  →  %3")
                                                .arg(QString::number(i + 1), item.displayName(),
                                                     item.url.toString()));
                    }
                    QMessageBox::information(
                        this, tr("更新检查通过"),
                        tr("“%1”按当前配置试搜“%2”，返回 %3 条可用结果。\n\n"
                           "这是预置配置源，搜索地址和字段映射由程序维护，无需重新探测；"
                           "如需改字段，请先复制成普通配置源。")
                            .arg(draft.name, keyword, QString::number(items.size())));
                }
                provider->deleteLater();
            });
    provider->search(keyword, 20);
}

/// @brief 网页影视源一键更新：跟随站点跳转并试搜验真，只更新可推导字段。
void SourceManagerDialog::refreshWebSource(const SourceConfig &draft, const QString &previousUrl)
{
    // 与试搜共用结果列表：试搜在途时不允许再发起站点体检。
    if (m_probeBusy || m_testBusy)
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
    updateActions();
    m_testList->clear();
    m_testList->addItem(tr("正在体检 %1 …").arg(base));

    const QString probeId = draft.id;
    QHash<QString, QString> headers = draft.headers;
    // 体检以当前填写的域名为准：草稿里可能残留旧域名的 Referer，不覆盖会直接 403
    headers.insert(QStringLiteral("Referer"), base);

    QPointer<SourceManagerDialog> self(this);
    m_http->fetch(root, QByteArray(), headers,
                  [self, draft, probeId, base, keyword](QNetworkReply *reply) {
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
        // 体检结果只对应发起时的 draft；用户中途改了表单不能拿新配置去试，
        // 否则会把“旧地址通过”记到新地址头上。
        SourceConfig probe = draft;
        probe.urlTemplate = currentBase;

        auto *provider = createSearchProvider(probe, self->m_http, self);
        if (provider == nullptr) {
            self->m_probeBusy = false;
            self->updateActions();
            self->m_testList->clear();
            self->m_testList->addItem(self->tr("失败：无法创建搜索器"));
            return;
        }
        connect(provider, &SearchProviderBase::finished, self,
                [self, provider, probe, probeId, base, currentBase, notes, keyword](
                    const QString &, const QVector<MediaItem> &items, int filtered,
                    const QString &error) {
                    if (self.isNull())
                        return;
                    provider->deleteLater();
                    self->m_probeBusy = false;
                    self->updateActions();

                    // 异步过程中列表可能重排/增删；按 id 找回当前行，不能再用旧行号。
                    int index = -1;
                    for (int i = 0; i < self->m_drafts.size(); ++i) {
                        if (self->m_drafts.at(i).id == probeId) {
                            index = i;
                            break;
                        }
                    }

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
                        const bool sameConfig =
                            index >= 0 && index < self->m_drafts.size()
                            && self->m_drafts.at(index).urlTemplate == base;
                        if (redirected && sameConfig) {
                            // 跳转后的地址验真通过，写回草稿（点“保存”才落盘）
                            self->m_drafts[index].urlTemplate = currentBase;
                            // 域名已变，旧 Referer 必须覆盖；否则会带着旧站点的头去请求新站
                            self->m_drafts[index].headers.insert(QStringLiteral("Referer"),
                                                                currentBase);
                            if (index == self->m_currentRow) {
                                self->m_siteUrl->setText(currentBase);
                                self->m_url->setText(currentBase);
                            }
                            lines.append(self->tr("地址已更新为 %1（保存后生效）").arg(currentBase));
                        } else if (redirected) {
                            lines.append(self->tr("配置在体检期间已修改，跳转地址 %1 未写回。")
                                             .arg(currentBase));
                        } else {
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
                    self->reloadList(index >= 0 ? index : self->m_currentRow);
                });
        provider->search(keyword, 3);
    }, NetLimits::kHtmlBytes);
}

/// @brief 复制当前源为一条新草稿。
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

/// @brief 删除选中源草稿（内置源只允许停用，不允许删除）。
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

/// @brief 用当前表单配置试搜，结果显示在右侧列表。
void SourceManagerDialog::onTest()
{
    if (m_currentRow < 0)
        return;
    // 按钮禁用挡不住回车/脚本重复触发；在途时直接忽略第二次请求。
    // 站点体检也会写同一块结果区，必须一起互斥。
    if (m_testBusy || m_probeBusy)
        return;

    const QString skipError = skipIfSyntaxError(m_skipIf->toPlainText());
    if (!skipError.isEmpty()) {
        QMessageBox::warning(this, tr("过滤规则有误"), skipError);
        return;
    }
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
    m_testBusy = true;
    m_testList->clear();
    m_testList->addItem(tr("正在搜索“%1”…").arg(keyword));
    m_btnTest->setEnabled(false);
    updateActions();

    auto *provider = createSearchProvider(draft, m_http, this);
    if (provider == nullptr) {
        m_testBusy = false;
        updateActions();
        m_testList->clear();
        m_testList->addItem(tr("失败：无法创建搜索器"));
        return;
    }
    connect(provider, &SearchProviderBase::finished, this,
            [this, provider](const QString &, const QVector<MediaItem> &items, int filtered,
                             const QString &error) {
                m_testBusy = false;
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
                updateActions();
                provider->deleteLater();
            });
    provider->search(keyword, 20);
}

/// @brief 从 JSON 文件导入源并合并进草稿（同 id 覆盖）。
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

    // 导入文件可能带重复 id；先去重（后者覆盖前者），否则“新增 N 个”会把
    // 同一个 id 重复计数，覆盖统计也会虚高。
    QVector<SourceConfig> deduped;
    int duplicateImports = 0;
    for (const SourceConfig &config : std::as_const(imported)) {
        bool replaced = false;
        for (SourceConfig &existing : deduped) {
            if (existing.id == config.id) {
                existing = config;
                replaced = true;
                ++duplicateImports;
                break;
            }
        }
        if (!replaced)
            deduped.append(config);
    }
    imported = deduped;

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
    int protectedSkipped = 0;
    for (SourceConfig &config : imported) {
        // 导入配置不等于授权执行命令：外部解析器一律先停用，
        // 用户之后在列表里勾选时还会再看到一次完整命令行确认。
        if (config.kind == QLatin1String("external")) {
            config.enabled = false;
            ++externalDisabled;
        }
        // 只有真正的内置实现才允许保留 builtin；普通配置不能靠这个字段
        // 绕过删除限制。
        if (config.kind != QLatin1String("builtin"))
            config.builtin = false;

        bool merged = false;
        for (SourceConfig &existing : m_drafts) {
            if (existing.id != config.id)
                continue;
            // 内置源 ID 不允许被导入文件覆盖，否则可替换内置实现或绕过删除限制。
            if (existing.builtin) {
                ++protectedSkipped;
                merged = true;
                break;
            }
            existing = config;
            merged = true;
            ++replaced;
            break;
        }
        if (!merged)
            m_drafts.append(config);
    }
    reloadList(0);
    QString detail = tr("新增/更新 %1 个源，覆盖同名 %2 个。").arg(imported.size()).arg(replaced);
    if (duplicateImports > 0)
        detail += tr("\n\n导入文件中有 %1 个重复 id，已按后出现的配置合并。")
                      .arg(duplicateImports);
    if (protectedSkipped > 0)
        detail += tr("\n\n其中 %1 个内置源 ID 已保留原配置，导入文件不能覆盖内置源。")
                      .arg(protectedSkipped);
    if (externalDisabled > 0) {
        detail += tr("\n\n其中 %1 个外部解析器已默认停用；"
                     "在列表里勾选时才会再确认命令。")
                      .arg(externalDisabled);
    }
    QMessageBox::information(this, tr("导入完成"), detail);
}

/// @brief 把当前草稿导出为 JSON 文件。
void SourceManagerDialog::onExport()
{
    commitFormToDraft();
    bool hasSecrets = false;
    for (const SourceConfig &config : m_drafts) {
        if (!config.cookie.isEmpty()) {
            hasSecrets = true;
            break;
        }
        for (auto it = config.headers.cbegin(); it != config.headers.cend(); ++it) {
            if (HeaderPolicy::isSensitiveHeader(it.key())) {
                hasSecrets = true;
                break;
            }
        }
        if (!hasSecrets
            && (urlHasSecretQuery(config.urlTemplate)
                || urlHasSecretQuery(config.detailUrlTemplate))) {
            // 有些源的凭据直接放在搜索/详情 URL 的 query 里，导出同样是明文。
            hasSecrets = true;
        }
        if (hasSecrets)
            break;
    }
    if (hasSecrets) {
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this, tr("导出文件包含明文凭据"),
            tr("为了让配置能在另一台机器导入，Cookie/Authorization 等敏感头"
               "或 URL 里的 api_key/token 会以明文写入。"
               "\n\n请只保存到你信任的位置，并在用完后删除。是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes)
            return;
    }

    const QString path = QFileDialog::getSaveFileName(this, tr("导出搜索源"),
                                                     QStringLiteral("sources.json"),
                                                     tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    if (!writeSourcesToFile(m_drafts, path))
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件"));
}

/// @brief 校验过滤规则并提交草稿。
/// @return 成功返回 true；语法错误或写盘失败返回 false，对话框保持打开。
/// @note 草稿中缺失的内置源会自动补回，避免保存时误删；非延迟模式下整表原子替换。
bool SourceManagerDialog::saveAll()
{
    // 手写过滤 JSON 有语法错误时不能静默丢掉这一段；先拦下并指明问题。
    const QString skipError = skipIfSyntaxError(m_skipIf->toPlainText());
    if (!skipError.isEmpty()) {
        QMessageBox::warning(this, tr("过滤规则有误"), skipError);
        return false;
    }
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

/// @brief 原子写出源列表 JSON，避免中断留下半截文件。
bool SourceManagerDialog::writeSourcesToFile(const QVector<SourceConfig> &sources,
                                             const QString &path)
{
    QJsonArray array;
    for (const SourceConfig &config : sources)
        array.append(sourceConfigToJson(config, true));

    const QByteArray payload = QJsonDocument(array).toJson(QJsonDocument::Indented);
    // 导出中途取消/断电不能留下半截 JSON，和项目其它持久化路径统一用原子写。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

/// @brief 从 JSON 数组读取源列表，跳过无法解析的条目。
/// @param sources 输出源列表。
/// @param error 输出失败原因。
/// @return 至少解析出一条有效源时返回 true。
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

/// @brief 确认按钮：保存成功才关闭，失败保留现场供用户重试。
void SourceManagerDialog::accept()
{
    if (!saveAll())
        return; // 写盘失败时保留对话框，用户可改权限/重试，不假装保存成功
    QDialog::accept();
}
