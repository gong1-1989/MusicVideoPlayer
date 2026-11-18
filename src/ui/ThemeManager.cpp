#include "ui/ThemeManager.h"

#include <QApplication>
#include <QFile>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcTheme, "mvp.ui.theme")

ThemeManager &ThemeManager::instance()
{
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager()
{
    m_themes = {
        {QStringLiteral("dark"), QStringLiteral("深色梦幻"),
         QStringLiteral(":/themes/dark.qss"), QColor(0x10, 0x12, 0x26), QColor(0x6f, 0xe6, 0xff),
         QColor(120, 170, 235, 60)},
        {QStringLiteral("light"), QStringLiteral("轻快明亮梦幻"),
         QStringLiteral(":/themes/light.qss"), QColor(0xfd, 0xfc, 0xff), QColor(0x6a, 0x4b, 0xff),
         QColor(120, 130, 220, 70)},
    };
    m_currentId = m_themes.isEmpty() ? QString() : m_themes.first().id;
}

ThemeInfo ThemeManager::find(const QString &id) const
{
    for (const ThemeInfo &theme : m_themes) {
        if (theme.id == id)
            return theme;
    }
    return m_themes.isEmpty() ? ThemeInfo{} : m_themes.first();
}

ThemeInfo ThemeManager::current() const
{
    return find(m_currentId);
}

bool ThemeManager::apply(const QString &id)
{
    const ThemeInfo theme = find(id);

    QFile file(theme.qssPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(lcTheme) << "theme file missing:" << theme.qssPath;
        return false;
    }

    const QString styleSheet = QString::fromUtf8(file.readAll());
    if (qApp != nullptr)
        qApp->setStyleSheet(styleSheet);

    m_currentId = theme.id;
    qCInfo(lcTheme) << "theme applied:" << theme.id;
    emit themeChanged(theme);
    return true;
}

bool ThemeManager::applySaved()
{
    return apply(AppSettings::themeId());
}
