/// @file TimeFormat.cpp
/// @brief 播放器时间文本格式化。
#include "util/TimeFormat.h"

/// @brief 把毫秒数格式化为播放器时间文本。
/// @param ms 从媒体起点开始的毫秒数；负值表示时长未知或无效。
/// @return 负值返回 "--:--"；不足一小时返回 mm:ss，否则返回 h:mm:ss。
/// @note 分钟和秒钟始终补足两位，小时不补零，保证进度栏宽度稳定。
QString TimeFormat::formatMs(qint64 ms)
{
    if (ms < 0)
        return QStringLiteral("--:--");

    const qint64 totalSeconds = ms / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
