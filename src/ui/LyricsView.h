#ifndef LYRICSVIEW_H
#define LYRICSVIEW_H

#include <QListWidget>

/// 歌词列表：在可视区上下各叠一层渐隐遮罩，消掉歌词"齐刷刷被切断"的观感。
/// 遮罩颜色取自当前主题，主题切换时自动重绘。
class LyricsView : public QListWidget
{
    Q_OBJECT

public:
    explicit LyricsView(QWidget *parent = nullptr);

    void setFadeExtent(int pixels);
    int fadeExtent() const { return m_fade; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_fade = 26;
};

#endif // LYRICSVIEW_H
