#ifndef WINDOWPRESET_H
#define WINDOWPRESET_H

#include <QCoreApplication>
#include <QLatin1String>
#include <QSize>
#include <QString>

/// 窗口尺寸预设。界面下拉框与持久化都用 id 字符串，避免枚举序号漂移。
/// 尺寸用标准分辨率，屏幕装不下时由界面层按可用工作区收一收。
enum class WindowPreset {
    Auto,       ///< 自适应：按可用工作区铺开（推荐）
    P720,       ///< 1280 x 720
    P768,       ///< 1366 x 768（笔记本常见）
    P900,       ///< 1600 x 900
    P1080,      ///< 1920 x 1080
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
        {WindowPreset::Auto, "auto", QSize(0, 0)},
        {WindowPreset::P720, "720p", QSize(1280, 720)},
        {WindowPreset::P768, "768p", QSize(1366, 768)},
        {WindowPreset::P900, "900p", QSize(1600, 900)},
        {WindowPreset::P1080, "1080p", QSize(1920, 1080)},
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
    return QStringLiteral("auto");
}

inline WindowPreset windowPresetFromId(const QString &id)
{
    // 老版本存的是 small/standard/large/cinema，映射到最接近的标准分辨率
    if (id == QLatin1String("small") || id == QLatin1String("standard")
        || id == QLatin1String("large"))
        return WindowPreset::P720;
    if (id == QLatin1String("cinema"))
        return WindowPreset::P900;

    int count = 0;
    const WindowPresetEntry *table = windowPresetTable(&count);
    for (int i = 0; i < count; ++i) {
        if (id == QLatin1String(table[i].id))
            return table[i].preset;
    }
    return WindowPreset::Auto;
}

#endif // WINDOWPRESET_H
