#ifndef UIICONS_H
#define UIICONS_H

#include <QColor>
#include <QIcon>
#include <QString>

class QAbstractButton;

/// 播放控制图标：内置图标由 QPainter 按当前主题色绘制，用户可在程序目录
/// icons/ 或 Data/icons/ 放同名文件覆盖，也可用 Data/ui-icons.json 指定路径。
namespace UiIcons {

/// 外部自定义图标目录（仅用于文档/诊断，不强制创建）。
QString externalIconDir();
QString configPath();

/// 取得图标：优先外部自定义，找不到时返回内置绘制图标。
QIcon iconFor(const QString &name, const QColor &color, int pixelSize = 64);

/// 把按钮改成纯图标样式，并写入悬停提示与无障碍名称。
void apply(QAbstractButton *button, const QString &name, const QString &tooltip,
           int iconSize = 20, const QColor &color = QColor());

} // namespace UiIcons

#endif // UIICONS_H
