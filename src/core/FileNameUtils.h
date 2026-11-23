#ifndef FILENAMEUTILS_H
#define FILENAMEUTILS_H

#include <QChar>
#include <QString>
#include <QStringList>

/// Windows 文件名/目录名清洗。下载落盘与播放缓存各有一套规则时，
/// 保留名、结尾点、控制字符的处理很容易分叉；这里保留一份共同实现。
namespace FileNameUtils {

/// 清洗单个路径分量（不含目录和扩展名）。
/// maxLength 已包含调用方自己追加的扩展名，避免拼完再截断。
inline QString sanitizeComponent(const QString &raw, const QString &fallback, int maxLength)
{
    QString name;
    name.reserve(raw.size());
    const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (const QChar ch : raw) {
        if (ch.category() == QChar::Other_Control || invalid.contains(ch))
            name.append(QLatin1Char('_'));
        else
            name.append(ch);
    }
    name = name.simplified();
    while (!name.isEmpty()
           && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))) {
        name.chop(1);
    }

    static const QStringList reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
    };
    // Windows 的保留设备名按“第一个点之前的主干”判断：CON.mp3、CON.tar.gz
    // 都是非法文件名。只比较完整字符串会漏掉带扩展名的标题。
    const auto reservedStem = [](const QString &value) {
        return reserved.contains(value.section(QLatin1Char('.'), 0, 0).trimmed(),
                                 Qt::CaseInsensitive);
    };
    const auto avoidReserved = [&reservedStem](QString &value, int limit) {
        if (!reservedStem(value))
            return;
        // 加前缀后仍不能超过调用方给定的长度；先截断再补下划线。
        if (value.size() >= limit)
            value.truncate(qMax(0, limit - 1));
        value.prepend(QLatin1Char('_'));
    };
    avoidReserved(name, maxLength);

    if (name.size() > maxLength)
        name.truncate(qMax(1, maxLength));
    // 截断后可能又落到保留名或结尾点，最后再收一次尾。
    while (!name.isEmpty()
           && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))) {
        name.chop(1);
    }
    avoidReserved(name, maxLength);
    if (name.isEmpty())
        name = fallback;
    return name;
}

} // namespace FileNameUtils

#endif // FILENAMEUTILS_H
