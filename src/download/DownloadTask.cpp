#include "download/DownloadTask.h"

#include "net/HttpClient.h"

#include <QLoggingCategory>
#include <QNetworkReply>
#include <QRegularExpression>

Q_LOGGING_CATEGORY(lcDownload, "mvp.download")

DownloadTask::DownloadTask(HttpClient *http,
                           const QUrl &url,
                           const QString &filePath,
                           QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_url(url)
    , m_filePath(filePath)
{
}

DownloadTask *DownloadTask::start(HttpClient *http,
                                  const QUrl &url,
                                  const QString &filePath,
                                  QObject *parent)
{
    auto *task = new DownloadTask(http, url, filePath, parent);
    task->startRequest();
    return task;
}

QString DownloadTask::sanitizeFileName(const QString &name, const QString &suffix)
{
    QString base = name;
    base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\x00-\x1F])")),
                 QStringLiteral("_"));
    base = base.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("download");
    if (base.size() > 120)
        base.truncate(120);

    const QString tail = QLatin1Char('.') + suffix;
    if (!base.endsWith(tail, Qt::CaseInsensitive))
        base += tail;
    return base;
}

void DownloadTask::startRequest()
{
    if (m_http == nullptr || !m_url.isValid()) {
        emit failed(tr("下载地址无效"));
        finish();
        return;
    }

    m_file = new QSaveFile(m_filePath, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        emit failed(tr("无法写入文件：%1").arg(m_file->errorString()));
        finish();
        return;
    }

    m_reply = m_http->get(m_url);
    connect(m_reply, &QNetworkReply::readyRead, this, &DownloadTask::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &DownloadTask::onFinished);
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) { emit progress(received, total); });
}

void DownloadTask::onReadyRead()
{
    if (m_reply.isNull() || m_file.isNull())
        return;
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty())
        return;
    if (!m_file->write(chunk)) {
        qCWarning(lcDownload) << "write failed:" << m_file->errorString();
        cancel();
    }
}

void DownloadTask::onFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    if (m_done || reply != m_reply)
        return;
    m_reply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        const QString message = HttpClient::describeError(reply);
        qCWarning(lcDownload) << "download failed:" << message;
        if (!m_file.isNull()) {
            m_file->cancelWriting();
            m_file->deleteLater();
            m_file = nullptr;
        }
        emit failed(message);
        finish();
        return;
    }

    // 冲掉缓冲区里最后一段数据
    if (!m_file.isNull()) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty())
            m_file->write(tail);

        if (!m_file->commit()) {
            const QString message = m_file->errorString();
            m_file->deleteLater();
            m_file = nullptr;
            emit failed(message);
            finish();
            return;
        }
        m_file->deleteLater();
        m_file = nullptr;
    }

    qCInfo(lcDownload) << "saved" << m_filePath;
    emit succeeded(m_filePath);
    finish();
}

void DownloadTask::cancel()
{
    if (m_done)
        return;

    if (!m_reply.isNull()) {
        QNetworkReply *reply = m_reply.data();
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (!m_file.isNull()) {
        m_file->cancelWriting();
        m_file->deleteLater();
        m_file = nullptr;
    }

    emit failed(tr("下载已取消"));
    finish();
}

void DownloadTask::finish()
{
    if (m_done)
        return;
    m_done = true;
    emit finished();
    deleteLater();
}
