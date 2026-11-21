#include "ui/SeekSlider.h"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

SeekSlider::SeekSlider(QWidget *parent)
    : QSlider(parent)
{
}

int SeekSlider::valueForGrooveClick(const QPoint &pos) const
{
    QStyleOptionSlider option;
    initStyleOption(&option);

    const QRect groove = style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

    const int handleLength = handle.width();
    const int span = qMax(1, groove.width() - handleLength);
    const int offset = qBound(0, pos.x() - groove.x() - handleLength / 2, span);

    int value = QStyle::sliderValueFromPosition(
        minimum(), maximum(), offset, span, option.upsideDown);
    if (invertedAppearance())
        value = maximum() + minimum() - value;
    return value;
}

void SeekSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || orientation() != Qt::Horizontal
        || maximum() <= minimum()) {
        QSlider::mousePressEvent(event);
        return;
    }

    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const QPoint pos = event->position().toPoint();

    // 点在滑块手柄上仍交给基类处理，保留正常的拖动行为。
    if (!groove.contains(pos) || handle.contains(pos)) {
        QSlider::mousePressEvent(event);
        return;
    }

    // 按拖动同样的信号顺序走：sliderPressed -> valueChanged -> sliderReleased，
    // PlaybackUiController 现有的 sliderReleased 定位逻辑无需改动。
    setSliderDown(true);
    setValue(valueForGrooveClick(pos));
    setSliderDown(false);
    event->accept();
}
