/// @brief StreamProxy 的流转缓存文件读写实现。
/// @note 内存/磁盘两种缓存正文的切片、追加、打开与元数据落盘集中在这里；
///       请求路由和预取状态机仍留在 StreamProxy.cpp。
#include "stream/StreamProxy.h"

#include "cache/MediaCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcProxyCacheIO, "mvp.stream")
qint64 StreamProxy::cacheDataSize(const CacheEntry &entry) const
{
    return entry.diskBacked ? entry.dataSize : entry.data.size();
}

qint64 StreamProxy::cacheSlice(CacheEntry &entry, qint64 offset, qint64 length,
                               QByteArray *out) const
{
    if (out == nullptr || offset < 0 || length <= 0)
        return 0;
    if (!entry.diskBacked) {
        if (offset >= entry.data.size())
            return 0;
        *out = entry.data.mid(static_cast<int>(offset), static_cast<int>(length));
        return out->size();
    }

    if (entry.file == nullptr || !entry.file->isOpen())
        return 0;
    if (!entry.file->seek(offset)) {
        qCWarning(lcProxyCacheIO) << "流转缓存文件定位失败" << entry.filePath << offset;
        return 0;
    }
    out->resize(static_cast<int>(length));
    const qint64 read = entry.file->read(out->data(), length);
    if (read <= 0) {
        out->clear();
        return 0;
    }
    out->resize(static_cast<int>(read));
    return read;
}

bool StreamProxy::appendCacheData(CacheEntry &entry, const QByteArray &chunk)
{
    if (chunk.isEmpty())
        return true;
    if (!entry.diskBacked) {
        entry.data.append(chunk);
        entry.dataSize = entry.data.size();
        return true;
    }

    if (entry.file == nullptr || !entry.file->isOpen())
        return false;
    if (!entry.file->seek(entry.dataSize)) {
        qCWarning(lcProxyCacheIO) << "流转缓存文件追加定位失败" << entry.filePath;
        return false;
    }
    const qint64 written = entry.file->write(chunk);
    if (written != chunk.size()) {
        qCWarning(lcProxyCacheIO) << "流转缓存文件写入失败" << entry.filePath << written
                           << "/" << chunk.size();
        return false;
    }
    // 断点续传依赖“文件里已经有这些字节”，每块写完就刷盘；256KB 一块，
    // 对本地 SSD/机械盘都不会形成明显 I/O 压力。
    entry.file->flush();
    entry.dataSize += written;
    return true;
}

qint64 StreamProxy::openCacheFile(CacheEntry &entry, const QString &path,
                                  qint64 existingBytes)
{
    if (path.isEmpty())
        return -1;
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return -1;

    std::unique_ptr<QFile> file(new QFile(path));
    if (!file->open(QIODevice::ReadWrite)) {
        qCWarning(lcProxyCacheIO) << "流转缓存文件无法打开" << path << file->errorString();
        return -1;
    }

    qint64 actual = file->size();
    if (existingBytes <= 0) {
        if (!file->resize(0)) {
            qCWarning(lcProxyCacheIO) << "流转缓存文件截断失败" << path;
            return -1;
        }
        actual = 0;
    } else if (actual > existingBytes) {
        if (!file->resize(existingBytes)) {
            qCWarning(lcProxyCacheIO) << "流转缓存文件尾部截断失败" << path;
            return -1;
        }
        actual = existingBytes;
    } else if (actual < existingBytes) {
        // 元数据记录的长度比真实文件大（上次崩溃在写入与刷盘之间），
        // 以真实文件长度为准，避免在空洞后面追加。
        existingBytes = actual;
    }

    entry.file = file.release();
    entry.filePath = path;
    entry.diskBacked = true;
    entry.dataSize = actual;
    return actual;
}

void StreamProxy::closeCacheFile(CacheEntry &entry)
{
    if (entry.file == nullptr)
        return;
    persistCacheState(entry);
    entry.file->flush();
    entry.file->close();
    delete entry.file;
    entry.file = nullptr;
}

void StreamProxy::persistCacheState(CacheEntry &entry)
{
    if (!entry.diskBacked || entry.filePath.isEmpty())
        return;
    MediaCache::writeState(entry.filePath, entry.cacheIdentity, entry.upstream,
                           entry.contentType, entry.total, entry.complete);
}

