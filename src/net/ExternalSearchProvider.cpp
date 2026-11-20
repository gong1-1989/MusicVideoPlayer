#include "net/ExternalSearchProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>
#include <QUrl>

Q_LOGGING_CATEGORY(lcExternal, "mvp.net.external")

namespace {

QString resolveCommand(const QString &command)
{
    const QFileInfo info(command);
    if (info.isAbsolute())
        return command;
    // 相对路径按程序目录解析，避免受"当前工作目录"影响
    const QString candidate =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(command);
    if (QFileInfo::exists(candidate))
        return candidate;
    return command; // 交给系统按 PATH 查找（例如 python / node）
}

/// 外部解析器可能直接吐站点里的相对地址；能补成绝对 URL 的就补，
/// 补不出来的返回空，避免“非空但不可播”的条目混进列表。
QUrl normalizeExternalUrl(const QString &raw, const QUrl &base = QUrl())
{
    QString text = raw.trimmed();
    if (text.isEmpty())
        return QUrl();
    if (text.startsWith(QStringLiteral("//")))
        text.prepend(QStringLiteral("https:"));

    QUrl url(text);
    const bool httpBase = base.isValid() && !base.isRelative()
                          && (base.scheme() == QLatin1String("http")
                              || base.scheme() == QLatin1String("https"));
    if (url.isRelative() && httpBase)
        url = base.resolved(url);
    return url;
}

} // namespace

ExternalSearchProvider::ExternalSearchProvider(const SourceConfig &config, QObject *parent)
    : SearchProviderBase(parent)
    , m_config(config)
{
}

void ExternalSearchProvider::search(const QString &keyword, int limit)
{
    m_done = false;
    m_filtered = 0;

    if (m_config.command.trimmed().isEmpty()) {
        finish(tr("没有配置解析器命令"));
        return;
    }

    QStringList arguments = QProcess::splitCommand(m_config.arguments);
    for (QString &token : arguments) {
        token.replace(QStringLiteral("{keyword}"), keyword);
        token.replace(QStringLiteral("{limit}"), QString::number(limit));
    }

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                onProcessFinished(exitCode, status);
            });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    onFailedToStart();
            });

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ExternalSearchProvider::onTimeout);
    m_timer->start(qMax(1000, m_config.timeoutMs));

    qCInfo(lcExternal) << "启动外部解析器" << m_config.command << arguments;
    m_process->start(resolveCommand(m_config.command), arguments);
}

void ExternalSearchProvider::onFailedToStart()
{
    finish(tr("解析器启动失败：%1（检查命令路径或运行时是否安装）")
               .arg(m_process ? m_process->errorString() : QString()));
}

void ExternalSearchProvider::onTimeout()
{
    if (m_done)
        return;
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
        m_process->kill();
    finish(tr("解析器超时（%1 ms）").arg(m_config.timeoutMs));
}

void ExternalSearchProvider::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_done)
        return;
    if (m_timer != nullptr)
        m_timer->stop();

    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString stderrText =
            QString::fromUtf8(m_process->readAllStandardError()).trimmed().left(240);
        finish(tr("解析器退出码 %1%2")
                   .arg(exitCode)
                   .arg(stderrText.isEmpty() ? QString() : QStringLiteral("：") + stderrText));
        return;
    }

    parseOutput(m_process->readAllStandardOutput());
}

void ExternalSearchProvider::parseOutput(const QByteArray &output)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcExternal) << "解析器输出不是 JSON：" << output.left(160);
        finish(tr("解析器输出不是 JSON"));
        return;
    }

    QJsonArray items;
    if (document.isArray()) {
        items = document.array();
    } else if (document.isObject()) {
        const QJsonObject object = document.object();
        if (object.contains(QStringLiteral("error")) && !object.contains(QStringLiteral("items"))) {
            finish(object.value(QStringLiteral("error")).toString());
            return;
        }
        items = object.value(QStringLiteral("items")).toArray();
        m_filtered = object.value(QStringLiteral("filtered")).toInt(0);
    } else {
        finish(tr("解析器输出了空结果"));
        return;
    }

    QVector<MediaItem> result;
    for (const QJsonValue &value : items) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();

        const QString stream = object.value(QStringLiteral("streamUrl")).toString().isEmpty()
                                   ? object.value(QStringLiteral("url")).toString()
                                   : object.value(QStringLiteral("streamUrl")).toString();
        if (stream.isEmpty()) {
            ++m_filtered;
            continue;
        }

        MediaItem item;
        item.type = m_config.mediaType;
        item.title = object.value(QStringLiteral("title")).toString();
        item.artist = object.value(QStringLiteral("artist")).toString();
        // 先归一 webUrl 作为相对地址基准，再处理播放页和歌词地址
        item.webUrl = normalizeExternalUrl(object.value(QStringLiteral("webUrl")).toString());
        item.url = normalizeExternalUrl(stream, item.webUrl);
        item.lyricUrl =
            normalizeExternalUrl(object.value(QStringLiteral("lyricUrl")).toString(),
                                 item.webUrl);
        item.durationMs = static_cast<qint64>(object.value(QStringLiteral("durationMs")).toDouble(0));
        item.sourceId = m_config.id;
        item.online = true;
        item.headers = m_config.headers;
        if (!m_config.cookie.isEmpty())
            item.headers.insert(QStringLiteral("Cookie"), m_config.cookie);

        if (item.title.isEmpty() || item.url.isEmpty() || item.url.isRelative()
            || !item.url.isValid()) {
            ++m_filtered;
            continue;
        }
        result.append(item);
    }

    qCInfo(lcExternal) << m_config.id << "外部解析器返回" << result.size() << "条，过滤"
                       << m_filtered << "条";
    emit finished(m_config.id, result, m_filtered, QString());
    m_done = true;
}

void ExternalSearchProvider::finish(const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    if (m_timer != nullptr)
        m_timer->stop();
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
        m_process->kill();
    emit finished(m_config.id, {}, m_filtered, error);
}
