#ifndef LRCPARSER_H
#define LRCPARSER_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <qglobal.h>

struct LrcLine
{
    qint64 timeMs = 0;
    QString text;
};

/// LRC 解析器：无状态、纯函数，便于单元测试。
class LrcParser
{
public:
    /// 解析已解码的 LRC 文本。支持 [mm:ss]、[mm:ss.x]、[mm:ss.xx]、
    /// [mm:ss.xxx]、一行多时间戳、[offset:] 全局偏移，并过滤 [ti:][ar:] 等元信息。
    static QVector<LrcLine> parse(const QString &content);

    /// 解析原始字节：自动识别 UTF-8 BOM，无效 UTF-8 时回退系统编码（GBK 等）。
    static QVector<LrcLine> parseBytes(const QByteArray &data);

    /// 返回 posMs 时刻应高亮的行下标，早于第一行时返回 -1。
    static int indexAt(const QVector<LrcLine> &lines, qint64 posMs);

private:
    static QString decode(const QByteArray &data);
};

#endif // LRCPARSER_H
