#include "ui/ThemeManager.h"

#include "ui/WindowChrome.h"

#include <QApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QWidget>

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
        qCWarning(lcTheme) << "主题样式文件缺失：" << theme.qssPath;
        return false;
    }

    const QString styleSheet = ThemeManager::scaledStyleSheet(
        QString::fromUtf8(file.readAll()), AppSettings::uiFontPercent());
    if (qApp != nullptr)
        qApp->setStyleSheet(styleSheet);

    m_currentId = theme.id;
    qCInfo(lcTheme) << "已应用主题：" << theme.id
                    << "字体缩放：" << AppSettings::uiFontPercent() << "%";
    emit themeChanged(theme);
    return true;
}

QString ThemeManager::scaledStyleSheet(const QString &qss, int percent)
{
    const int bounded = qBound(80, percent, 150);
    if (bounded == 100)
        return qss;

    // 主题里的字号混用 12/13/15/16px，只替换数值，保留原有选择器与其它样式。
    QRegularExpression expression(QStringLiteral("font-size\\s*:\\s*(\\d+)\\s*px"));
    QString scaled;
    int cursor = 0;
    auto matches = expression.globalMatch(qss);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        scaled += qss.mid(cursor, match.capturedStart() - cursor);
        const int sourcePx = match.captured(1).toInt();
        const int targetPx = qMax(9, qRound(sourcePx * bounded / 100.0));
        scaled += QStringLiteral("font-size: %1px").arg(targetPx);
        cursor = match.capturedEnd();
    }
    scaled += qss.mid(cursor);
    return scaled;
}

bool ThemeManager::applySaved()
{
    return apply(AppSettings::themeId());
}

void ThemeManager::applyWindowChrome(QWidget *window) const
{
    const ThemeInfo theme = current();
    const bool dark = theme.fadeColor.lightness() < 128;
    // 标题栏底色跟面板底色一致，标题文字用与主题正文接近的浅/深色
    const QColor text = dark ? QColor(0xdd, 0xe4, 0xff) : QColor(0x33, 0x33, 0x4f);
    WindowChrome::apply(window, dark, theme.fadeColor, text);
}
