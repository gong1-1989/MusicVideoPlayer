#include "download/DownloadJob.h"

#include "util/HeaderPolicy.h"
#include "util/SecretStore.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QFileInfo>
#include <QObject>

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
    // altUrls 也是可直接播放的签名地址，下载恢复不使用它们；不写进记录，
    // 否则“只加密主直链”仍会留下另一组明文 URL。
    itemObject.remove(QStringLiteral("altUrls"));
    object.insert(QStringLiteral("item"), itemObject);
    // 直链 query 常带签名/API key，不能像标题一样明文落盘。
    // 只有加密成功才写 urlProtected；加密失败时宁可不写 URL，也不能
    // 先插入明文 url 再“加密成功才删除”，否则 DPAPI 失败会留下明文。
    const QString protectedUrl =
        SecretStore::protectForStorage(url.toString(), QStringLiteral("download/%1/url").arg(id));
    Status storedStatus = status;
    QString storedMessage = message;
    if (!protectedUrl.isEmpty()) {
        object.insert(QStringLiteral("urlProtected"), protectedUrl);
    } else {
        object.insert(QStringLiteral("urlUnavailable"), true);
        if (status != Status::Completed) {
            // 没有可恢复的直链时，不要把任务留在 Paused/Downloading，
            // 否则下次启动会拿空 URL 发起请求。保留记录但明确标为失败。
            storedStatus = Status::Failed;
            storedMessage = QObject::tr("直链凭据保护失败，任务无法在下次启动时续传");
        }
    }
    object.insert(QStringLiteral("filePath"), filePath);
    object.insert(QStringLiteral("cacheCopySourcePath"), cacheCopySourcePath);
    object.insert(QStringLiteral("bytesReceived"), double(bytesReceived));
    object.insert(QStringLiteral("bytesTotal"), double(bytesTotal));
    object.insert(QStringLiteral("maxBytes"), double(maxBytes));
    object.insert(QStringLiteral("segmentIndex"), segmentIndex);
    object.insert(QStringLiteral("segmentCount"), segmentCount);
    object.insert(QStringLiteral("hls"), hls);
    object.insert(QStringLiteral("hlsFingerprint"), hlsFingerprint);
    object.insert(QStringLiteral("snapshot"), snapshot);
    object.insert(QStringLiteral("allowIncomplete"), allowIncomplete);
    object.insert(QStringLiteral("needsSnapshotConsent"), needsSnapshotConsent);
    object.insert(QStringLiteral("committed"), committed);
    object.insert(QStringLiteral("status"), statusKey(storedStatus));
    object.insert(QStringLiteral("message"), storedMessage);
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
    bool urlUnavailable = object.value(QStringLiteral("urlUnavailable")).toBool(false);
    const QString plainUrl = protectedUrl.isEmpty()
                                 ? object.value(QStringLiteral("url")).toString()
                                 : SecretStore::revealStored(
                                       protectedUrl,
                                       QStringLiteral("download/%1/url").arg(parsed.id));
    if (!plainUrl.isEmpty()) {
        parsed.url = QUrl(plainUrl);
    } else if (!protectedUrl.isEmpty()) {
        // 密文存在但当前用户/机器解不开：旧任务无法续传，按“直链不可用”处理。
        urlUnavailable = true;
    }
    if (parsed.item.url.isEmpty())
        parsed.item.url = parsed.url;
    parsed.filePath = object.value(QStringLiteral("filePath")).toString();
    parsed.cacheCopySourcePath =
        object.value(QStringLiteral("cacheCopySourcePath")).toString();
    parsed.bytesReceived =
        qint64(object.value(QStringLiteral("bytesReceived")).toDouble());
    if (!object.contains(QStringLiteral("bytesReceived")))
        parsed.bytesReceived = qint64(object.value(QStringLiteral("receivedBytes")).toDouble());
    parsed.bytesTotal = qint64(object.value(QStringLiteral("bytesTotal")).toDouble(-1));
    parsed.maxBytes = qint64(object.value(QStringLiteral("maxBytes")).toDouble());
    parsed.segmentIndex = object.value(QStringLiteral("segmentIndex")).toInt();
    parsed.segmentCount = object.value(QStringLiteral("segmentCount")).toInt();
    parsed.hls = object.value(QStringLiteral("hls")).toBool();
    parsed.hlsFingerprint = object.value(QStringLiteral("hlsFingerprint")).toString();
    parsed.snapshot = object.value(QStringLiteral("snapshot")).toBool(false);
    parsed.allowIncomplete = object.value(QStringLiteral("allowIncomplete")).toBool(false);
    parsed.needsSnapshotConsent =
        object.value(QStringLiteral("needsSnapshotConsent")).toBool(false);
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

    if (parsed.id.isEmpty() || parsed.filePath.isEmpty() || parsed.item.title.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("下载任务缺少必要字段");
        return false;
    }
    // 历史记录里的 Completed/Failed 不依赖直链，允许无 URL 加载；
    // 只有 Paused/Downloading 需要续传地址。加密失败的续传任务被降级为
    // Failed，保留记录与半成品路径，但不让 DownloadManager 拉起空请求。
    const bool needsUrl =
        parsed.status == DownloadJob::Status::Paused
        || parsed.status == DownloadJob::Status::Downloading;
    if (parsed.url.isEmpty() && needsUrl) {
        if (!urlUnavailable) {
            if (error != nullptr)
                *error = QStringLiteral("下载任务缺少直链地址");
            return false;
        }
        parsed.status = DownloadJob::Status::Failed;
        parsed.message = QObject::tr("直链凭据保护失败，任务无法在下次启动时续传");
    }

    *job = parsed;
    return true;
}
