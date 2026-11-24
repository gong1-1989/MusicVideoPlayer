/// @file SourceProbeDialog.cpp
/// @brief 外部解析器探测窗口：调用进程、解析 JSON、确认回填配置。
#include "ui/SourceProbeDialog.h"

#include "ui/ThemeManager.h"
#include "ui_SourceProbeDialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>

namespace {

/// 解析器 stdout 上限：避免异常进程把界面内存撑满。
constexpr int kMaxParserOutputBytes = 2 * 1024 * 1024;

QString mediaTypeText(MediaType type)
{
    return type == MediaType::Video ? QObject::tr("视频") : QObject::tr("音乐");
}

QString kindText(const SourceConfig &config)
{
    if (config.kind == QLatin1String("builtin"))
        return QObject::tr("内置抓取（网页型影视）");
    if (config.kind == QLatin1String("external"))
        return QObject::tr("外部解析器进程");
    return QObject::tr("配置驱动（JSON 接口）");
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
                                     const SourceParserSettings &parser, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SourceProbeDialog)
    , m_parser(parser)
{
    ui->setupUi(this);
    setModal(true);
    setWindowTitle(titleText);
    const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                : QRect(0, 0, 1280, 800);
    resize(qMin(760, available.width() - 40), qMin(560, available.height() - 60));

    bindUi();
    m_type->addItem(tr("视频"), QStringLiteral("video"));
    m_type->addItem(tr("音乐"), QStringLiteral("music"));
    m_type->setCurrentIndex(type == MediaType::Video ? 0 : 1);
    m_url->setText(initialUrl);

    ThemeManager::instance().applyWindowChrome(this);

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
    m_url = ui->edit_url;
    m_type = ui->combo_type;
    m_btnProbe = ui->btn_probe;
    m_btnSave = ui->btn_save;
    m_status = ui->label_status;
    m_parserLabel = ui->label_parser;
    m_nameLabel = ui->label_name;
    m_typeLabel = ui->label_type;
    m_urlLabel = ui->label_url;
    m_kindLabel = ui->label_kind;
    m_log = ui->text_log;

    m_parserLabel->setText(tr("解析器：%1\n参数：%2")
                               .arg(m_parser.command, m_parser.arguments));

    connect(m_btnProbe, &QPushButton::clicked, this, &SourceProbeDialog::startProbe);
    connect(m_btnSave, &QPushButton::clicked, this, &QDialog::accept);
    connect(ui->btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_url, &QLineEdit::returnPressed, this, &SourceProbeDialog::startProbe);
    connect(this, &QDialog::finished, this, &SourceProbeDialog::stopProcess);
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

void SourceProbeDialog::startProbe()
{
    if (m_busy)
        return;

    const QString text = m_url->text().trimmed();
    if (text.isEmpty()) {
        showFailure(tr("先填一个网站地址。"));
        return;
    }

    QUrl url(text);
    if (url.scheme().isEmpty())
        url = QUrl(QStringLiteral("https://") + text);
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        showFailure(tr("网址必须以 http:// 或 https:// 开头。"));
        return;
    }
    if (m_parser.command.trimmed().isEmpty()) {
        showFailure(tr("还没有配置外部解析器。请先在源管理里填写解析器命令。"));
        return;
    }

    stopProcess();
    m_busy = true;
    m_hasResult = false;
    m_output.clear();
    m_stderr.clear();
    m_outputOverflow = false;
    m_btnProbe->setEnabled(false);
    m_btnSave->setEnabled(false);
    m_btnSave->setText(tr("确认使用"));
    m_nameLabel->clear();
    m_typeLabel->clear();
    m_urlLabel->clear();
    m_kindLabel->clear();
    m_log->clear();
    m_status->setText(tr("正在调用外部解析器探测 %1 …").arg(url.toString()));

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
        arg.replace(QStringLiteral("{site}"), url.toString());
        arg.replace(QStringLiteral("{url}"), url.toString());
        arg.replace(QStringLiteral("{type}"), typeText);
        arg.replace(QStringLiteral("{mode}"), QStringLiteral("add"));
    }

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MVP_SOURCE_SITE"), url.toString());
    env.insert(QStringLiteral("MVP_SOURCE_TYPE"), typeText);
    env.insert(QStringLiteral("MVP_SOURCE_MODE"), QStringLiteral("add"));
    m_process->setProcessEnvironment(env);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &SourceProbeDialog::readOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &SourceProbeDialog::readOutput);
    connect(m_process, &QProcess::errorOccurred, this, &SourceProbeDialog::onProcessError);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) { finishProbe(); });

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        m_busy = false;
        stopProcess();
        showFailure(tr("解析器超过 %1 秒没有返回，已结束进程。")
                        .arg(m_parser.timeoutMs / 1000));
    });
    m_timeout->start(qMax(5000, m_parser.timeoutMs));

    m_process->start(program, args);
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

void SourceProbeDialog::readOutput()
{
    if (m_process == nullptr || m_outputOverflow)
        return;
    m_output.append(m_process->readAllStandardOutput());
    m_stderr.append(m_process->readAllStandardError());
    if (m_output.size() + m_stderr.size() > kMaxParserOutputBytes) {
        m_outputOverflow = true;
        m_busy = false;
        stopProcess();
        showFailure(tr("解析器输出超过 2 MB，已结束进程。"));
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
        return;
    }
    // 其它错误交给 finished() 统一收尾，避免同一次失败弹两次。
}

void SourceProbeDialog::finishProbe()
{
    if (!m_busy)
        return;
    m_busy = false;
    m_btnProbe->setEnabled(true);

    if (m_timeout != nullptr) {
        m_timeout->stop();
        m_timeout->deleteLater();
        m_timeout = nullptr;
    }
    if (m_process != nullptr) {
        m_output.append(m_process->readAllStandardOutput());
        m_stderr.append(m_process->readAllStandardError());
        const bool failedToStart = m_process->error() == QProcess::FailedToStart;
        const QProcess::ExitStatus exitStatus = m_process->exitStatus();
        const int exitCode = m_process->exitCode();
        m_process->deleteLater();
        m_process = nullptr;
        if (failedToStart) {
            showFailure(tr("解析器启动失败：%1").arg(m_parser.command));
            return;
        }
        if (exitStatus == QProcess::CrashExit && m_output.trimmed().isEmpty()) {
            showFailure(tr("解析器异常退出。"), QString::fromUtf8(m_stderr));
            return;
        }
        Q_UNUSED(exitCode);
    }

    if (m_outputOverflow)
        return;

    const QString output = QString::fromUtf8(m_output).trimmed();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(m_output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QString detail = output;
        const QString err = QString::fromUtf8(m_stderr).trimmed();
        if (!err.isEmpty())
            detail += (detail.isEmpty() ? QString() : QStringLiteral("\n")) + err;
        showFailure(tr("解析器输出不是有效的 JSON 对象。"),
                    detail.isEmpty() ? tr("（没有输出）") : detail);
        return;
    }

    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("ok")).toBool(true)) {
        showFailure(reasonFromJson(root), output);
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
        showFailure(tr("解析器返回的 source 不是有效配置。"), output);
        return;
    }

    if (config.id.trimmed().isEmpty())
        config.id = fallbackIdFromUrl(QUrl(m_url->text().trimmed()));
    if (config.name.trimmed().isEmpty())
        config.name = root.value(QStringLiteral("siteName")).toString().trimmed();
    if (config.name.trimmed().isEmpty())
        config.name = QUrl(m_url->text().trimmed()).host();
    config.enabled = true;
    config.builtin = false;
    if (!config.isValid()) {
        showFailure(tr("解析器返回的配置不可用：%1").arg(config.validationError()), output);
        return;
    }

    showSuccess(config, root);
}

void SourceProbeDialog::showFailure(const QString &error, const QString &detail)
{
    m_hasResult = false;
    m_btnSave->setEnabled(false);
    m_status->setText(tr("探测失败：%1").arg(error));
    if (!detail.trimmed().isEmpty())
        m_log->setPlainText(detail);
    m_btnProbe->setEnabled(true);
}

void SourceProbeDialog::showSuccess(const SourceConfig &config, const QJsonObject &root)
{
    m_config = config;
    m_hasResult = true;

    if (config.mediaType == MediaType::Video)
        m_type->setCurrentIndex(0);
    else
        m_type->setCurrentIndex(1);

    QString status = tr("探测成功：已由外部解析器返回可用配置。");
    const QString note = root.value(QStringLiteral("validationNote")).toString();
    if (!note.isEmpty())
        status += QLatin1Char(' ') + note;
    m_status->setText(status);

    m_nameLabel->setText(config.name);
    m_typeLabel->setText(mediaTypeText(config.mediaType));
    m_urlLabel->setText(config.urlTemplate);
    m_kindLabel->setText(kindText(config));

    QStringList logLines;
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
    m_log->setPlainText(logLines.join(QLatin1Char('\n')));

    m_btnSave->setEnabled(true);
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

void SourceProbeDialog::closeEvent(QCloseEvent *event)
{
    stopProcess();
    QDialog::closeEvent(event);
}
