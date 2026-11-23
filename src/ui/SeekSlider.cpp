/// @file SeekSlider.cpp
/// @brief 支持点击凹槽直接定位的播放进度条实现。
#include "ui/SeekSlider.h"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

/// @brief 构造进度条；所有行为由默认 QSlider 样式和尺寸决定。
SeekSlider::SeekSlider(QWidget *parent)
    : QSlider(parent)
{
}

/// @brief 把凹槽内的点击坐标换算成滑块数值。
/// @param pos 相对 SeekSlider 的点击坐标。
/// @return 点击位置对应的绝对值；考虑手柄宽度、上下/反向显示。
/// @note 用 QStyle 计算 groove/handle 几何，兼容不同主题和 DPI；
///       span 至少为 1，避免除零。
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

/// @brief 鼠标左键点击凹槽时直接跳到对应位置。
/// @note 点击手柄仍交给基类，保留拖动行为；跳转时按
///       sliderPressed → valueChanged → sliderReleased 的顺序发信号，
///       让 PlaybackUiController 复用现有的“释放后 seek”逻辑。
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
