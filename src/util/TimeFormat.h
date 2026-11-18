#ifndef TIMEFORMAT_H
#define TIMEFORMAT_H

#include <QString>
#include <qglobal.h>

namespace TimeFormat {

/// 毫秒转 mm:ss，超过一小时输出 h:mm:ss；非法值（< 0）返回 "--:--"。
QString formatMs(qint64 ms);

} // namespace TimeFormat

#endif // TIMEFORMAT_H
