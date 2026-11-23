/// @file AppShortcuts.cpp
/// @brief 快捷键动作表、默认绑定、持久化与冲突检测实现。
#include "settings/AppShortcuts.h"

#include "settings/AppSettings.h"

#include <QKeySequence>

namespace {

/// 全局动作表。id 是写入 QSettings 的稳定键，禁止随界面文案变化而改名；
/// label 只用于设置页和冲突提示，toggle 仅表达该动作是否为开关语义。
const AppShortcuts::Action kActions[] = {
    {QStringLiteral("playPause"), QStringLiteral("播放 / 暂停（切换）"),
     QStringLiteral("Space"), true},
    {QStringLiteral("stop"), QStringLiteral("停止播放"), QStringLiteral("Ctrl+."), false},
    {QStringLiteral("previous"), QStringLiteral("上一首 / 上一个视频"),
     QStringLiteral("Ctrl+Left"), false},
    {QStringLiteral("next"), QStringLiteral("下一首 / 下一个视频"),
     QStringLiteral("Ctrl+Right"), false},
    {QStringLiteral("fullscreen"), QStringLiteral("全屏 / 退出全屏（切换）"),
     QStringLiteral("F11"), true},
    {QStringLiteral("settings"), QStringLiteral("打开系统设置"),
     QStringLiteral("Ctrl+,"), false},
    {QStringLiteral("savePlaylist"), QStringLiteral("保存播放列表"),
     QStringLiteral("Ctrl+S"), false},
    {QStringLiteral("search"), QStringLiteral("打开搜索发现"),
     QStringLiteral("Ctrl+F"), false},
    {QStringLiteral("downloads"), QStringLiteral("打开下载任务"),
     QStringLiteral("Ctrl+J"), false},
    {QStringLiteral("volumeUp"), QStringLiteral("音量 +"),
     QStringLiteral("Ctrl+Up"), false},
    {QStringLiteral("volumeDown"), QStringLiteral("音量 -"),
     QStringLiteral("Ctrl+Down"), false},
};

const QVector<AppShortcuts::Action> kActionList = [] {
    QVector<AppShortcuts::Action> list;
    for (const auto &action : kActions)
        list.append(action);
    return list;
}();

} // namespace

namespace AppShortcuts {

/// @brief 返回完整的快捷键动作表。
/// @return 进程生命周期内不变的只读列表。
const QVector<Action> &actions()
{
    return kActionList;
}

/// @brief 按稳定动作 id 查找动作定义。
/// @param id 动作表中的稳定键，例如 "playPause"。
/// @return 命中时返回动作指针；未知 id 返回 nullptr。
const Action *findAction(const QString &id)
{
    for (const Action &action : kActionList) {
        if (action.id == id)
            return &action;
    }
    return nullptr;
}

/// @brief 把用户输入的快捷键文本规范化为可持久化的 PortableText。
/// @param text 用户输入的文本，例如 "Ctrl+S" 或 "Space"。
/// @return 规范化后的文本；空输入或 Qt 无法解析时返回空字符串。
/// @note 使用 PortableText 而不是 NativeText，避免不同平台落盘格式不一致。
QString normalizeSequence(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QString();
    const QKeySequence sequence =
        QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (sequence.isEmpty())
        return QString();
    return sequence.toString(QKeySequence::PortableText);
}

/// @brief 生成动作表的完整默认绑定。
/// @return 动作 id 到规范化快捷键的映射；默认值为空的动作也会保留空值。
QHash<QString, QString> defaultBindings()
{
    QHash<QString, QString> bindings;
    for (const Action &action : kActionList)
        bindings.insert(action.id, normalizeSequence(action.defaultSequence));
    return bindings;
}

/// @brief 读取当前生效的绑定，缺失配置回落到动作表默认值。
/// @return 动作 id 到快捷键的映射。
/// @note 用户显式清空的快捷键会保存为空字符串，因此不能用“是否为空”判断
///       是否需要回落；这里只按 AppSettings::hasShortcut() 判断是否已配置。
QHash<QString, QString> currentBindings()
{
    QHash<QString, QString> bindings;
    for (const Action &action : kActionList) {
        const QString stored = AppSettings::hasShortcut(action.id)
                                   ? normalizeSequence(AppSettings::shortcut(action.id))
                                   : normalizeSequence(action.defaultSequence);
        bindings.insert(action.id, stored);
    }
    return bindings;
}

/// @brief 把绑定写回设置，只处理动作表中已知的 id。
/// @param bindings 待保存的动作 id 到快捷键映射。
/// @note 未知 id 会被忽略，防止旧版本或手工编辑的残留键在升级后继续生效。
void storeBindings(const QHash<QString, QString> &bindings)
{
    for (const Action &action : kActionList) {
        if (!bindings.contains(action.id))
            continue;
        AppSettings::setShortcut(action.id, normalizeSequence(bindings.value(action.id)));
    }
}

/// @brief 检测同一快捷键被多个动作占用的情况。
/// @param bindings 待检测的动作 id 到快捷键映射。
/// @return 每个冲突按键对应一条记录；空快捷键不参与冲突。
/// @note 同一动作重复出现不会产生冲突；结果按 QHash 迭代顺序排列，调用方
///       只应把它用于提示，不应依赖固定顺序。
QVector<Conflict> conflicts(const QHash<QString, QString> &bindings)
{
    QHash<QString, QStringList> owners;
    for (const Action &action : kActionList) {
        const QString sequence = normalizeSequence(bindings.value(action.id));
        if (sequence.isEmpty())
            continue;
        owners[sequence].append(action.id);
    }

    QVector<Conflict> result;
    for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
        if (it.value().size() > 1) {
            Conflict conflict;
            conflict.sequence = it.key();
            conflict.actionIds = it.value();
            for (const QString &id : it.value()) {
                const Action *action = findAction(id);
                conflict.labels.append(action != nullptr ? action->label : id);
            }
            result.append(conflict);
        }
    }
    return result;
}

/// @brief 把冲突列表整理成可直接展示的中文多行文本。
/// @param conflicts conflicts() 返回的冲突记录。
/// @return 每行形如 "Ctrl+S：保存播放列表、打开搜索发现"；无冲突时返回空字符串。
QString conflictMessage(const QVector<Conflict> &conflicts)
{
    QStringList lines;
    for (const Conflict &conflict : conflicts) {
        lines.append(QStringLiteral("%1：%2").arg(conflict.sequence,
                                                   conflict.labels.join(QStringLiteral("、"))));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace AppShortcuts
