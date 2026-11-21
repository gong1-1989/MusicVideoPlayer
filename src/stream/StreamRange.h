#ifndef STREAMRANGE_H
#define STREAMRANGE_H

#include "stream/StreamCacheStore.h"

#include <QByteArray>
#include <QList>
#include <QString>

/// HTTP Range 解析、分片规划和缓存拼接。
/// 只做纯数据计算，不碰 socket 或 QNetworkReply，便于单元测试。
class StreamRange
{
public:
    struct Part
    {
        qint64 start = 0;
        qint64 end = 0;
    };

    /// 解析 `Content-Range: bytes start-end/total`。
    static bool parseContentRange(const QByteArray &value, qint64 *start, qint64 *end,
                                  qint64 *total);

    /// 解析请求里的单段 Range；多段、非法区间返回 false。
    static bool parseRange(const QString &value, qint64 total, qint64 *start, qint64 *end);

    /// 首片探测完成后，从 nextStart 开始按连接数拆分剩余区间。
    static QList<Part> split(qint64 total, qint64 nextStart, int connections,
                             qint64 minPartBytes);

    /// 把一片已读取的数据放进暂存区，超过期望长度的部分截断。
    static void append(StreamCacheEntry &entry, qint64 start, qint64 end,
                       const QByteArray &chunk);

    /// 把从 data.size() 开始连续且完整的暂存分片并入 data。
    /// 返回是否因为 data 已达到总长而完整。
    static bool mergeConsecutive(StreamCacheEntry &entry);
};

#endif // STREAMRANGE_H
