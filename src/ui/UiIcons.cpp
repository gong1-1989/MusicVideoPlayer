/// @file UiIcons.cpp
/// @brief 播放控制图标的内置绘制与外部图标覆盖实现。
#include "ui/UiIcons.h"

#include "storage/StoragePaths.h"
#include "ui/ThemeManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

#include <cmath>
#include <initializer_list>

namespace {

constexpr double kPi = 3.14159265358979323846;

/// @brief 把初始化列表拼成 QPolygonF，便于描述折线顶点。
QPolygonF makePolyline(std::initializer_list<QPointF> points)
{
    QPolygonF polygon;
    for (const QPointF &point : points)
        polygon << point;
    return polygon;
}

/// @brief 外部图标允许的文件后缀，按优先级从高到低排列。
QStringList iconSuffixes()
{
    return {QStringLiteral("svg"), QStringLiteral("png"), QStringLiteral("ico"),
            QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("webp"),
            QStringLiteral("bmp")};
}

/// @brief 把配置中的相对路径解析到程序目录，绝对路径原样返回。
QString resolveRelativeToApp(const QString &path)
{
    if (path.isEmpty())
        return {};
    QFileInfo info(path);
    return info.isAbsolute() ? path
                             : QDir(QCoreApplication::applicationDirPath()).filePath(path);
}

/// @brief 在指定目录中按后缀优先级查找同名图标文件。
/// @return 找到时返回绝对路径，否则返回空字符串。
QString findFileInDir(const QString &dir, const QString &name)
{
    for (const QString &suffix : iconSuffixes()) {
        const QString path = QDir(dir).filePath(name + QLatin1Char('.') + suffix);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

/// @brief 读取 Data/ui-icons.json 中为指定图标配置的路径。
/// @return 配置存在且文件真实存在时返回解析后的绝对路径；否则返回空。
/// @note JSON 损坏、字段为空或指向不存在的文件都按“未配置”处理，不阻断
///       内置图标兜底。
QString configuredPath(const QString &name)
{
    QFile file(UiIcons::configPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};

    const QString path = doc.object().value(name).toString().trimmed();
    if (path.isEmpty())
        return {};
    const QString resolved = resolveRelativeToApp(path);
    return QFileInfo::exists(resolved) ? resolved : QString();
}

/// @brief 按“显式配置 > 程序目录 icons/ > Data/icons/”的顺序查找外部图标。
QString externalIconPath(const QString &name)
{
    // 明确配置优先；随后允许按文件名直接覆盖，方便用户不改 JSON 也能换图标。
    const QString configured = configuredPath(name);
    if (!configured.isEmpty())
        return configured;

    const QStringList dirs = {UiIcons::externalIconDir(),
                              StoragePaths::dataFile(QStringLiteral("icons"))};
    for (const QString &dir : dirs) {
        const QString path = findFileInDir(dir, name);
        if (!path.isEmpty())
            return path;
    }
    return {};
}

/// @brief 创建透明画布并配置统一画笔。
/// @return 新建的 QPainter，调用方负责 delete。
/// @note 内置图标固定用 4.8 像素圆头画笔，缩放后仍保持柔和边缘。
QPainter *beginIcon(QPixmap *pixmap, const QColor &color)
{
    pixmap->fill(Qt::transparent);
    auto *painter = new QPainter(pixmap);
    painter->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color);
    pen.setWidthF(4.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(color);
    return painter;
}

/// @brief 在折线末端绘制实心箭头，用于循环/随机模式图标。
/// @param tip 箭头尖端坐标。
/// @param direction 箭头指向的方向向量，内部会归一化。
void drawArrowHead(QPainter *painter, const QPointF &tip, const QPointF &direction)
{
    const QPointF unit = direction / std::hypot(direction.x(), direction.y());
    const QPointF normal(-unit.y(), unit.x());
    const QPointF base = tip - unit * 10.0;
    QPolygonF head;
    head << tip << (base + normal * 5.5) << (base - normal * 5.5);
    painter->drawPolygon(head);
}

/// @brief 按名称绘制内置图标。
/// @param name 图标名称，必须与按钮调用处传入的稳定字符串一致。
/// @param color 线条与填充颜色，通常来自当前主题强调色。
/// @param pixelSize 输出位图边长（像素）。
/// @return 透明背景的图标位图；未知名称绘制圆点兜底，避免空按钮。
/// @note 所有图形坐标都按 64x64 设计，再按 pixelSize/64 等比缩放，
///       因此新增图标时必须使用同一坐标系。
QPixmap builtinPixmap(const QString &name, const QColor &color, int pixelSize)
{
    QPixmap pixmap(pixelSize, pixelSize);
    QPainter *painter = beginIcon(&pixmap, color);

    const qreal scale = pixelSize / 64.0;
    painter->scale(scale, scale);

    if (name == QLatin1String("play")) {
        QPolygonF triangle;
        triangle << QPointF(22, 14) << QPointF(22, 50) << QPointF(51, 32);
        painter->drawPolygon(triangle);
    } else if (name == QLatin1String("pause")) {
        painter->drawRoundedRect(QRectF(21, 15, 8, 34), 3, 3);
        painter->drawRoundedRect(QRectF(35, 15, 8, 34), 3, 3);
    } else if (name == QLatin1String("stop")) {
        painter->drawRoundedRect(QRectF(18, 18, 28, 28), 5, 5);
    } else if (name == QLatin1String("prev")) {
        painter->drawRoundedRect(QRectF(14, 17, 6, 30), 2, 2);
        QPolygonF triangle;
        triangle << QPointF(48, 14) << QPointF(48, 50) << QPointF(23, 32);
        painter->drawPolygon(triangle);
    } else if (name == QLatin1String("next")) {
        painter->drawRoundedRect(QRectF(44, 17, 6, 30), 2, 2);
        QPolygonF triangle;
        triangle << QPointF(16, 14) << QPointF(16, 50) << QPointF(41, 32);
        painter->drawPolygon(triangle);
    } else if (name == QLatin1String("seek-back")) {
        // 双三角表示快退，不含竖线，避免和“上一首”的单三角+竖线混淆。
        QPolygonF first;
        first << QPointF(31, 14) << QPointF(31, 50) << QPointF(13, 32);
        painter->drawPolygon(first);
        QPolygonF second;
        second << QPointF(52, 14) << QPointF(52, 50) << QPointF(34, 32);
        painter->drawPolygon(second);
    } else if (name == QLatin1String("seek-forward")) {
        // 双三角表示快进，与“下一首”的三角+竖线保持区分。
        QPolygonF first;
        first << QPointF(12, 14) << QPointF(12, 50) << QPointF(30, 32);
        painter->drawPolygon(first);
        QPolygonF second;
        second << QPointF(33, 14) << QPointF(33, 50) << QPointF(51, 32);
        painter->drawPolygon(second);
    } else if (name == QLatin1String("mode-list")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawArc(QRectF(14, 14, 36, 36), 45 * 16, 285 * 16);
        drawArrowHead(painter, QPointF(49, 37), QPointF(3, 10));
    } else if (name == QLatin1String("mode-single")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawArc(QRectF(13, 13, 38, 38), 40 * 16, 292 * 16);
        drawArrowHead(painter, QPointF(49, 38), QPointF(3, 10));
        QFont font = painter->font();
        font.setBold(true);
        font.setPixelSize(18);
        painter->setFont(font);
        painter->setPen(color);
        painter->drawText(QRectF(20, 17, 24, 30), Qt::AlignCenter, QStringLiteral("1"));
    } else if (name == QLatin1String("mode-random")) {
        painter->setBrush(Qt::NoBrush);
        QPainterPath upper;
        upper.moveTo(14, 20);
        upper.lineTo(30, 20);
        upper.lineTo(49, 42);
        painter->drawPath(upper);
        drawArrowHead(painter, QPointF(52, 45), QPointF(10, 10));

        QPainterPath lower;
        lower.moveTo(14, 44);
        lower.lineTo(30, 44);
        lower.lineTo(49, 22);
        painter->drawPath(lower);
        drawArrowHead(painter, QPointF(52, 19), QPointF(10, -10));
    } else if (name == QLatin1String("fullscreen")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(makePolyline(
            {QPointF(14, 26), QPointF(14, 14), QPointF(26, 14)}));
        painter->drawPolyline(makePolyline(
            {QPointF(38, 14), QPointF(50, 14), QPointF(50, 26)}));
        painter->drawPolyline(makePolyline(
            {QPointF(50, 38), QPointF(50, 50), QPointF(38, 50)}));
        painter->drawPolyline(makePolyline(
            {QPointF(26, 50), QPointF(14, 50), QPointF(14, 38)}));
    } else if (name == QLatin1String("settings")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(32, 32), 14, 14);
        painter->drawEllipse(QPointF(32, 32), 5, 5);
        for (int i = 0; i < 8; ++i) {
            const qreal angle = i * kPi / 4.0;
            const QPointF from(32 + std::cos(angle) * 17, 32 + std::sin(angle) * 17);
            const QPointF to(32 + std::cos(angle) * 23, 32 + std::sin(angle) * 23);
            painter->drawLine(from, to);
        }
    } else if (name == QLatin1String("volume")) {
        QPolygonF speaker;
        speaker << QPointF(12, 26) << QPointF(21, 26) << QPointF(32, 16)
                << QPointF(32, 48) << QPointF(21, 38) << QPointF(12, 38);
        painter->drawPolygon(speaker);
        painter->setBrush(Qt::NoBrush);
        painter->drawArc(QRectF(24, 20, 22, 24), -55 * 16, 110 * 16);
        painter->drawArc(QRectF(25, 12, 34, 40), -50 * 16, 100 * 16);
    } else if (name == QLatin1String("search")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(27, 27), 12, 12);
        painter->drawLine(QPointF(36, 36), QPointF(50, 50));
    } else if (name == QLatin1String("cancel")) {
        painter->drawLine(QPointF(20, 20), QPointF(44, 44));
        painter->drawLine(QPointF(44, 20), QPointF(20, 44));
    } else if (name == QLatin1String("page-prev")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(makePolyline(
            {QPointF(40, 15), QPointF(23, 32), QPointF(40, 49)}));
    } else if (name == QLatin1String("page-next")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(makePolyline(
            {QPointF(24, 15), QPointF(41, 32), QPointF(24, 49)}));
    } else if (name == QLatin1String("downloads")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawLine(QPointF(32, 13), QPointF(32, 36));
        painter->drawPolyline(makePolyline(
            {QPointF(22, 28), QPointF(32, 39), QPointF(42, 28)}));
        painter->drawPolyline(makePolyline(
            {QPointF(16, 41), QPointF(16, 51), QPointF(48, 51), QPointF(48, 41)}));
    } else if (name == QLatin1String("fullscreen-exit")) {
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(makePolyline(
            {QPointF(26, 26), QPointF(26, 15), QPointF(15, 15)}));
        painter->drawPolyline(makePolyline(
            {QPointF(38, 26), QPointF(38, 15), QPointF(49, 15)}));
        painter->drawPolyline(makePolyline(
            {QPointF(38, 38), QPointF(38, 49), QPointF(49, 49)}));
        painter->drawPolyline(makePolyline(
            {QPointF(26, 38), QPointF(26, 49), QPointF(15, 49)}));
    } else {
        // 未知名称用圆点兜底，至少不会出现空按钮。
        painter->drawEllipse(QPointF(32, 32), 9, 9);
    }

    delete painter;
    return pixmap;
}

} // namespace

namespace UiIcons {

/// @brief 返回程序目录下的外部图标目录。
/// @note 仅用于查找与文档说明，不保证目录存在，也不会自动创建。
QString externalIconDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("icons"));
}

/// @brief 返回图标映射配置文件路径（Data/ui-icons.json）。
QString configPath()
{
    return StoragePaths::dataFile(QStringLiteral("ui-icons.json"));
}

/// @brief 取得指定名称的图标。
/// @param name 图标名称，例如 "play"、"settings"。
/// @param color 内置图标的绘制颜色。
/// @param pixelSize 内置位图边长，外部图标由 QIcon 自行缩放。
/// @return 优先返回外部自定义图标；加载失败时回落到内置绘制图标。
QIcon iconFor(const QString &name, const QColor &color, int pixelSize)
{
    const QString external = externalIconPath(name);
    if (!external.isEmpty()) {
        QIcon icon(external);
        if (!icon.isNull())
            return icon;
    }
    return QIcon(builtinPixmap(name, color, pixelSize));
}

/// @brief 把按钮改为纯图标样式，并补齐提示与无障碍信息。
/// @param button 目标按钮；空指针直接忽略。
/// @param name 图标名称。
/// @param tooltip 悬停提示，同时作为 accessibleName。
/// @param iconSize 图标显示边长（像素）。
/// @param color 内置图标颜色；无效时依次回落到主题强调色和按钮文字色。
void apply(QAbstractButton *button, const QString &name, const QString &tooltip,
           int iconSize, const QColor &color)
{
    if (button == nullptr)
        return;

    QColor resolved = color;
    if (!resolved.isValid())
        resolved = ThemeManager::instance().current().accentColor;
    if (!resolved.isValid())
        resolved = qApp->palette().color(QPalette::ButtonText);

    button->setIcon(iconFor(name, resolved, 64));
    button->setIconSize(QSize(iconSize, iconSize));
    button->setText(QString());
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
}

} // namespace UiIcons
