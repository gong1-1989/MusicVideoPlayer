#ifndef VIDEOSTAGE_H
#define VIDEOSTAGE_H

#include <QWidget>

class VideoSurface;

/// 视频舞台容器：给视频面描一条主题色圆角细边，并把圆角半径下发给视频面。
/// 视频本身由 QVideoWidget 原生窗口零拷贝显示，内层直角不再逐帧读回后自绘。
class VideoStage : public QWidget
{
    Q_OBJECT

public:
    explicit VideoStage(QWidget *parent = nullptr);

    void setCornerRadius(qreal radius);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyRadiusToSurface();
    VideoSurface *surface() const;

    qreal m_radius = 16.0;
};

#endif // VIDEOSTAGE_H
