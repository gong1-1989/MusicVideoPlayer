/// @file LrcParser.cpp
/// @brief LRC 歌词的解码、时间轴解析与当前行定位实现。
#include "lyrics/LrcParser.h"

#include <QRegularExpression>
#include <QtCore/qstringconverter.h>

#include <algorithm>

namespace {

/// @brief UTF-8 BOM 字节序列，用于在解码前剥离。
const QByteArray &utf8Bom()
{
    static const QByteArray bom = QByteArray::fromHex("EFBBBF");
    return bom;
}

/// @brief 匹配 LRC 行首时间戳。
/// @note 支持 [mm:ss]、[mm:ss.xx]、[mm:ss:xx]；分 1-3 位、秒 1-2 位、
///       小数 1-3 位，兼容“厘秒”写法和不补零的分钟。
const QRegularExpression &timestampRegex()
{
    static const QRegularExpression rx(
        QStringLiteral(R"(\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])"));
    return rx;
}

/// @brief 匹配 [offset:±毫秒] 全局时间偏移标签。
const QRegularExpression &offsetRegex()
{
    static const QRegularExpression rx(
        QStringLiteral(R"(\[offset:\s*([+-]?\d+)\s*\])"),
        QRegularExpression::CaseInsensitiveOption);
    return rx;
}

/// @brief 把时间戳的小数部分换算成毫秒。
/// @param fraction 小数位文本，长度 1-3。
/// @return 按位数归一：1 位代表十分之一秒，2 位代表百分之一秒，3 位代表毫秒。
qint64 fractionToMs(const QString &fraction)
{
    if (fraction.isEmpty())
        return 0;
    const int value = fraction.toInt();
    switch (fraction.size()) {
    case 1:
        return value * 100;
    case 2:
        return value * 10;
    default:
        return value;
    }
}

} // namespace

/// @brief 解码 LRC 原始字节。
/// @param data 文件内容。
/// @return 优先按 UTF-8（剥离 BOM）；解码失败时回退系统编码，兼容 GBK/GB18030。
QString LrcParser::decode(const QByteArray &data)
{
    if (data.isEmpty())
        return QString();

    if (data.startsWith(utf8Bom()))
        return QString::fromUtf8(data.mid(utf8Bom().size()));

    QStringDecoder utf8(QStringConverter::Utf8);
    QString text = utf8.decode(data);
    if (utf8.hasError()) {
        // 本地 LRC 大量是 GBK/GB18030，按系统编码兜底
        QStringDecoder fallback(QStringConverter::System);
        text = fallback.decode(data);
    }
    return text;
}

/// @brief 解析已解码的 LRC 文本。
/// @param content 完整歌词文本。
/// @return 按时间升序排列的歌词行；无有效时间戳时返回空。
/// @note 一行可带多个时间戳（副歌复用），每个时间戳生成一条独立记录；
///       [offset:] 会加到所有时间上，最后统一 stable_sort 保持同时间行原顺序。
QVector<LrcLine> LrcParser::parse(const QString &content)
{
    QVector<LrcLine> lines;
    if (content.isEmpty())
        return lines;

    qint64 offsetMs = 0;
    const QRegularExpressionMatch offsetMatch = offsetRegex().match(content);
    if (offsetMatch.hasMatch())
        offsetMs = offsetMatch.captured(1).toLongLong();

    const QStringList rawLines = content.split(QLatin1Char('\n'));
    for (const QString &rawLine : rawLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        QRegularExpressionMatchIterator it = timestampRegex().globalMatch(line);
        if (!it.hasNext())
            continue; // [ti:]、[ar:]、纯文本行

        QVector<qint64> stamps;
        int textStart = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const qint64 stamp = match.captured(1).toLongLong() * 60000
                               + match.captured(2).toLongLong() * 1000
                               + fractionToMs(match.captured(3));
            stamps.append(stamp);
            textStart = match.capturedEnd();
        }

        const QString text = line.mid(textStart).trimmed();
        for (qint64 stamp : stamps)
            lines.append(LrcLine{stamp + offsetMs, text});
    }

    std::stable_sort(lines.begin(), lines.end(),
                     [](const LrcLine &lhs, const LrcLine &rhs) { return lhs.timeMs < rhs.timeMs; });
    return lines;
}

/// @brief 解码并解析原始 LRC 字节。
QVector<LrcLine> LrcParser::parseBytes(const QByteArray &data)
{
    return parse(decode(data));
}

/// @brief 查找 posMs 时刻正在播放的歌词行。
/// @param lines 已排序的歌词行。
/// @param posMs 当前播放位置（毫秒）。
/// @return 最后一条 timeMs <= posMs 的行下标；早于第一行时返回 -1。
/// @note 使用 upper_bound，时间复杂度 O(log n)，适合逐帧调用。
int LrcParser::indexAt(const QVector<LrcLine> &lines, qint64 posMs)
{
    if (lines.isEmpty())
        return -1;

    const auto it = std::upper_bound(lines.cbegin(), lines.cend(), posMs,
                                     [](qint64 value, const LrcLine &line) {
                                         return value < line.timeMs;
                                     });
    if (it == lines.cbegin())
        return -1;
    return static_cast<int>(std::distance(lines.cbegin(), it)) - 1;
}
