#include "settings/AppShortcuts.h"

#include "settings/AppSettings.h"

#include <QKeySequence>

namespace {

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

const QVector<Action> &actions()
{
    return kActionList;
}

const Action *findAction(const QString &id)
{
    for (const Action &action : kActionList) {
        if (action.id == id)
            return &action;
    }
    return nullptr;
}

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

QHash<QString, QString> defaultBindings()
{
    QHash<QString, QString> bindings;
    for (const Action &action : kActionList)
        bindings.insert(action.id, normalizeSequence(action.defaultSequence));
    return bindings;
}

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

void storeBindings(const QHash<QString, QString> &bindings)
{
    for (const Action &action : kActionList) {
        if (!bindings.contains(action.id))
            continue;
        AppSettings::setShortcut(action.id, normalizeSequence(bindings.value(action.id)));
    }
}

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
