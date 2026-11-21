#pragma once

#include <QSlider>

/// 播放进度条：点击凹槽直接跳到点击位置。
/// QSlider 默认点击凹槽只前进一个 pageStep（默认 10），对毫秒级进度条
/// 等于没有反应，因此这里把点击坐标换算成绝对时间值。
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    int valueForGrooveClick(const QPoint &pos) const;
};
