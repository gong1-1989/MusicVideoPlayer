#include "download/DownloadJob.h"

#include "util/HeaderPolicy.h"
#include "util/SecretStore.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QFileInfo>

namespace {

QString statusKey(DownloadJob::Status status)
{
    switch (status) {
    case DownloadJob::Status::Downloading:
        return QStringLiteral("downloading");
    case DownloadJob::Status::Paused:
        return QStringLiteral("paused");
    case DownloadJob::Status::Completed:
        return QStringLiteral("completed");
    case DownloadJob::Status::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("paused");
}

DownloadJob::Status statusFromKey(const QString &key)
{
    if (key == QLatin1String("downloading"))
        return DownloadJob::Status::Downloading;
    if (key == QLatin1String("completed"))
        return DownloadJob::Status::Completed;
    if (key == QLatin1String("failed"))
        return DownloadJob::Status::Failed;
    return DownloadJob::Status::Paused;
}

} // namespace

int DownloadJob::percent() const
{
    if (status == Status::Completed)
        return 100;
    if (hls && segmentCount > 0)
        return qBound(0, int(qint64(segmentIndex) * 100 / segmentCount), 100);
    if (bytesTotal > 0)
        return qBound(0, int(bytesReceived * 100 / bytesTotal), 100);
    return 0;
}

QString DownloadJob::statusText() const
{
    switch (status) {
    case Status::Downloading:
        return message.isEmpty() ? QStringLiteral("正在下载") : message;
    case Status::Paused:
        return message.isEmpty() ? QStringLiteral("已暂停，等待续传") : message;
    case Status::Completed:
        return message.isEmpty() ? QStringLiteral("下载完成") : message;
    case Status::Failed:
        return message.isEmpty() ? QStringLiteral("下载失败") : message;
    }
    return message;
}

QString DownloadJob::partialPath() const
{
    return filePath + QStringLiteral(".mvpdownload");
}

QJsonObject DownloadJob::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    QJsonObject itemObject = mediaItemToJson(item);
    // item.url 和 job.url 是同一个直链；只保留 job.url 的加密形式，
    // 避免 DPAPI 只保护一份、另一份仍以明文留在 JSON 里。
    itemObject.remove(QStringLiteral("url"));
    object.insert(QStringLiteral("item"), itemObject);
    object.insert(QStringLiteral("url"), url.toString());
    // 直链 query 常带签名/API key，不能像标题一样明文落盘；能加密就只存密文。
    const QString protectedUrl =
        SecretStore::protectForStorage(url.toString(), QStringLiteral("download/%1/url").arg(id));
    if (!protectedUrl.isEmpty()) {
        object.insert(QStringLiteral("urlProtected"), protectedUrl);
        object.remove(QStringLiteral("url"));
    }
    object.insert(QStringLiteral("filePath"), filePath);
    object.insert(QStringLiteral("bytesReceived"), double(bytesReceived));
    object.insert(QStringLiteral("bytesTotal"), double(bytesTotal));
    object.insert(QStringLiteral("maxBytes"), double(maxBytes));
    object.insert(QStringLiteral("segmentIndex"), segmentIndex);
    object.insert(QStringLiteral("segmentCount"), segmentCount);
    object.insert(QStringLiteral("hls"), hls);
    object.insert(QStringLiteral("committed"), committed);
    object.insert(QStringLiteral("status"), statusKey(status));
    object.insert(QStringLiteral("message"), message);
    object.insert(QStringLiteral("createdAt"), createdAt.toString(Qt::ISODate));
    object.insert(QStringLiteral("completedAt"), completedAt.toString(Qt::ISODate));

    if (!headers.isEmpty()) {
        QJsonObject headersObject;
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            if (HeaderPolicy::isSensitiveHeader(it.key())) {
                // Cookie/Authorization 不能明文落盘；用当前用户的 DPAPI 加密。
                const QString protectedValue =
                    SecretStore::protectForStorage(it.value(), QStringLiteral("download/%1").arg(id));
                if (!protectedValue.isEmpty())
                    headersObject.insert(it.key(), protectedValue);
            } else {
                headersObject.insert(it.key(), it.value());
            }
        }
        object.insert(QStringLiteral("headers"), headersObject);
    }
    return object;
}

bool DownloadJob::fromJson(const QJsonObject &object, DownloadJob *job, QString *error)
{
    if (job == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("任务记录为空");
        return false;
    }

    DownloadJob parsed;
    parsed.id = object.value(QStringLiteral("id")).toString();
    parsed.item = mediaItemFromJson(object.value(QStringLiteral("item")).toObject());
    const QString protectedUrl = object.value(QStringLiteral("urlProtected")).toString();
    const QString plainUrl = protectedUrl.isEmpty()
                                 ? object.value(QStringLiteral("url")).toString()
                                 : SecretStore::revealStored(
                                       protectedUrl,
                                       QStringLiteral("download/%1/url").arg(parsed.id));
    parsed.url = QUrl(plainUrl);
    if (parsed.item.url.isEmpty())
        parsed.item.url = parsed.url;
    parsed.filePath = object.value(QStringLiteral("filePath")).toString();
    parsed.bytesReceived =
        qint64(object.value(QStringLiteral("bytesReceived")).toDouble());
    if (!object.contains(QStringLiteral("bytesReceived")))
        parsed.bytesReceived = qint64(object.value(QStringLiteral("receivedBytes")).toDouble());
    parsed.bytesTotal = qint64(object.value(QStringLiteral("bytesTotal")).toDouble(-1));
    parsed.maxBytes = qint64(object.value(QStringLiteral("maxBytes")).toDouble());
    parsed.segmentIndex = object.value(QStringLiteral("segmentIndex")).toInt();
    parsed.segmentCount = object.value(QStringLiteral("segmentCount")).toInt();
    parsed.hls = object.value(QStringLiteral("hls")).toBool();
    parsed.committed = object.value(QStringLiteral("committed")).toBool(false);
    parsed.status = statusFromKey(object.value(QStringLiteral("status")).toString());
    parsed.message = object.value(QStringLiteral("message")).toString();
    parsed.createdAt =
        QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(),
                              Qt::ISODate);
    parsed.completedAt =
        QDateTime::fromString(object.value(QStringLiteral("completedAt")).toString(),
                              Qt::ISODate);

    const QJsonObject headers = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        QString value = it.value().toString();
        if (HeaderPolicy::isSensitiveHeader(it.key()))
            value = SecretStore::revealStored(value, QStringLiteral("download/%1").arg(parsed.id));
        if (!value.isEmpty())
            parsed.headers.insert(it.key(), value);
    }

    if (parsed.id.isEmpty() || parsed.url.isEmpty() || parsed.filePath.isEmpty()
        || parsed.item.title.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("下载任务缺少必要字段");
        return false;
    }

    *job = parsed;
    return true;
}
