/// @file SourceProbeDialog.cpp
/// @brief 外部解析器批量探测：多站点输入、逐行结果、名称编辑与确认回填。
#include "ui/SourceProbeDialog.h"

#include "ui/ThemeManager.h"
#include "ui_SourceProbeDialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>

namespace {

/// 解析器 stdout 上限：避免异常进程把界面内存撑满。
constexpr int kMaxParserOutputBytes = 2 * 1024 * 1024;
constexpr int kMaxBatchSites = 48;

QString mediaTypeText(MediaType type)
{
    return type == MediaType::Video ? QObject::tr("视频") : QObject::tr("音乐");
}

QUrl normalizeSite(const QString &text)
{
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QUrl();
    if (!trimmed.contains(QStringLiteral("://")))
        trimmed.prepend(QStringLiteral("https://"));
    QUrl url(trimmed);
    if (!url.isValid() || url.host().isEmpty())
        return QUrl();
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        return QUrl();
    return url;
}

QString fallbackIdFromUrl(const QUrl &url)
{
    QString host = url.host().toLower();
    host.remove(QRegularExpression(QStringLiteral("[^a-z0-9]+")));
    return host.isEmpty() ? QStringLiteral("probe_source")
                          : QStringLiteral("probe_") + host;
}

QString formatAttempts(const QJsonArray &attempts)
{
    QStringList lines;
    for (const QJsonValue &value : attempts) {
        const QJsonObject attempt = value.toObject();
        lines.append(QStringLiteral("[%1] %2 — %3")
                         .arg(attempt.value(QStringLiteral("status")).toInt())
                         .arg(attempt.value(QStringLiteral("note")).toString(),
                              attempt.value(QStringLiteral("url")).toString()));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

SourceProbeDialog::SourceProbeDialog(MediaType type, const QString &initialUrl,
                                     const QString &titleText,
                                     const SourceParserSettings &parser, QWidget *parent,
                                     const QString &mode)
    : QDialog(parent)
    , ui(new Ui::SourceProbeDialog)
    , m_parser(parser)
    , m_mode(mode.isEmpty() ? QStringLiteral("add") : mode)
{
    ui->setupUi(this);
    setModal(true);
    setWindowTitle(titleText);
    const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                : QRect(0, 0, 1280, 800);
    resize(qMin(880, available.width() - 40), qMin(640, available.height() - 60));

    bindUi();
    m_type->addItem(tr("视频"), QStringLiteral("video"));
    m_type->addItem(tr("音乐"), QStringLiteral("music"));
    m_type->setCurrentIndex(type == MediaType::Video ? 0 : 1);
    if (!initialUrl.trimmed().isEmpty())
        m_sitesEdit->setPlainText(initialUrl.trimmed());

    ThemeManager::instance().applyWindowChrome(this);

    // 更新入口已有站点，直接开始探测；新增入口留空，等用户粘贴多个站点。
    if (!initialUrl.trimmed().isEmpty())
        QTimer::singleShot(0, this, &SourceProbeDialog::startProbe);
}

SourceProbeDialog::~SourceProbeDialog()
{
    stopProcess();
    delete ui;
}

void SourceProbeDialog::bindUi()
{
    m_sitesEdit = ui->edit_sites;
    m_type = ui->combo_type;
    m_btnProbe = ui->btn_probe;
    m_btnSave = ui->btn_save;
    m_status = ui->label_status;
    m_parserLabel = ui->label_parser;
    m_table = ui->table_results;
    m_log = ui->text_log;

    m_parserLabel->setText(tr("解析器：%1\n参数：%2\n场景：%3")
                               .arg(m_parser.command, m_parser.arguments, m_mode));
    m_parserLabel->setToolTip(tr("参数中的 {site}、{type}、{mode} 会按当前站点、"
                                 "媒体类型和调用场景替换。{url} 是 {site} 的别名。"));

    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({tr("选择"), tr("站点"), tr("名称"),
                                        tr("类型"), tr("状态"), tr("搜索地址 / 原因")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->setColumnWidth(1, 190);
    m_table->setColumnWidth(2, 180);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked
                             | QAbstractItemView::EditKeyPressed);

    connect(m_btnProbe, &QPushButton::clicked, this, &SourceProbeDialog::startProbe);
    connect(m_btnSave, &QPushButton::clicked, this, &SourceProbeDialog::accept);
    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating || item == nullptr)
            return;
        const int row = item->row();
        if (row < 0 || row >= m_rowConfigs.size())
            return;
        if (item->column() == 2 && m_rowConfigs.at(row).isValid()) {
            m_rowConfigs[row].name = item->text().trimmed();
        } else if (item->column() == 0) {
            updateResultActions();
        }
    });
}

void SourceProbeDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                    : QRect(0, 0, 1280, 800);
        const QRect frame = frameGeometry();
        int x = available.center().x() - frame.width() / 2;
        int y = available.center().y() - frame.height() / 2;
        x = qBound(available.left(), x,
                   qMax(available.left(), available.right() - frame.width() + 1));
        y = qBound(available.top(), y,
                   qMax(available.top(), available.bottom() - frame.height() + 1));
        move(x, y);
    });
}

MediaType SourceProbeDialog::currentType() const
{
    return m_type->currentIndex() == 1 ? MediaType::Music : MediaType::Video;
}

int SourceProbeDialog::addResultRow(const QString &siteText, const QString &status)
{
    const int row = m_table->rowCount();
    m_updating = true;
    m_table->insertRow(row);

    auto *check = new QTableWidgetItem();
    check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    check->setCheckState(Qt::Unchecked);
    m_table->setItem(row, 0, check);

    auto *siteItem = new QTableWidgetItem(siteText);
    siteItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, 1, siteItem);

    auto *nameItem = new QTableWidgetItem();
    nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, 2, nameItem);

    auto *typeItem = new QTableWidgetItem();
    typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, 3, typeItem);

    auto *statusItem = new QTableWidgetItem(status);
    statusItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, 4, statusItem);

    auto *detailItem = new QTableWidgetItem();
    detailItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, 5, detailItem);

    m_rowConfigs.append(SourceConfig());
    m_updating = false;
    return row;
}

void SourceProbeDialog::setRowStatus(int row, const QString &status, const QString &detail)
{
    if (row < 0 || row >= m_table->rowCount())
        return;
    m_updating = true;
    if (QTableWidgetItem *item = m_table->item(row, 4))
        item->setText(status);
    if (QTableWidgetItem *item = m_table->item(row, 5)) {
        item->setText(detail);
        item->setToolTip(detail);
    }
    m_updating = false;
}

void SourceProbeDialog::startProbe()
{
    if (m_busy)
        return;
    if (m_parser.command.trimmed().isEmpty()) {
        showFailure(tr("还没有配置外部解析器。请先在源管理里配置解析器命令。"));
        return;
    }

    QStringList lines;
    QSet<QString> seen;
    const QStringList rawLines = m_sitesEdit->toPlainText().split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &raw : rawLines) {
        const QString text = raw.trimmed();
        if (text.isEmpty())
            continue;
        const QString key = text.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        lines.append(text);
        if (lines.size() >= kMaxBatchSites)
            break;
    }
    if (lines.isEmpty()) {
        showFailure(tr("先输入至少一个网站地址，每行一个。"));
        return;
    }
    if (rawLines.size() > kMaxBatchSites)
        m_log->setPlainText(tr("单次最多探测 %1 个站点，超出部分已忽略。").arg(kMaxBatchSites));

    m_updating = true;
    m_table->setRowCount(0);
    m_updating = false;
    m_rowConfigs.clear();
    m_sites.clear();
    m_siteRows.clear();
    m_configs.clear();
    m_nextSite = 0;
    m_output.clear();
    m_stderr.clear();
    m_outputOverflow = false;
    m_log->clear();
    m_btnSave->setEnabled(false);
    m_btnProbe->setEnabled(false);
    m_status->setText(tr("正在整理站点…"));

    for (const QString &line : lines) {
        const QUrl url = normalizeSite(line);
        if (!url.isValid()) {
            const int row = addResultRow(line, tr("地址无效"));
            setRowStatus(row, tr("失败：地址无效"),
                         tr("只支持 http/https 站点地址"));
            continue;
        }
        const int row = addResultRow(line, tr("等待探测"));
        if (QTableWidgetItem *item = m_table->item(row, 3))
            item->setText(mediaTypeText(currentType()));
        m_sites.append(url);
        m_siteRows.append(row);
    }

    if (m_sites.isEmpty()) {
        m_busy = false;
        m_btnProbe->setEnabled(true);
        m_status->setText(tr("没有可探测的有效站点。"));
        updateResultActions();
        return;
    }

    m_busy = true;
    startNextSite();
}

void SourceProbeDialog::startNextSite()
{
    if (m_nextSite >= m_sites.size()) {
        m_busy = false;
        m_btnProbe->setEnabled(true);
        int successCount = 0;
        for (const SourceConfig &config : m_rowConfigs) {
            if (config.isValid())
                ++successCount;
        }
        m_status->setText(tr("探测完成：共 %1 个站点，成功 %2 个。")
                              .arg(m_sites.size())
                              .arg(successCount));
        updateResultActions();
        return;
    }

    // 每个站点都要重新置为忙碌状态：上一站点的 finishCurrentProbe/onTimeout
    // 会把 m_busy 清掉，若只靠 startProbe 开头设置，第二站点的 finished 会被
    // 直接 return，界面就永远停在“正在探测 2/2”。
    m_busy = true;
    m_btnProbe->setEnabled(false);
    updateResultActions();

    const int row = m_siteRows.at(m_nextSite);
    setRowStatus(row, tr("探测中…"), tr("正在调用外部解析器"));
    m_status->setText(tr("正在探测 %1/%2：%3")
                          .arg(m_nextSite + 1)
                          .arg(m_sites.size())
                          .arg(m_sites.at(m_nextSite).toString()));
    startSiteProbe(m_sites.at(m_nextSite));
}

void SourceProbeDialog::startSiteProbe(const QUrl &site)
{
    stopProcess();
    m_output.clear();
    m_stderr.clear();
    m_outputOverflow = false;

    QString program = m_parser.command.trimmed();
    const QFileInfo programInfo(program);
    if (programInfo.isRelative()) {
        const QDir appDir(QCoreApplication::applicationDirPath());
        const QString local = appDir.filePath(program);
        if (QFileInfo::exists(local))
            program = local;
        else if (program.contains(QLatin1Char('/')))
            program = local;
    }

    const QString typeText = currentType() == MediaType::Video ? QStringLiteral("video")
                                                               : QStringLiteral("music");
    QStringList args = QProcess::splitCommand(m_parser.arguments);
    for (QString &arg : args) {
        arg.replace(QStringLiteral("{site}"), site.toString());
        arg.replace(QStringLiteral("{url}"), site.toString());
        arg.replace(QStringLiteral("{type}"), typeText);
        arg.replace(QStringLiteral("{mode}"), m_mode);
    }

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MVP_SOURCE_SITE"), site.toString());
    env.insert(QStringLiteral("MVP_SOURCE_TYPE"), typeText);
    env.insert(QStringLiteral("MVP_SOURCE_MODE"), m_mode);
    m_process->setProcessEnvironment(env);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &SourceProbeDialog::readOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &SourceProbeDialog::readOutput);
    connect(m_process, &QProcess::errorOccurred, this, &SourceProbeDialog::onProcessError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) { finishCurrentProbe(); });

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &SourceProbeDialog::onTimeout);
    m_timeout->start(qMax(5000, m_parser.timeoutMs));

    m_process->start(program, args);
}

void SourceProbeDialog::finishCurrentProbe()
{
    if (!m_busy)
        return;

    m_busy = false;
    if (m_timeout != nullptr) {
        m_timeout->stop();
        m_timeout->deleteLater();
        m_timeout = nullptr;
    }

    QProcess *process = m_process;
    m_process = nullptr;
    if (process == nullptr) {
        // 只在被 stopProcess 主动终止后走到这里；继续下一个站点。
        m_nextSite++;
        QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
        return;
    }

    m_output.append(process->readAllStandardOutput());
    m_stderr.append(process->readAllStandardError());
    const bool failedToStart = process->error() == QProcess::FailedToStart;
    const QProcess::ExitStatus exitStatus = process->exitStatus();
    process->deleteLater();

    if (failedToStart) {
        showFailure(tr("解析器启动失败：%1").arg(m_parser.command));
        return;
    }

    const int row = m_siteRows.at(m_nextSite);
    const QString output = QString::fromUtf8(m_output).trimmed();
    if (exitStatus == QProcess::CrashExit && output.isEmpty()) {
        setRowStatus(row, tr("失败：解析器异常退出"),
                     QString::fromUtf8(m_stderr).trimmed());
        m_nextSite++;
        QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(m_output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QString detail = output;
        const QString err = QString::fromUtf8(m_stderr).trimmed();
        if (!err.isEmpty())
            detail += (detail.isEmpty() ? QString() : QStringLiteral("\n")) + err;
        setRowStatus(row, tr("失败：输出不是 JSON"),
                     detail.isEmpty() ? tr("（没有输出）") : detail);
        m_nextSite++;
        QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
        return;
    }

    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("ok")).toBool(true)) {
        setRowStatus(row, tr("失败：%1").arg(reasonFromJson(root)), output);
        m_nextSite++;
        QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
        return;
    }

    QJsonObject sourceObject;
    for (const QString &key : {QStringLiteral("source"), QStringLiteral("config"),
                               QStringLiteral("result")}) {
        if (root.value(key).isObject()) {
            sourceObject = root.value(key).toObject();
            break;
        }
    }
    if (sourceObject.isEmpty())
        sourceObject = root;

    bool parsed = false;
    SourceConfig config = configFromJsonObject(sourceObject, &parsed);
    if (!parsed) {
        setRowStatus(row, tr("失败：source 不是有效配置"), output);
        m_nextSite++;
        QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
        return;
    }

    if (config.id.trimmed().isEmpty())
        config.id = fallbackIdFromUrl(m_sites.at(m_nextSite));
    if (config.name.trimmed().isEmpty())
        config.name = root.value(QStringLiteral("siteName")).toString().trimmed();
    if (config.name.trimmed().isEmpty())
        config.name = m_sites.at(m_nextSite).host();
    config.enabled = true;
    config.builtin = false;
    if (!config.isValid()) {
        setRowStatus(row, tr("失败：配置不可用"),
                     config.validationError() + QLatin1Char('\n') + output);
        m_nextSite++;
        QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
        return;
    }

    m_updating = true;
    m_rowConfigs[row] = config;
    if (QTableWidgetItem *item = m_table->item(row, 2)) {
        item->setText(config.name);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
    }
    if (QTableWidgetItem *item = m_table->item(row, 3))
        item->setText(mediaTypeText(config.mediaType));
    if (QTableWidgetItem *item = m_table->item(row, 0)) {
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    m_updating = false;
    setRowStatus(row, tr("成功"), config.urlTemplate);

    QStringList logLines;
    logLines.append(tr("站点：%1").arg(m_sites.at(m_nextSite).toString()));
    logLines.append(tr("解析器返回的源配置："));
    logLines.append(QString::fromUtf8(
        QJsonDocument(sourceConfigToJson(config, true)).toJson(QJsonDocument::Indented)));
    const QJsonArray samples = root.value(QStringLiteral("samples")).toArray();
    if (!samples.isEmpty()) {
        logLines.append(tr("试搜样例："));
        for (const QJsonValue &sample : samples)
            logLines.append(QStringLiteral("· ") + sample.toString());
    }
    const QString attempts = formatAttempts(root.value(QStringLiteral("attempts")).toArray());
    if (!attempts.isEmpty()) {
        logLines.append(tr("探测记录："));
        logLines.append(attempts);
    }
    if (!m_log->toPlainText().isEmpty())
        m_log->appendPlainText(QString());
    m_log->appendPlainText(logLines.join(QLatin1Char('\n')));

    m_nextSite++;
    QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
}

void SourceProbeDialog::onTimeout()
{
    if (!m_busy)
        return;
    m_busy = false;
    const int row = m_siteRows.value(m_nextSite, -1);
    setRowStatus(row, tr("失败：探测超时"),
                 tr("超过 %1 秒没有返回").arg(m_parser.timeoutMs / 1000));
    stopProcess();
    m_nextSite++;
    QTimer::singleShot(0, this, &SourceProbeDialog::startNextSite);
}

void SourceProbeDialog::readOutput()
{
    if (m_process == nullptr || m_outputOverflow)
        return;
    m_output.append(m_process->readAllStandardOutput());
    m_stderr.append(m_process->readAllStandardError());
    if (m_output.size() + m_stderr.size() > kMaxParserOutputBytes) {
        m_outputOverflow = true;
        showFailure(tr("解析器输出超过 2 MB，已结束本轮探测。"));
    }
}

void SourceProbeDialog::onProcessError(QProcess::ProcessError error)
{
    if (!m_busy)
        return;
    if (error == QProcess::FailedToStart) {
        m_busy = false;
        stopProcess();
        showFailure(tr("解析器启动失败：%1").arg(m_parser.command));
    }
    // 其它错误交给 finished() 统一收尾，避免同一次失败进两次。
}

void SourceProbeDialog::stopProcess()
{
    if (m_timeout != nullptr) {
        m_timeout->stop();
        m_timeout->deleteLater();
        m_timeout = nullptr;
    }
    if (m_process == nullptr)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(2000);
    }
    process->deleteLater();
}

void SourceProbeDialog::showFailure(const QString &error, const QString &detail)
{
    stopProcess();
    m_busy = false;
    m_configs.clear();
    m_btnSave->setEnabled(false);
    m_btnProbe->setEnabled(true);
    m_status->setText(tr("探测失败：%1").arg(error));
    if (!detail.trimmed().isEmpty())
        m_log->setPlainText(detail);
    updateResultActions();
}

QString SourceProbeDialog::reasonFromJson(const QJsonObject &root) const
{
    const QString error = root.value(QStringLiteral("error")).toString().trimmed();
    if (!error.isEmpty())
        return error;
    return tr("解析器返回 ok=false，但没有给出失败原因。");
}

SourceConfig SourceProbeDialog::configFromJsonObject(const QJsonObject &object, bool *ok) const
{
    bool parsed = false;
    SourceConfig config = sourceConfigFromJson(object, &parsed);
    if (!parsed && object.contains(QStringLiteral("url"))) {
        QJsonObject normalized = object;
        normalized.insert(QStringLiteral("urlTemplate"),
                          object.value(QStringLiteral("url")));
        normalized.remove(QStringLiteral("url"));
        config = sourceConfigFromJson(normalized, &parsed);
    }
    if (ok != nullptr)
        *ok = parsed;
    return config;
}

void SourceProbeDialog::updateResultActions()
{
    bool hasSelected = false;
    for (int row = 0; row < m_rowConfigs.size(); ++row) {
        if (!m_rowConfigs.at(row).isValid())
            continue;
        const QTableWidgetItem *check = m_table->item(row, 0);
        if (check != nullptr && check->checkState() == Qt::Checked) {
            hasSelected = true;
            break;
        }
    }
    m_btnSave->setEnabled(!m_busy && hasSelected);
    m_btnProbe->setEnabled(!m_busy);
}

void SourceProbeDialog::accept()
{
    if (m_busy) {
        QMessageBox::information(this, tr("正在探测"), tr("请等待当前探测结束。"));
        return;
    }

    QVector<SourceConfig> selected;
    for (int row = 0; row < m_rowConfigs.size(); ++row) {
        const QTableWidgetItem *check = m_table->item(row, 0);
        if (check == nullptr || check->checkState() != Qt::Checked)
            continue;
        SourceConfig config = m_rowConfigs.at(row);
        if (!config.isValid())
            continue;
        if (QTableWidgetItem *nameItem = m_table->item(row, 2)) {
            const QString name = nameItem->text().trimmed();
            if (!name.isEmpty())
                config.name = name;
        }
        if (config.name.trimmed().isEmpty()) {
            const QTableWidgetItem *siteItem = m_table->item(row, 1);
            const QUrl site = siteItem != nullptr ? normalizeSite(siteItem->text()) : QUrl();
            config.name = site.host();
        }
        config.enabled = true;
        config.builtin = false;
        if (config.isValid())
            selected.append(config);
    }

    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("没有可加入的源"),
                             tr("请至少勾选一个探测成功的站点。"));
        return;
    }
    if (m_mode == QLatin1String("update") && selected.size() > 1) {
        QMessageBox::warning(this, tr("更新模式"),
                             tr("更新一次只能选择一个站点，请只保留一行勾选。"));
        return;
    }

    m_configs = selected;
    QDialog::accept();
}
