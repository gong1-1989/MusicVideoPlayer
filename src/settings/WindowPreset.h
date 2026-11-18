#ifndef WINDOWPRESET_H
#define WINDOWPRESET_H

#include <QCoreApplication>
#include <QSize>
#include <QString>

/// 窗口尺寸预设。界面下拉框与持久化都用 id 字符串，避免枚举序号漂移。
enum class WindowPreset {
    Small,      ///< 1280 x 800
    Standard,   ///< 1360 x 840
    Large,      ///< 1440 x 900
    Cinema,     ///< 1600 x 900
    Last        ///< 记住上次关闭时的大小
};

struct WindowPresetEntry {
    WindowPreset preset;
    const char *id;
    QSize size;
};

inline const WindowPresetEntry *windowPresetTable(int *count)
{
    static const WindowPresetEntry kTable[] = {
        {WindowPreset::Small, "small", QSize(1200, 660)},
        {WindowPreset::Standard, "standard", QSize(1280, 700)},
        {WindowPreset::Large, "large", QSize(1340, 720)},
        {WindowPreset::Cinema, "cinema", QSize(1440, 780)},
        {WindowPreset::Last, "last", QSize(0, 0)},
    };
    if (count != nullptr)
        *count = int(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}

inline QString windowPresetId(WindowPreset preset)
{
    int count = 0;
    const WindowPresetEntry *table = windowPresetTable(&count);
    for (int i = 0; i < count; ++i) {
        if (table[i].preset == preset)
            return QString::fromLatin1(table[i].id);
    }
    return QStringLiteral("standard");
}

inline WindowPreset windowPresetFromId(const QString &id)
{
    int count = 0;
    const WindowPresetEntry *table = windowPresetTable(&count);
    for (int i = 0; i < count; ++i) {
        if (id == QLatin1String(table[i].id))
            return table[i].preset;
    }
    return WindowPreset::Standard;
}

#endif // WINDOWPRESET_H
