#ifndef APPSHORTCUTS_H
#define APPSHORTCUTS_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

/// 快捷键动作表与冲突检测。动作 id 稳定落盘，界面只按 id 读写；
/// 默认值集中在 defaultBindings()，设置页和主窗口共用同一份定义。
namespace AppShortcuts {

struct Action
{
    QString id;
    /// 面向用户的动作名；强对立动作写成“播放 / 暂停”“全屏 / 退出全屏”
    QString label;
    QString defaultSequence;
    bool toggle = false;
};

struct Conflict
{
    /// 规范化后的按键文本，例如 "Ctrl+S"
    QString sequence;
    /// 占用该按键的全部动作 id
    QStringList actionIds;
    QStringList labels;
};

const QVector<Action> &actions();
const Action *findAction(const QString &id);

/// 未配置的动作使用缺省值；显式清空的动作为空，不会回落到默认值。
QHash<QString, QString> defaultBindings();
QHash<QString, QString> currentBindings();

/// 按动作表落盘；不在动作表里的键会被忽略，避免旧版本残留键继续生效。
void storeBindings(const QHash<QString, QString> &bindings);

/// 把用户输入规范化为 PortableText；空输入返回空字符串。
QString normalizeSequence(const QString &text);

/// 检测重复绑定。空快捷键不参与冲突；同一动作内部永远不冲突。
QVector<Conflict> conflicts(const QHash<QString, QString> &bindings);

/// 把冲突整理成可直接展示给用户的中文提示。
QString conflictMessage(const QVector<Conflict> &conflicts);

} // namespace AppShortcuts

#endif // APPSHORTCUTS_H
