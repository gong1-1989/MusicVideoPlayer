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

QPolygonF makePolyline(std::initializer_list<QPointF> points)
{
    QPolygonF polygon;
    for (const QPointF &point : points)
        polygon << point;
    return polygon;
}

QStringList iconSuffixes()
{
    return {QStringLiteral("svg"), QStringLiteral("png"), QStringLiteral("ico"),
            QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("webp"),
            QStringLiteral("bmp")};
}

QString resolveRelativeToApp(const QString &path)
{
    if (path.isEmpty())
        return {};
    QFileInfo info(path);
    return info.isAbsolute() ? path
                             : QDir(QCoreApplication::applicationDirPath()).filePath(path);
}

QString findFileInDir(const QString &dir, const QString &name)
{
    for (const QString &suffix : iconSuffixes()) {
        const QString path = QDir(dir).filePath(name + QLatin1Char('.') + suffix);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

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

void drawArrowHead(QPainter *painter, const QPointF &tip, const QPointF &direction)
{
    const QPointF unit = direction / std::hypot(direction.x(), direction.y());
    const QPointF normal(-unit.y(), unit.x());
    const QPointF base = tip - unit * 10.0;
    QPolygonF head;
    head << tip << (base + normal * 5.5) << (base - normal * 5.5);
    painter->drawPolygon(head);
}

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

QString externalIconDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("icons"));
}

QString configPath()
{
    return StoragePaths::dataFile(QStringLiteral("ui-icons.json"));
}

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
