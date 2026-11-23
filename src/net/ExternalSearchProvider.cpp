/// @file ExternalSearchProvider.cpp
/// @brief 外部命令解析器搜索源的进程管理与输出解析实现。
#include "net/ExternalSearchProvider.h"
#include "net/NetLimits.h"
#include "util/NetworkAddressPolicy.h"

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

/// @brief 解析解析器命令路径。
/// @return 绝对路径原样返回；相对路径优先按程序目录解析，找不到则交给 PATH。
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

/// @brief 归一化外部解析器返回的地址。
/// @param raw 原始地址。
/// @param base 用于解析相对地址的站点地址。
/// @return 可用的绝对/相对 URL；空输入返回空。
/// @note 外部解析器可能直接吐站点相对地址；补不出绝对 URL 的条目
///       后续会被过滤，避免“非空但不可播”混进列表。
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

/// @brief 构造外部解析器搜索源。
/// @param config 源配置，内部按值保存。
/// @param parent 父对象。
ExternalSearchProvider::ExternalSearchProvider(const SourceConfig &config, QObject *parent)
    : SearchProviderBase(parent)
    , m_config(config)
{
}

/// @brief 启动外部命令并按 JSON 输出解析结果。
/// @param keyword 关键词，替换参数中的 {keyword}。
/// @param limit 期望条数，替换 {limit} 并作为硬截断上限。
/// @note 同时限制 stdout/stderr 各 2MB；超时默认 8 秒，可被配置覆盖。
void ExternalSearchProvider::search(const QString &keyword, int limit)
{
    m_done = false;
    m_filtered = 0;
    m_limit = qMax(0, limit);
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
    // stderr 边到边读并计总字节，避免持续输出的错误日志堆在 QProcess
    // 内部缓冲区；超限立即中止。
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

/// @brief 取消当前搜索：停止计时并终止子进程。
void ExternalSearchProvider::abort()
{
    m_done = true;
    if (m_timer != nullptr)
        m_timer->stop();
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
        m_process->kill();
}

/// @brief 子进程启动失败时收口并提示检查命令路径/运行时。
void ExternalSearchProvider::onFailedToStart()
{
    finish(tr("解析器启动失败：%1（检查命令路径或运行时是否安装）")
               .arg(m_process ? m_process->errorString() : QString()));
}

/// @brief 超时后终止子进程并按失败收口，避免搜索永久挂起。
void ExternalSearchProvider::onTimeout()
{
    if (m_done)
        return;
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
        m_process->kill();
    const int timeoutMs = m_config.timeoutMs > 0 ? m_config.timeoutMs : 8000;
    finish(tr("解析器超时（%1 ms）").arg(timeoutMs));
}

/// @brief 子进程正常退出后读取 stdout 并解析。
/// @note 非 0 退出码或异常退出只附带 stderr 末尾 240 字符，避免日志过长。
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

/// @brief 解析外部解析器的 JSON 输出并构建条目。
/// @param output stdout 原始字节。
/// @note 支持顶层数组或 {items:[],filtered:N,error:"..."}；按 limit 硬截断；
///       凭据作用域只认配置源站，解析器输出的 webUrl 仅用于补全相对地址。
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
        // 解析器可能忽略命令行里的 {limit}，一次吐出整个站点结果；
        // 这里按调用方页大小硬截断，避免 2 MB 输出变成数万条占用界面/内存。
        if (m_limit > 0 && result.size() >= m_limit)
            break;
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
        // Cookie/Authorization 的作用域只能来自用户配置的源站，不能让解析器输出的
        // 远程 webUrl 决定凭据发往哪个 host；webUrl 只用于补全相对地址和展示。
        const QUrl trustedOrigin(m_config.urlTemplate);
        item.sourceOrigin = HeaderPolicy::originOnly(trustedOrigin);
        item.online = true;
        item.headers = m_config.headers;
        if (!m_config.cookie.isEmpty())
            item.headers.insert(QStringLiteral("Cookie"), m_config.cookie);
        item.headers = HeaderPolicy::withSourceHeaders(item.headers, trustedOrigin);

        if (item.title.isEmpty() || item.url.isEmpty() || item.url.isRelative()
            || !item.url.isValid()
            || !NetworkAddressPolicy::allowsMediaTarget(item.url, item.sourceOrigin)) {
            ++m_filtered;
            continue;
        }
        result.append(item);
    }

    qCInfo(lcExternal) << m_config.id << "外部解析器返回" << result.size() << "条，过滤"
                       << m_filtered << "条";
    // 先置完成位再发信号：finished 的同步槽可能立刻发起第二次搜索，
    // 旧顺序会看到 m_done=false，造成重入或旧输出覆盖新搜索。
    m_done = true;
    emit finished(m_config.id, result, m_filtered, QString());
}

/// @brief 以失败收口并停止计时/子进程；已结束时直接返回。
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
