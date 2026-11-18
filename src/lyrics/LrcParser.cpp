#include "lyrics/LrcParser.h"

#include <QRegularExpression>
#include <QtCore/qstringconverter.h>

#include <algorithm>

namespace {

const QByteArray &utf8Bom()
{
    static const QByteArray bom = QByteArray::fromHex("EFBBBF");
    return bom;
}

/// [mm:ss] / [mm:ss.xx] / [mm:ss:xx]，分最多 3 位，秒最多 2 位，小数最多 3 位
const QRegularExpression &timestampRegex()
{
    static const QRegularExpression rx(
        QStringLiteral(R"(\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])"));
    return rx;
}

const QRegularExpression &offsetRegex()
{
    static const QRegularExpression rx(
        QStringLiteral(R"(\[offset:\s*([+-]?\d+)\s*\])"),
        QRegularExpression::CaseInsensitiveOption);
    return rx;
}

/// 小数位按位数归一：1 位=十分之一秒，2 位=厘秒，3 位=毫秒。
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

QVector<LrcLine> LrcParser::parseBytes(const QByteArray &data)
{
    return parse(decode(data));
}

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
