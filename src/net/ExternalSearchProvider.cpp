#include "net/ExternalSearchProvider.h"
#include "net/NetLimits.h"

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
    m_stderrBytes = 0;
    m_stderrTail.clear();

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
    // 解析器输出直接进内存解析；这里在数据到达时就设上限，
    // 避免子进程持续输出把缓冲区顶大。
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        if (m_done || m_process == nullptr)
            return;
        if (m_process->bytesAvailable() > NetLimits::kJsonBytes)
            finish(tr("解析器输出超过 2 MB 上限"));
    });
    // stderr 过去只在进程结束时读，持续输出的错误日志会一直堆在
    // QProcess 内部缓冲区。这里边到边丢并计总字节，超限就中止。
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        if (m_done || m_process == nullptr)
            return;
        const QByteArray chunk = m_process->readAllStandardError();
        m_stderrBytes += chunk.size();
        if (!chunk.isEmpty()) {
            // 只保留末尾 240 字符给失败提示；前面的内容随时丢弃，
            // 避免 stderr 在 QProcess 缓冲区里无限增长。
            m_stderrTail += QString::fromUtf8(chunk);
            if (m_stderrTail.size() > 240)
                m_stderrTail = m_stderrTail.right(240);
        }
        if (m_stderrBytes > NetLimits::kJsonBytes)
            finish(tr("解析器错误输出超过 2 MB 上限"));
    });

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ExternalSearchProvider::onTimeout);
    const int timeoutMs = m_config.timeoutMs > 0 ? m_config.timeoutMs : 8000;
    m_timer->start(qMax(1000, timeoutMs));

    qCInfo(lcExternal) << "启动外部解析器" << m_config.command << arguments;
    m_process->start(resolveCommand(m_config.command), arguments);
}

void ExternalSearchProvider::abort()
{
    m_done = true;
    if (m_timer != nullptr)
        m_timer->stop();
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
        m_process->kill();
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
    const int timeoutMs = m_config.timeoutMs > 0 ? m_config.timeoutMs : 8000;
    finish(tr("解析器超时（%1 ms）").arg(timeoutMs));
}

void ExternalSearchProvider::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_done)
        return;
    if (m_timer != nullptr)
        m_timer->stop();

    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString stderrText = m_stderrTail.trimmed().left(240);
        finish(tr("解析器退出码 %1%2")
                   .arg(exitCode)
                   .arg(stderrText.isEmpty() ? QString() : QStringLiteral("：") + stderrText));
        return;
    }

    const QByteArray output = m_process->readAllStandardOutput();
    if (output.size() > NetLimits::kJsonBytes) {
        finish(tr("解析器输出超过 2 MB 上限"));
        return;
    }
    parseOutput(output);
}

void ExternalSearchProvider::parseOutput(const QByteArray &output)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        // 解析器输出可能包含带签名的 streamUrl，失败日志只记录大小和错误位置。
        qCWarning(lcExternal) << "解析器输出不是 JSON：输出" << output.size()
                              << "字节，错误" << parseError.errorString()
                              << "位置" << parseError.offset;
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
        item.sourceOrigin = item.webUrl.isValid() && !item.webUrl.host().isEmpty()
                                ? item.webUrl
                                : QUrl(m_config.urlTemplate);
        item.online = true;
        item.headers = m_config.headers;
        if (!m_config.cookie.isEmpty())
            item.headers.insert(QStringLiteral("Cookie"), m_config.cookie);
        item.headers = HeaderPolicy::withSourceHeaders(item.headers, item.sourceOrigin);

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
