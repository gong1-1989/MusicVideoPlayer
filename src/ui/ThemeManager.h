#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include "settings/AppSettings.h"

#include <QColor>
#include <QObject>
#include <QString>
#include <QVector>

/// 外观主题元数据：样式表在资源里，C++ 只保留自绘控件需要的那几个颜色。
/// QSS 负责能静态描述的一切，自绘（视频圆角、歌词渐隐）从这里取色。
struct ThemeInfo {
    QString id;
    QString name;
    QString qssPath;      ///< 形如 ":/themes/dark.qss"
    QColor fadeColor;     ///< 歌词面板渐隐用的底色
    QColor accentColor;   ///< 当前歌词行等强调色
    QColor stageBorder;   ///< 视频舞台描边
};

class ThemeManager : public QObject
{
    Q_OBJECT

public:
    static ThemeManager &instance();

    const QVector<ThemeInfo> &themes() const { return m_themes; }
    QString currentId() const { return m_currentId; }
    ThemeInfo current() const;

    /// 加载并应用主题；失败时返回 false（保留当前样式）
    bool apply(const QString &id);
    /// 按设置里的主题应用
    bool applySaved();

signals:
    void themeChanged(const ThemeInfo &theme);

private:
    ThemeManager();
    ThemeInfo find(const QString &id) const;

    QVector<ThemeInfo> m_themes;
    QString m_currentId;
};

#endif // THEMEMANAGER_H
