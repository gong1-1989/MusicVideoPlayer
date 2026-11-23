/// @file ThemeManager.cpp
/// @brief 主题元数据、QSS 加载与字号缩放实现。
#include "ui/ThemeManager.h"

#include "ui/WindowChrome.h"

#include <QApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QWidget>

Q_LOGGING_CATEGORY(lcTheme, "mvp.ui.theme")

/// @brief 返回进程级单例。
/// @note 主题是全局状态，所有窗口共用同一份当前主题。
ThemeManager &ThemeManager::instance()
{
    static ThemeManager manager;
    return manager;
}

/// @brief 构造内置主题表。
/// @note QSS 只负责静态样式；fadeColor/accentColor/stageBorder 供自绘控件取色。
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

/// @brief 按 id 查找主题；未知 id 回落到第一个内置主题。
ThemeInfo ThemeManager::find(const QString &id) const
{
    for (const ThemeInfo &theme : m_themes) {
        if (theme.id == id)
            return theme;
    }
    return m_themes.isEmpty() ? ThemeInfo{} : m_themes.first();
}

/// @brief 返回当前生效主题的元数据。
ThemeInfo ThemeManager::current() const
{
    return find(m_currentId);
}

/// @brief 加载指定主题的 QSS，按当前字号缩放后应用到 QApplication。
/// @return 样式文件缺失或读取失败返回 false，并保留当前样式。
/// @note 只有成功读到 QSS 才更新 m_currentId 并发出 themeChanged，避免
///       界面切到半成品主题。
bool ThemeManager::apply(const QString &id)
{
    const ThemeInfo theme = find(id);

    QFile file(theme.qssPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(lcTheme) << "主题样式文件缺失：" << theme.qssPath;
        return false;
    }

    const QString styleSheet = ThemeManager::scaledStyleSheet(
        QString::fromUtf8(file.readAll()), AppSettings::uiFontSizePx());
    if (qApp != nullptr)
        qApp->setStyleSheet(styleSheet);

    m_currentId = theme.id;
    qCInfo(lcTheme) << "已应用主题：" << theme.id
                    << "界面字号：" << AppSettings::uiFontSizePx() << "px";
    emit themeChanged(theme);
    return true;
}

/// @brief 按基础字号缩放 QSS 中所有 font-size: Npx。
/// @param qss 原始样式表。
/// @param basePx 用户选择的界面基础字号，内部夹到 9..20。
/// @return 调整后的样式表；basePx 为 13 时原样返回。
/// @note 采用相对差值而不是统一替换，保留正文/辅助文字之间的层级差。
QString ThemeManager::scaledStyleSheet(const QString &qss, int basePx)
{
    const int bounded = qBound(9, basePx, 20);
    // 主题以 13px 为正文基准；用户选 15px 时，12/11px 的辅助文字也同步上移 2px，
    // 保持“正文/辅助文字”的层级差，而不是把所有字号粗暴拉成同一个值。
    const int delta = bounded - 13;
    if (delta == 0)
        return qss;

    QRegularExpression expression(QStringLiteral("font-size\\s*:\\s*(\\d+)\\s*px"));
    QString scaled;
    int cursor = 0;
    auto matches = expression.globalMatch(qss);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        scaled += qss.mid(cursor, match.capturedStart() - cursor);
        const int sourcePx = match.captured(1).toInt();
        const int targetPx = qMax(9, sourcePx + delta);
        scaled += QStringLiteral("font-size: %1px").arg(targetPx);
        cursor = match.capturedEnd();
    }
    scaled += qss.mid(cursor);
    return scaled;
}

/// @brief 按 AppSettings 中保存的主题 id 应用主题。
bool ThemeManager::applySaved()
{
    return apply(AppSettings::themeId());
}

/// @brief 给窗口原生标题栏套用当前主题的明暗与配色。
/// @param window 目标窗口；内部按 fadeColor 亮度判断深/浅色。
/// @note 仅在 Windows 生效，其他平台由 WindowChrome 空实现。
void ThemeManager::applyWindowChrome(QWidget *window) const
{
    const ThemeInfo theme = current();
    const bool dark = theme.fadeColor.lightness() < 128;
    // 标题栏底色跟面板底色一致，标题文字用与主题正文接近的浅/深色
    const QColor text = dark ? QColor(0xdd, 0xe4, 0xff) : QColor(0x33, 0x33, 0x4f);
    WindowChrome::apply(window, dark, theme.fadeColor, text);
}
