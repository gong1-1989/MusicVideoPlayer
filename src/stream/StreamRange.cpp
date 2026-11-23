/// @file StreamRange.cpp
/// @brief HTTP Range 解析、分片切分与缓存片段合并。
#include "stream/StreamRange.h"

#include <QRegularExpression>

#include <algorithm>

/// @brief 解析响应头 Content-Range（格式 bytes a-b/total）。
/// @param value 响应头原文。
/// @param start 输出起始偏移。
/// @param end 输出结束偏移（含）。
/// @param total 输出资源总长度。
/// @return 格式合法且区间不越界返回 true。
bool StreamRange::parseContentRange(const QByteArray &value, qint64 *start, qint64 *end,
                                    qint64 *total)
{
    if (start == nullptr || end == nullptr || total == nullptr)
        return false;
    static const QRegularExpression pattern(
        QStringLiteral("^bytes\\s+(\\d+)-(\\d+)/(\\d+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(value).trimmed());
    if (!match.hasMatch())
        return false;
    bool okStart = false;
    bool okEnd = false;
    bool okTotal = false;
    const qint64 first = match.captured(1).toLongLong(&okStart);
    const qint64 last = match.captured(2).toLongLong(&okEnd);
    const qint64 size = match.captured(3).toLongLong(&okTotal);
    if (!okStart || !okEnd || !okTotal || size <= 0 || first < 0 || last < first
        || last >= size) {
        return false;
    }
    *start = first;
    *end = last;
    *total = size;
    return true;
}

/// @brief 解析请求头 Range，支持 bytes=a-b、bytes=a-、bytes=-N 三种单段形式。
/// @param value 请求头原文。
/// @param total 资源总长度，必须大于 0。
/// @param start 输出起始偏移。
/// @param end 输出结束偏移（含），超出总长会夹到 total-1。
/// @return 可解析为单段区间返回 true；多段 Range 返回 false，由调用方退化为 200 全量。
bool StreamRange::parseRange(const QString &value, qint64 total, qint64 *start, qint64 *end)
{
    if (start == nullptr || end == nullptr || total <= 0)
        return false;

    const QString trimmed = value.trimmed();
    if (!trimmed.startsWith(QLatin1String("bytes="), Qt::CaseInsensitive))
        return false;

    const QString spec = trimmed.mid(6).trimmed();
    // 多段 Range（bytes=0-1,5-6）无法用单个切片表达，返回 false 让
    // serveFromCache 退化成 200 全量。
    if (spec.contains(QLatin1Char(',')))
        return false;

    const int dash = spec.indexOf(QLatin1Char('-'));
    if (dash < 0)
        return false;

    const QString left = spec.left(dash).trimmed();
    const QString right = spec.mid(dash + 1).trimmed();

    qint64 first = 0;
    qint64 last = 0;
    if (left.isEmpty()) {
        // bytes=-N：最后 N 个字节
        if (right.isEmpty())
            return false;
        bool ok = false;
        const qint64 suffix = right.toLongLong(&ok);
        if (!ok || suffix <= 0)
            return false;
        first = qMax<qint64>(0, total - suffix);
        last = total - 1;
    } else {
        bool ok = false;
        first = left.toLongLong(&ok);
        if (!ok || first < 0)
            return false;
        if (right.isEmpty()) {
            last = total - 1;
        } else {
            last = right.toLongLong(&ok);
            if (!ok || last < first)
                return false;
            last = qMin(last, total - 1);
        }
    }

    *start = first;
    *end = last;
    return true;
}

/// @brief 从 nextStart 起把剩余字节切成最多 connections 段，每段不小于 minPartBytes。
/// @return 分段列表；起点越界或总长非正时返回空。
QList<StreamRange::Part> StreamRange::split(qint64 total, qint64 nextStart, int connections,
                                            qint64 minPartBytes)
{
    QList<Part> parts;
    if (total <= 0 || nextStart < 0 || nextStart >= total)
        return parts;

    const int count = std::max(1, connections);
    const qint64 partSize =
        std::max(minPartBytes, (total + count - 1) / count);
    for (qint64 next = nextStart; next < total;) {
        const qint64 end = qMin(total - 1, next + partSize - 1);
        parts.append({next, end});
        next = end + 1;
    }
    return parts;
}

/// @brief 把一段响应正文追加到缓存条目的对应区间。
/// @param entry 缓存条目。
/// @param start 区间起始偏移。
/// @param end 区间结束偏移（含）。
/// @param chunk 本次收到的正文，超出区间长度会被截断。
/// @note 已写满的区间直接跳过，支持断点续传时重复到达的数据。
void StreamRange::append(StreamCacheEntry &entry, qint64 start, qint64 end,
                         const QByteArray &chunk)
{
    if (start > end || chunk.isEmpty())
        return;

    const qint64 expected = end - start + 1;
    QByteArray &buffer = entry.rangeParts[start];
    if (buffer.size() >= expected)
        return;

    const qint64 remain = expected - buffer.size();
    if (chunk.size() > remain)
        buffer.append(chunk.left(static_cast<int>(remain)));
    else
        buffer.append(chunk);
    entry.rangePartEnds.insert(start, end);
}

/// @brief 从当前连续末尾开始，把紧随其后的完整区间并入 entry.data。
/// @param entry 缓存条目。
/// @return 合并后是否已完整（data 长度达到 total）。
/// @note 只合并从 data.size() 开始的连续片段；中间缺失时空洞保持，等待后续补传。
bool StreamRange::mergeConsecutive(StreamCacheEntry &entry)
{
    while (true) {
        const qint64 start = entry.data.size();
        const auto part = entry.rangeParts.find(start);
        if (part == entry.rangeParts.end())
            break;
        const qint64 end = entry.rangePartEnds.value(start, -1);
        if (end < start || part.value().size() < end - start + 1)
            break;
        entry.data.append(part.value());
        entry.rangeParts.erase(part);
        entry.rangePartEnds.remove(start);
    }

    if (entry.total >= 0 && entry.data.size() >= entry.total)
        entry.complete = true;
    return entry.complete;
}
