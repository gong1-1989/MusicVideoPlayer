#include "cache/HlsPreloadIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>

namespace {

QByteArray entryPayload(const QString &identity, const HlsPreloadEntry &entry)
{
    QByteArray range = entry.range.trimmed();
    if (range.compare("bytes=0-", Qt::CaseInsensitive) == 0)
        range.clear();
    QJsonObject object;
    object.insert(QStringLiteral("i"), identity);
    object.insert(QStringLiteral("u"), entry.url.toString(QUrl::FullyEncoded));
    object.insert(QStringLiteral("r"), QString::fromLatin1(range.toBase64()));
    object.insert(QStringLiteral("p"), entry.filePath);
    object.insert(QStringLiteral("o"), QString::number(entry.offset));
    object.insert(QStringLiteral("s"), QString::number(entry.size));
    object.insert(QStringLiteral("t"), QString::fromLatin1(entry.contentType));
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace

HlsPreloadIndex &HlsPreloadIndex::instance()
{
    static HlsPreloadIndex index;
    return index;
}

QByteArray HlsPreloadIndex::normalizeRange(const QByteArray &range)
{
    const QByteArray trimmed = range.trimmed();
    // 普通 HLS 分片播放器通常带 `bytes=0-`；后台缓存记录的是整段无 Range。
    // 两者必须命中同一条索引，否则缓存好的分片仍会被当成未命中。
    if (trimmed.compare("bytes=0-", Qt::CaseInsensitive) == 0)
        return {};
    return trimmed;
}

QString HlsPreloadIndex::keyFor(const QString &identity, const QUrl &url, const QByteArray &range)
{
    return identity + QLatin1Char('|') + url.toString(QUrl::FullyEncoded) + QLatin1Char('|')
           + QString::fromLatin1(normalizeRange(range).toBase64());
}

bool HlsPreloadIndex::fileHasSlice(const QString &path, qint64 offset, qint64 size)
{
    if (path.isEmpty() || offset < 0 || size <= 0)
        return false;
    const QFileInfo info(path);
    return info.exists() && info.size() >= offset + size;
}

QString HlsPreloadIndex::alternatePath(const QString &path)
{
    if (path.endsWith(QLatin1String(".mvpdownload"), Qt::CaseInsensitive))
        return path.left(path.size() - int(QStringLiteral(".mvpdownload").size()));
    return path + QStringLiteral(".mvpdownload");
}

void HlsPreloadIndex::openJournal(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    m_slices.clear();
    m_journalPath = path;
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject object = document.object();
        const QString identity = object.value(QStringLiteral("i")).toString();
        const QString url = object.value(QStringLiteral("u")).toString();
        const QString filePath = object.value(QStringLiteral("p")).toString();
        const qint64 offset = object.value(QStringLiteral("o")).toVariant().toLongLong();
        const qint64 size = object.value(QStringLiteral("s")).toVariant().toLongLong();
        if (identity.isEmpty() || url.isEmpty() || filePath.isEmpty() || offset < 0 || size <= 0)
            continue;
        HlsPreloadSlice slice;
        slice.filePath = filePath;
        slice.offset = offset;
        slice.size = size;
        slice.contentType = object.value(QStringLiteral("t")).toString().toLatin1();
        const QUrl parsed(url);
        const QByteArray range =
            QByteArray::fromBase64(object.value(QStringLiteral("r")).toString().toLatin1());
        if (parsed.isValid())
            m_slices.insert(keyFor(identity, parsed, range), slice);
    }
}

void HlsPreloadIndex::record(const QString &identity, const HlsPreloadEntry &entry)
{
    recordMany(identity, QList<HlsPreloadEntry>{entry});
}

void HlsPreloadIndex::recordMany(const QString &identity, const QList<HlsPreloadEntry> &entries)
{
    if (identity.isEmpty() || entries.isEmpty())
        return;

    QMutexLocker lock(&m_mutex);
    QByteArray payload;
    for (const HlsPreloadEntry &entry : entries) {
        if (!entry.url.isValid() || entry.filePath.isEmpty() || entry.offset < 0 || entry.size <= 0)
            continue;
        HlsPreloadSlice slice;
        slice.filePath = entry.filePath;
        slice.offset = entry.offset;
        slice.size = entry.size;
        slice.contentType = entry.contentType;
        m_slices.insert(keyFor(identity, entry.url, entry.range), slice);
        payload += entryPayload(identity, entry);
    }
    if (payload.isEmpty() || m_journalPath.isEmpty())
        return;

    QDir().mkpath(QFileInfo(m_journalPath).absolutePath());
    QFile file(m_journalPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    file.write(payload);
}

bool HlsPreloadIndex::find(const QString &identity, const QUrl &url, const QByteArray &range,
                           HlsPreloadSlice *slice) const
{
    if (identity.isEmpty() || slice == nullptr || !url.isValid())
        return false;
    QMutexLocker lock(&m_mutex);
    auto it = m_slices.constFind(keyFor(identity, url, range));
    // 播放器可能把整段请求写成 `bytes=0-<长度-1>`，而索引登记的是整片。
    // 这种从 0 开始、终点正好覆盖整片长度的 Range 也视为命中。
    if (it == m_slices.constEnd()) {
        const QByteArray normalized = normalizeRange(range);
        if (normalized.startsWith(QByteArrayLiteral("bytes=0-"))) {
            const QByteArray tail = normalized.mid(8);
            if (!tail.isEmpty()) {
                const qint64 end = tail.toLongLong();
                auto full = m_slices.constFind(keyFor(identity, url, QByteArray()));
                if (full != m_slices.constEnd() && end >= 0
                    && end + 1 == full.value().size) {
                    it = full;
                }
            }
        }
    }
    if (it == m_slices.constEnd())
        return false;

    const HlsPreloadSlice stored = it.value();
    if (fileHasSlice(stored.filePath, stored.offset, stored.size)) {
        *slice = stored;
        return true;
    }
    // 下载任务先写 `.mvpdownload`，完成时才改名成 `.video/.audio`。索引可能
    // 在任一时点建立，读取时两种名字都试一次；文件不存在或长度不够就失败。
    const QString alternate = alternatePath(stored.filePath);
    if (fileHasSlice(alternate, stored.offset, stored.size)) {
        *slice = stored;
        slice->filePath = alternate;
        return true;
    }
    return false;
}

int HlsPreloadIndex::count(const QString &identity) const
{
    if (identity.isEmpty())
        return 0;
    QMutexLocker lock(&m_mutex);
    int total = 0;
    const QString prefix = identity + QLatin1Char('|');
    for (auto it = m_slices.constBegin(); it != m_slices.constEnd(); ++it) {
        if (it.key().startsWith(prefix))
            ++total;
    }
    return total;
}

void HlsPreloadIndex::clear()
{
    QMutexLocker lock(&m_mutex);
    m_slices.clear();
}
