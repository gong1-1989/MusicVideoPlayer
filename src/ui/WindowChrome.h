#ifndef WINDOWCHROME_H
#define WINDOWCHROME_H

#include <QColor>

class QWidget;

/// 给 Windows 原生标题栏上色：
/// - Win10 1809+：切到深色标题栏（不再是一条白条）；
/// - Win11 22H2+：还能直接指定标题栏底色和标题文字色，跟主题完全对齐。
/// 其它平台是空实现，调用方不需要判断。
namespace WindowChrome {

void apply(QWidget *window, bool dark, const QColor &captionColor = QColor(),
           const QColor &textColor = QColor());

} // namespace WindowChrome

#endif // WINDOWCHROME_H
