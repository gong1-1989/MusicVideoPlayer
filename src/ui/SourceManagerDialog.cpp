#include "ui/SourceManagerDialog.h"

#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SearchProviderBase.h"
#include "net/SourceRegistry.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

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
                                         QWidget *parent)
    : QDialog(parent)
    , m_registry(registry)
    , m_http(http)
{
    setObjectName(QStringLiteral("SourceManagerDialog"));
    setWindowTitle(tr("搜索源管理"));
    setModal(true);

    // 表单内容比小屏高，先按可用工作区收一收，右侧表单内部自己滚动
    const QRect available = QGuiApplication::primaryScreen()
                                ? QGuiApplication::primaryScreen()->availableGeometry()
                                : QRect(0, 0, 1280, 800);
    resize(qMin(900, available.width() - 40), qMin(640, available.height() - 60));

    if (m_registry != nullptr)
        m_drafts = m_registry->sources();

    buildUi();
    reloadList(m_drafts.isEmpty() ? -1 : 0);
}

void SourceManagerDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);

    auto *title = new QLabel(tr("搜索源"), this);
    title->setObjectName(QStringLiteral("label_sectionTitle"));
    title->setStyleSheet(QStringLiteral("font-size:16px;"));
    root->addWidget(title);

    auto *hint = new QLabel(
        tr("每个源是一份配置：搜索地址 + 结果路径 + 字段对应，填好后用“试搜”确认。"
           "需要签名的站点（B站、QQ音乐等）无法用配置描述。"),
        this);
    hint->setObjectName(QStringLiteral("label_hint"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // ---------------------------------------------------------- 左：列表
    auto *leftBox = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftBox);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    m_list = new QListWidget(leftBox);
    m_list->setMinimumWidth(240);
    leftLayout->addWidget(m_list, 1);

    auto *listButtons = new QHBoxLayout;
    listButtons->setSpacing(8);
    auto *btnAdd = new QPushButton(tr("新增"), leftBox);
    m_btnDuplicate = new QPushButton(tr("复制"), leftBox);
    m_btnRemove = new QPushButton(tr("删除"), leftBox);
    listButtons->addWidget(btnAdd);
    listButtons->addWidget(m_btnDuplicate);
    listButtons->addWidget(m_btnRemove);
    leftLayout->addLayout(listButtons);

    auto *listButtons2 = new QHBoxLayout;
    auto *btnImport = new QPushButton(tr("导入"), leftBox);
    auto *btnExport = new QPushButton(tr("导出"), leftBox);
    listButtons2->addWidget(btnImport);
    listButtons2->addWidget(btnExport);
    leftLayout->addLayout(listButtons2);

    splitter->addWidget(leftBox);

    // ---------------------------------------------------------- 右：编辑
    auto *rightBox = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightBox);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);

    auto *basicGroup = new QGroupBox(tr("基本信息"), rightBox);
    auto *basicForm = new QFormLayout(basicGroup);
    basicForm->setHorizontalSpacing(12);
    basicForm->setVerticalSpacing(6);

    m_name = new QLineEdit(basicGroup);
    basicForm->addRow(tr("名称"), m_name);

    m_type = new QComboBox(basicGroup);
    m_type->addItem(tr("音乐"), QStringLiteral("music"));
    m_type->addItem(tr("视频"), QStringLiteral("video"));
    basicForm->addRow(tr("类型"), m_type);

    m_kind = new QComboBox(basicGroup);
    m_kind->addItem(tr("配置驱动（JSON 接口）"), QStringLiteral("config"));
    m_kind->addItem(tr("内置实现（需要签名）"), QStringLiteral("builtin"));
    m_kind->addItem(tr("外部解析器进程"), QStringLiteral("external"));
    basicForm->addRow(tr("实现方式"), m_kind);

    m_builtinRow = new QWidget(basicGroup);
    auto *builtinLayout = new QHBoxLayout(m_builtinRow);
    builtinLayout->setContentsMargins(0, 0, 0, 0);
    builtinLayout->setSpacing(8);
    m_provider = new QComboBox(m_builtinRow);
    m_provider->addItem(tr("B站"), QStringLiteral("bilibili"));
    builtinLayout->addWidget(m_provider, 1);
    auto *builtinHint = new QLabel(tr("搜索走 wbi 签名，播放取单文件 mp4"), m_builtinRow);
    builtinHint->setObjectName(QStringLiteral("label_hint"));
    builtinLayout->addWidget(builtinHint, 2);
    basicForm->addRow(tr("内置实现"), m_builtinRow);

    rightLayout->addWidget(basicGroup);

    m_requestGroup = new QGroupBox(tr("请求"), rightBox);
    auto *form = new QFormLayout(m_requestGroup);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);

    m_method = new QComboBox(m_requestGroup);
    m_method->addItem(QStringLiteral("GET"), QStringLiteral("GET"));
    m_method->addItem(QStringLiteral("POST"), QStringLiteral("POST"));
    form->addRow(tr("请求方式"), m_method);

    m_url = new QLineEdit(m_requestGroup);
    m_url->setPlaceholderText(
        tr("https://example.com/search?term={keyword}&limit={limit}"));
    form->addRow(tr("搜索地址"), m_url);

    m_body = new QPlainTextEdit(m_requestGroup);
    m_body->setFixedHeight(36);
    m_body->setPlaceholderText(tr("POST 时的表单内容，可使用 {keyword} {limit}"));
    form->addRow(tr("请求体"), m_body);

    m_headers = new QPlainTextEdit(m_requestGroup);
    m_headers->setFixedHeight(52);
    m_headers->setPlaceholderText(tr("每行一个：键: 值\nReferer: https://example.com/"));
    form->addRow(tr("请求头"), m_headers);

    m_cookie = new QLineEdit(m_requestGroup);
    m_cookie->setPlaceholderText(tr("可选，例如 MUSIC_U=xxxx"));
    form->addRow(tr("Cookie"), m_cookie);

    m_itemsPath = new QLineEdit(m_requestGroup);
    m_itemsPath->setPlaceholderText(tr("结果数组在响应里的路径，例如 result.songs；留空表示根就是数组"));
    form->addRow(tr("结果路径"), m_itemsPath);

    rightLayout->addWidget(m_requestGroup);

    m_fieldsGroup = new QGroupBox(tr("字段对应"), rightBox);
    auto *fieldsForm = new QFormLayout(m_fieldsGroup);
    fieldsForm->setHorizontalSpacing(12);
    fieldsForm->setVerticalSpacing(6);

    m_idField = new QLineEdit(m_fieldsGroup);
    m_idField->setPlaceholderText(tr("id（用于拼播放地址）"));
    m_titleField = new QLineEdit(m_fieldsGroup);
    m_titleField->setPlaceholderText(tr("歌名/视频名"));
    m_artistField = new QLineEdit(m_fieldsGroup);
    m_artistField->setPlaceholderText(tr("作者/歌手，可空"));
    m_durationField = new QLineEdit(m_fieldsGroup);
    m_durationField->setPlaceholderText(tr("时长，毫秒；可空"));
    m_streamField = new QLineEdit(m_fieldsGroup);
    m_streamField->setPlaceholderText(
        tr("播放直链字段；需要拼地址时写 @https://host/play?id={id}"));
    m_lyricField = new QLineEdit(m_fieldsGroup);
    m_lyricField->setPlaceholderText(tr("歌词地址，可空"));
    m_webField = new QLineEdit(m_fieldsGroup);
    m_webField->setPlaceholderText(tr("原网页地址，可空"));

    fieldsForm->addRow(tr("id"), m_idField);
    fieldsForm->addRow(tr("标题"), m_titleField);
    fieldsForm->addRow(tr("作者"), m_artistField);
    fieldsForm->addRow(tr("时长"), m_durationField);
    fieldsForm->addRow(tr("播放地址"), m_streamField);
    fieldsForm->addRow(tr("歌词地址"), m_lyricField);
    fieldsForm->addRow(tr("网页地址"), m_webField);
    rightLayout->addWidget(m_fieldsGroup);

    m_filterGroup = new QGroupBox(tr("过滤规则（可选）"), rightBox);
    auto *filterLayout = new QVBoxLayout(m_filterGroup);
    m_skipIf = new QPlainTextEdit(m_filterGroup);
    m_skipIf->setFixedHeight(64);
    m_skipIf->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_skipIf->setPlaceholderText(
        tr("命中规则的条目会被跳过，例如：\n"
           "{ \"any\": [ { \"path\": \"fee\", \"op\": \"eq\", \"value\": 1 } ] }\n"
           "op 可选 eq / ne / gt / gte / lt / lte / empty / notEmpty / contains"));
    filterLayout->addWidget(m_skipIf);
    rightLayout->addWidget(m_filterGroup);

    m_externalGroup = new QGroupBox(tr("外部解析器"), rightBox);
    auto *externalForm = new QFormLayout(m_externalGroup);
    externalForm->setHorizontalSpacing(12);
    externalForm->setVerticalSpacing(6);

    m_command = new QLineEdit(m_externalGroup);
    m_command->setPlaceholderText(
        tr("python、node，或程序目录下的脚本 / 绝对路径的 exe"));
    externalForm->addRow(tr("命令"), m_command);

    m_arguments = new QLineEdit(m_externalGroup);
    m_arguments->setPlaceholderText(
        tr("参数，可用 {keyword} {limit}，例如 --keyword={keyword} --limit={limit}"));
    externalForm->addRow(tr("参数"), m_arguments);

    m_timeout = new QSpinBox(m_externalGroup);
    m_timeout->setRange(1000, 60000);
    m_timeout->setSingleStep(500);
    m_timeout->setSuffix(tr(" 毫秒"));
    externalForm->addRow(tr("超时"), m_timeout);

    auto *externalHint = new QLabel(
        tr("解析器把结果以 JSON 打到 stdout：\n"
           "[ { \"title\": \"...\", \"artist\": \"...\", \"streamUrl\": \"...\" } ]\n"
           "也可以输出 { \"items\": [...], \"filtered\": 2 }；报错时退出码非 0 或输出 "
           "{ \"error\": \"原因\" }。"),
        m_externalGroup);
    externalHint->setObjectName(QStringLiteral("label_hint"));
    externalHint->setWordWrap(true);
    externalForm->addRow(externalHint);

    rightLayout->addWidget(m_externalGroup);
    rightLayout->addStretch(1);

    connect(m_kind, &QComboBox::currentIndexChanged, this,
            [this](int) { updateKindUi(); });

    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(rightBox);

    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({250, 640});
    root->addWidget(splitter, 1);

    // ---------------------------------------------------------- 试搜
    auto *testRow = new QHBoxLayout;
    testRow->setSpacing(8);
    m_testKeyword = new QLineEdit(this);
    m_testKeyword->setPlaceholderText(tr("试搜关键词"));
    m_btnTest = new QPushButton(tr("试搜当前源"), this);
    testRow->addWidget(new QLabel(tr("试搜"), this));
    testRow->addWidget(m_testKeyword, 1);
    testRow->addWidget(m_btnTest);
    root->addLayout(testRow);

    m_testList = new QListWidget(this);
    m_testList->setMaximumHeight(84);
    m_testList->setMinimumHeight(56);
    root->addWidget(m_testList);

    auto *buttons = new QDialogButtonBox(this);
    auto *cancelButton = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    auto *saveButton = buttons->addButton(tr("保存"), QDialogButtonBox::AcceptRole);
    saveButton->setObjectName(QStringLiteral("btn_applyPrimary"));
    saveButton->setDefault(true);
    root->addWidget(buttons);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, this, &SourceManagerDialog::accept);
    connect(btnAdd, &QPushButton::clicked, this, &SourceManagerDialog::onAdd);
    connect(m_btnDuplicate, &QPushButton::clicked, this, &SourceManagerDialog::onDuplicate);
    connect(m_btnRemove, &QPushButton::clicked, this, &SourceManagerDialog::onRemove);
    connect(btnImport, &QPushButton::clicked, this, &SourceManagerDialog::onImport);
    connect(btnExport, &QPushButton::clicked, this, &SourceManagerDialog::onExport);
    connect(m_btnTest, &QPushButton::clicked, this, &SourceManagerDialog::onTest);

    connect(m_list, &QListWidget::currentRowChanged, this, &SourceManagerDialog::selectRow);
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_updating || item == nullptr)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        for (SourceConfig &config : m_drafts) {
            if (config.id == id)
                config.enabled = item->checkState() == Qt::Checked;
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
            text += tr(" · 配置不完整");
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, config.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(config.enabled ? Qt::Checked : Qt::Unchecked);
        if (!config.isValid())
            item->setForeground(Qt::red);
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

    m_builtinRow->setVisible(isBuiltin);
    m_requestGroup->setVisible(isConfig);
    m_fieldsGroup->setVisible(isConfig);
    m_filterGroup->setVisible(isConfig);
    m_externalGroup->setVisible(isExternal);
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
    config.kind = m_kind->currentData().toString();
    config.provider = m_provider->currentData().toString();
    if (config.kind == QLatin1String("external")) {
        config.command = m_command->text().trimmed();
        config.arguments = m_arguments->text();
        config.timeoutMs = m_timeout->value();
    }
    config.method = m_method->currentData().toString();
    config.urlTemplate = m_url->text().trimmed();
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

    commitFormToDraft();
    int replaced = 0;
    for (const SourceConfig &config : imported) {
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
    QMessageBox::information(this, tr("导入完成"),
                             tr("新增/更新 %1 个源，覆盖同名 %2 个。").arg(imported.size()).arg(replaced));
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

void SourceManagerDialog::saveAll()
{
    commitFormToDraft();
    if (m_registry == nullptr)
        return;

    for (const SourceConfig &config : m_drafts)
        m_registry->upsert(config);

    // 删掉草稿里已经不存在的源
    const QVector<SourceConfig> existing = m_registry->sources();
    for (const SourceConfig &config : existing) {
        bool kept = false;
        for (const SourceConfig &draft : m_drafts) {
            if (draft.id == config.id) {
                kept = true;
                break;
            }
        }
        if (!kept && !config.builtin)
            m_registry->remove(config.id);
    }
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
    saveAll();
    QDialog::accept();
}
