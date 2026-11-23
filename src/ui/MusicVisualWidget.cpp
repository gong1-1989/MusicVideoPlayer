#include "ui/MusicVisualWidget.h"

#include "audio/AudioSpectrumAnalyzer.h"
#include "settings/AppSettings.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QLinearGradient>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QtMath>

#include <utility>

Q_LOGGING_CATEGORY(lcMusicVisual, "mvp.ui.musicvisual")

namespace {

constexpr int kBandCount = 46;

QColor withAlpha(const QColor &color, int alpha)
{
    QColor copy = color;
    copy.setAlpha(qBound(0, alpha, 255));
    return copy;
}

QColor mixColors(const QColor &from, const QColor &to, qreal progress)
{
    const qreal p = qBound(0.0, progress, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * p,
                            from.greenF() + (to.greenF() - from.greenF()) * p,
                            from.blueF() + (to.blueF() - from.blueF()) * p);
}

/// 低频暖粉 -> 橙 -> 紫 -> 蓝 -> 高频薄荷青；比单一主题色更能表现频段层次。
QColor spectrumPaletteColor(qreal progress)
{
    static const QColor stops[] = {
        QColor(255, 94, 158), QColor(255, 150, 92), QColor(173, 110, 255),
        QColor(80, 170, 255), QColor(55, 235, 205)
    };
    const int segmentCount = 4; // stops 有 5 个颜色，形成 4 段插值
    const qreal value = qBound(0.0, progress, 1.0) * segmentCount;
    const int index = qMin(segmentCount - 1, int(value));
    return mixColors(stops[index], stops[index + 1], value - index);
}

QString sourceLabel(const MediaItem &item)
{
    if (item.sourceId == QLatin1String("link"))
        return QObject::tr("直链");
    if (!item.sourceId.isEmpty())
        return item.sourceId;
    return item.online ? QObject::tr("网络") : QObject::tr("本地");
}

QColor accentFromCover(const QPixmap &cover)
{
    if (cover.isNull())
        return QColor();
    const QImage image = cover.toImage().scaled(24, 24, Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
    if (image.isNull())
        return QColor();
    qint64 r = 0, g = 0, b = 0, count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            // 跳过纯黑/纯白和几乎无彩度的像素，否则平均出来总是灰。
            if (pixel.lightness() < 24 || pixel.lightness() > 244)
                continue;
            if (pixel.saturation() < 40)
                continue;
            r += pixel.red();
            g += pixel.green();
            b += pixel.blue();
            ++count;
        }
    }
    if (count == 0)
        return QColor();
    QColor color(int(r / count), int(g / count), int(b / count));
    // 封面主色可能偏暗或偏灰，提到适合做光晕/下划线的饱和度与亮度。
    if (color.saturation() < 110)
        color.setHsl(color.hslHue(), 150, qBound(120, color.lightness(), 205));
    return color;
}

} // namespace

MusicVisualWidget::MusicVisualWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("musicVisual"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_phase += m_playing ? 0.055 : 0.012;
        if (m_phase > 10000.0)
            m_phase = 0.0;
        if (m_audioSpectrumAge < 1000)
            m_audioSpectrumAge += 33;
        update();
    });

    m_spectrum = new AudioSpectrumAnalyzer(this);
    connect(m_spectrum, &AudioSpectrumAnalyzer::levelsChanged,
            this, &MusicVisualWidget::setAudioSpectrum);
    connect(m_spectrum, &AudioSpectrumAnalyzer::unavailable, this,
            [this](const QString &reason) {
                // 播放器音频缓冲不可用时清空真实频谱，绘制层回退到拟真动画。
                m_audioSpectrum.clear();
                m_audioSpectrumAge = 1000;
                qCWarning(lcMusicVisual) << reason;
            });

    if (parent != nullptr) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }
}

MusicVisualWidget::~MusicVisualWidget()
{
    if (m_spectrum != nullptr)
        m_spectrum->stopCapture();
}

void MusicVisualWidget::setTrack(const MediaItem &item)
{
    m_item = item;
    m_sourceDisplayName.clear();
    m_cover = QPixmap();
    m_coverScaled = QPixmap();
    m_coverScaledRadius = 0;
    m_audioSpectrum.clear();
    m_audioSpectrumAge = 1000;
    if (!item.online && item.posterUrl.isLocalFile()) {
        QPixmap loaded(item.posterUrl.toLocalFile());
        if (!loaded.isNull())
            m_cover = loaded;
    }
    const QColor coverAccent = accentFromCover(m_cover);
    m_accent = coverAccent.isValid() ? coverAccent : m_themeAccent;
    update();
}

void MusicVisualWidget::setSourceDisplayName(const QString &name)
{
    if (m_sourceDisplayName == name)
        return;
    m_sourceDisplayName = name;
    update();
}

void MusicVisualWidget::setAccentColor(const QColor &color)
{
    if (!color.isValid() || m_themeAccent == color)
        return;
    m_themeAccent = color;
    // 没有封面主色时（例如网络条目或本地无封面音乐）立即跟随主题。
    if (!accentFromCover(m_cover).isValid())
        m_accent = color;
    update();
}

void MusicVisualWidget::reset()
{
    m_item = MediaItem();
    m_sourceDisplayName.clear();
    m_cover = QPixmap();
    m_coverScaled = QPixmap();
    m_coverScaledRadius = 0;
    m_accent = m_themeAccent;
    m_audioSpectrum.clear();
    m_audioSpectrumAge = 1000;
    m_lyrics.clear();
    m_lyricIndex = -1;
    m_phase = 0.0;
    m_playing = false;
    if (m_timer != nullptr)
        m_timer->stop();
    if (m_spectrum != nullptr)
        m_spectrum->stopCapture();
    update();
}

void MusicVisualWidget::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    // 暂停后画面静止，不需要再用 33ms 定时器全窗口重绘；恢复播放时
    // 只在可见状态下重新启动，隐藏窗口仍由 showEvent 负责。
    if (m_timer != nullptr) {
        if (m_playing && isVisible())
            m_timer->start();
        else
            m_timer->stop();
    }
    if (!m_playing) {
        // 暂停/停止时保留最后一帧真实频谱并定格；只有从未拿到真实数据时
        // 才让绘制层回退拟真动画，避免暂停后反而出现满屏假能量。
        m_audioSpectrumAge = m_audioSpectrum.size() == kBandCount ? 0 : 1000;
    }
    updateCaptureState();
    update();
}

void MusicVisualWidget::setMediaPlayer(QMediaPlayer *player)
{
    if (m_spectrum == nullptr)
        return;
    m_spectrum->attachPlayer(player);
    updateCaptureState();
}

void MusicVisualWidget::setLyrics(const QStringList &lines)
{
    m_lyrics = lines;
    m_lyricIndex = -1;
    update();
}

void MusicVisualWidget::setLyricIndex(int index)
{
    if (m_lyricIndex == index)
        return;
    m_lyricIndex = index;
    update();
}

void MusicVisualWidget::clearLyrics()
{
    if (m_lyrics.isEmpty() && m_lyricIndex < 0)
        return;
    m_lyrics.clear();
    m_lyricIndex = -1;
    update();
}

void MusicVisualWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRectF rect = QRectF(this->rect());
    // 每帧重绘只读一次设置；原先 drawTrackInfo/drawLyrics 各建一个 QSettings。
    const int basePx = qBound(9, AppSettings::uiFontSizePx(), 20);
    drawBackground(painter, rect);
    drawSpectrum(painter, rect);
    drawDisc(painter, rect);
    drawTrackInfo(painter, rect, basePx);
    // 歌词最后绘制，压在频谱之上保证可读
    drawLyrics(painter, rect, basePx);
}

void MusicVisualWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_coverScaled = QPixmap();
    m_coverScaledRadius = 0;
    update();
}

void MusicVisualWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 暂停状态下显示只画一次静态画面，不为动画空转。
    if (m_timer != nullptr && m_playing && !m_timer->isActive())
        m_timer->start();
    updateCaptureState();
}

void MusicVisualWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_timer != nullptr)
        m_timer->stop();
    if (m_spectrum != nullptr)
        m_spectrum->stopCapture();
}

void MusicVisualWidget::updateCaptureState()
{
    if (m_spectrum == nullptr)
        return;
    const bool shouldCapture = m_playing && isVisible();
    if (shouldCapture) {
        if (!m_spectrum->isCapturing())
            m_spectrum->startCapture();
    } else if (m_spectrum->isCapturing()) {
        m_spectrum->stopCapture();
    }
}

void MusicVisualWidget::setAudioSpectrum(const QVector<float> &levels)
{
    if (!m_playing || levels.isEmpty())
        return;
    m_audioSpectrum = levels;
    m_audioSpectrumAge = 0;
    // 正常播放时 33ms 定时器已在重绘，这里不额外 update()，避免双倍刷新。
}

QColor MusicVisualWidget::spectrumColor(int bandIndex) const
{
    const qreal progress =
        bandIndex <= 0 ? 0.0 : qBound(0.0, qreal(bandIndex) / (kBandCount - 1), 1.0);
    return spectrumPaletteColor(progress);
}

bool MusicVisualWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        raise();
    }
    return QWidget::eventFilter(watched, event);
}

void MusicVisualWidget::drawBackground(QPainter &painter, const QRectF &rect)
{
    painter.fillRect(rect, QColor(7, 9, 22));

    // 两团缓慢漂移的光晕，形成“极光在流动”的观感；真实频谱可用时，
    // 低频暖色和高频冷色会跟随音乐变化，避免整块背景只有一种蓝紫。
    const bool live = m_audioSpectrum.size() == kBandCount && m_audioSpectrumAge <= 300;
    const QColor warmColor = live ? spectrumColor(4).lighter(112) : QColor(255, 92, 168);
    const QColor coolColor = live ? spectrumColor(38).lighter(112) : QColor(42, 210, 255);
    const qreal driftX = qSin(m_phase * 0.62) * rect.width() * 0.10;
    const qreal driftY = qCos(m_phase * 0.47) * rect.height() * 0.08;

    QRadialGradient warm(rect.center() + QPointF(-rect.width() * 0.22 + driftX,
                                                 -rect.height() * 0.16 + driftY),
                         qMax(rect.width(), rect.height()) * 0.62);
    warm.setColorAt(0.0, withAlpha(warmColor, 128));
    warm.setColorAt(0.45, withAlpha(warmColor.darker(150), 58));
    warm.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(rect, warm);

    QRadialGradient cool(rect.center() + QPointF(rect.width() * 0.24 + driftY,
                                                 rect.height() * 0.10 + driftX),
                         qMax(rect.width(), rect.height()) * 0.66);
    cool.setColorAt(0.0, withAlpha(coolColor, 112));
    cool.setColorAt(0.48, withAlpha(coolColor.darker(160), 54));
    cool.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(rect, cool);

    // 底部压暗，给频谱和曲目信息留出稳定可读区。
    QLinearGradient floor(rect.topLeft(), rect.bottomLeft());
    floor.setColorAt(0.0, QColor(4, 6, 16, 0));
    floor.setColorAt(0.62, QColor(4, 6, 16, 70));
    floor.setColorAt(1.0, QColor(2, 3, 10, 210));
    painter.fillRect(rect, floor);

    painter.setPen(QPen(withAlpha(QColor(180, 220, 255), 34), 1));
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 14, 14);
}

void MusicVisualWidget::drawSpectrum(QPainter &painter, const QRectF &rect)
{
    const int count = kBandCount;
    const bool live = m_audioSpectrum.size() == count && m_audioSpectrumAge <= 300;
    const qreal totalWidth = rect.width() * 0.86;
    if (totalWidth <= 0 || rect.height() <= 0)
        return;
    // 极窄窗口下固定 gap 会吃掉全部宽度，barWidth 变负导致圆角/光晕反向绘制。
    qreal gap = qMax(2.0, rect.width() / 620.0);
    if (gap * (count - 1) > totalWidth * 0.6)
        gap = totalWidth * 0.4 / qMax(1, count - 1);
    const qreal barWidth = qMax(1.0, (totalWidth - gap * (count - 1)) / count);
    const qreal baseY = rect.height() * 0.88;
    const qreal maxHeight = rect.height() * ((m_playing || live) ? 0.20 : 0.07);

    painter.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        qreal level = 0.0;
        if (live) {
            // 真实频谱：QAudioBufferOutput 给出的播放器自身 PCM，低频鼓点
            // 和高频乐器各自对应。dBFS 映射后再做一次 gamma，让系统音量
            // 偏低时也能看清细节。
            level = qBound(0.0, qreal(m_audioSpectrum.at(i)), 1.0);
            level = qPow(level, 0.72);
        } else {
            // 播放器缓冲不可用/尚未出首帧时的兜底：保留原来的拟真动画。
            const qreal a = qSin(m_phase * 2.1 + i * 0.52) * 0.5 + 0.5;
            const qreal b = qSin(m_phase * 3.7 + i * 1.17) * 0.5 + 0.5;
            const qreal c = qSin(m_phase * 0.9 + i * 0.21) * 0.5 + 0.5;
            level = 0.16 + (a * 0.46 + b * 0.30 + c * 0.24) * 0.84;
        }
        const qreal h = maxHeight * level;
        const QRectF bar(rect.left() + (rect.width() - totalWidth) / 2.0
                             + i * (barWidth + gap),
                         baseY - h, barWidth, h + (live ? 2.0 : 8.0));

        // 每根柱子按频段取色：低频暖粉/橙，中频紫/蓝，高频薄荷青。
        // 能量越高，颜色越亮，保留梦幻感但不让整排柱子同色。
        const QColor color = spectrumColor(i).lighter(100 + qRound(level * 20));
        if (h > 1.0) {
            QColor halo = color;
            halo.setAlpha(live ? 30 + qRound(level * 34) : 20);
            const qreal haloPad = qMax(1.0, barWidth * 0.30);
            painter.setBrush(halo);
            painter.drawRoundedRect(bar.adjusted(-haloPad, -1.5, haloPad, 3.0),
                                    (barWidth + haloPad * 2) / 2.0,
                                    (barWidth + haloPad * 2) / 2.0);
        }

        QLinearGradient barGrad(bar.topLeft(), bar.bottomLeft());
        QColor topColor = color.lighter(140);
        topColor.setAlpha(live ? 238 : 198);
        QColor midColor = color;
        midColor.setAlpha(live ? 216 : 180);
        QColor bottomColor = color.darker(155);
        bottomColor.setAlpha(live ? 112 : 78);
        barGrad.setColorAt(0.0, topColor);
        barGrad.setColorAt(0.52, midColor);
        barGrad.setColorAt(1.0, bottomColor);
        painter.setBrush(barGrad);
        painter.drawRoundedRect(bar, barWidth / 2.0, barWidth / 2.0);

        if (h > 2.5) {
            QColor cap(255, 255, 255, live ? 105 : 70);
            const QRectF capRect(bar.left(), bar.top(), bar.width(),
                                 qMin(qreal(2.2), h * 0.18));
            painter.setBrush(cap);
            painter.drawRoundedRect(capRect, capRect.height() / 2.0,
                                    capRect.height() / 2.0);
        }
    }
}

void MusicVisualWidget::drawDisc(QPainter &painter, const QRectF &rect)
{
    if (rect.width() < 420.0 || rect.height() < 220.0)
        return;

    const qreal radius = qMin(rect.width(), rect.height()) * 0.20;
    const QPointF center(rect.left() + rect.width() * 0.75,
                         rect.top() + rect.height() * 0.42);

    // 光晕跟随当前能量最高的频段取色：低频时偏暖，高频时偏冷，
    // 让整块舞台和频谱一起变化，而不是固定在主题蓝紫色。
    QColor tint = m_accent;
    if (m_audioSpectrum.size() == kBandCount && m_audioSpectrumAge <= 300) {
        int peakBand = 0;
        for (int i = 1; i < m_audioSpectrum.size(); ++i) {
            if (m_audioSpectrum.at(i) > m_audioSpectrum.at(peakBand))
                peakBand = i;
        }
        tint = spectrumColor(peakBand);
    }

    // 外圈呼吸光晕
    QRadialGradient glow(center, radius * 1.65);
    glow.setColorAt(0.0, withAlpha(tint, 105));
    glow.setColorAt(0.45, withAlpha(tint.darker(155), 32));
    glow.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(center, radius * 1.65, radius * 1.65);

    if (!m_cover.isNull()) {
        QPainterPath clip;
        clip.addEllipse(center, radius, radius);
        painter.setClipPath(clip);
        const int pixelRadius = qMax(1, qRound(radius));
        if (m_coverScaled.isNull() || m_coverScaledRadius != pixelRadius) {
            m_coverScaled = m_cover.scaled(pixelRadius * 2, pixelRadius * 2,
                                           Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
            m_coverScaledRadius = pixelRadius;
        }
        const QPixmap &scaled = m_coverScaled;
        painter.drawPixmap(QRectF(center.x() - radius, center.y() - radius,
                                  radius * 2, radius * 2), scaled,
                          QRectF((scaled.width() - radius * 2) / 2.0,
                                 (scaled.height() - radius * 2) / 2.0,
                                 radius * 2, radius * 2));
        painter.setClipping(false);
        painter.setPen(QPen(withAlpha(QColor(220, 240, 255), 140), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius, radius);
        return;
    }

    QRadialGradient disc(center - QPointF(radius * 0.28, radius * 0.30), radius * 1.5);
    disc.setColorAt(0.0, QColor(40, 48, 86));
    disc.setColorAt(0.55, QColor(18, 22, 44));
    disc.setColorAt(1.0, QColor(8, 10, 24));
    painter.setBrush(disc);
    painter.setPen(QPen(withAlpha(QColor(150, 200, 255), 55), 1));
    painter.drawEllipse(center, radius, radius);

    painter.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 4; ++i) {
        const qreal r = radius * (0.22 + i * 0.19);
        painter.setPen(QPen(withAlpha(tint.lighter(130), 18 + i * 6), 1));
        painter.drawEllipse(center, r, r);
    }

    // 中心音符：不依赖图标资源，直接画，主题切换也不会失效。
    const qreal noteSize = radius * 0.58;
    QPainterPath note;
    note.addEllipse(QRectF(center.x() - noteSize * 0.52,
                            center.y() + noteSize * 0.18,
                            noteSize * 0.62, noteSize * 0.46));
    note.addRect(QRectF(center.x() + noteSize * 0.02,
                         center.y() - noteSize * 0.52,
                         noteSize * 0.11, noteSize * 1.10));
    QPainterPath flag;
    flag.moveTo(center.x() + noteSize * 0.13, center.y() - noteSize * 0.52);
    flag.cubicTo(center.x() + noteSize * 0.56, center.y() - noteSize * 0.30,
                 center.x() + noteSize * 0.52, center.y() + noteSize * 0.02,
                 center.x() + noteSize * 0.18, center.y() + noteSize * 0.08);
    note = note.united(flag);
    QLinearGradient noteGradient(center.x() - noteSize, center.y() - noteSize,
                                 center.x() + noteSize, center.y() + noteSize);
    noteGradient.setColorAt(0.0, QColor(210, 240, 255));
    noteGradient.setColorAt(0.55, QColor(120, 190, 255));
    noteGradient.setColorAt(1.0, QColor(255, 120, 200));
    painter.setBrush(noteGradient);
    painter.setPen(Qt::NoPen);
    painter.drawPath(note);
}

void MusicVisualWidget::drawTrackInfo(QPainter &painter, const QRectF &rect, int basePx)
{
    // 删除当前条目后 reset() 会清空 m_item；此时不要继续画“未知曲目/未知歌手”，
    // 否则用户仍会看到一张写着占位文案的动感视图，误以为没有复位。
    if (!m_item.isValid())
        return;

    const qreal left = rect.left() + rect.width() * 0.075;
    const qreal maxWidth = qMax(120.0, rect.width() * (rect.width() < 620 ? 0.84 : 0.52));
    const qreal top = rect.top() + rect.height() * 0.16;
    // 动感视图曾经按窗口高度算字号，窗口越大字越夸张；现在统一以设置里的
    // 真实基础字号为锚点，只给歌名增加一档强调，其余文字与播放列表贴近。

    QFont titleFont = font();
    titleFont.setPixelSize(basePx + 4);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(240, 247, 255));
    const QFontMetrics titleMetrics(titleFont);
    const QString title = titleMetrics.elidedText(
        m_item.title.isEmpty() ? tr("未知曲目") : m_item.title, Qt::ElideRight,
        int(maxWidth));
    painter.drawText(QRectF(left, top, maxWidth, titleMetrics.height() + 4),
                     Qt::AlignLeft | Qt::AlignVCenter, title);

    const qreal subtitleTop = top + titleMetrics.height() + 8;
    QFont subtitleFont = font();
    subtitleFont.setPixelSize(basePx);
    painter.setFont(subtitleFont);
    painter.setPen(withAlpha(QColor(205, 222, 255), 205));
    const QFontMetrics subtitleMetrics(subtitleFont);
    const QString subtitle =
        m_item.artist.isEmpty() ? tr("未知歌手") : m_item.artist;
    painter.drawText(QRectF(left, subtitleTop, maxWidth, subtitleMetrics.height() + 4),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     subtitleMetrics.elidedText(subtitle, Qt::ElideRight, int(maxWidth)));

    // 品质 / 来源小徽标：与搜索结果保持同一套信息口径。
    QStringList tags;
    if (!m_item.quality.isEmpty())
        tags.append(m_item.quality);
    tags.append(m_sourceDisplayName.isEmpty() ? sourceLabel(m_item) : m_sourceDisplayName);
    if (!m_playing)
        tags.append(tr("已暂停"));

    const qreal badgeTop = subtitleTop + subtitleMetrics.height() + 14;
    qreal badgeLeft = left;
    QFont badgeFont = font();
    badgeFont.setPixelSize(qMax(9, basePx - 2));
    painter.setFont(badgeFont);
    const QFontMetrics badgeMetrics(badgeFont);
    for (const QString &tag : std::as_const(tags)) {
        const qreal width = badgeMetrics.horizontalAdvance(tag) + 16.0;
        if (badgeLeft + width > left + maxWidth)
            break;
        QRectF badge(badgeLeft, badgeTop, width, badgeMetrics.height() + 6);
        painter.setPen(QPen(withAlpha(QColor(170, 210, 255), 55), 1));
        painter.setBrush(withAlpha(QColor(80, 120, 255), 34));
        painter.drawRoundedRect(badge, badge.height() / 2.0, badge.height() / 2.0);
        painter.setPen(withAlpha(QColor(225, 240, 255), 220));
        painter.drawText(badge, Qt::AlignCenter, tag);
        badgeLeft += width + 8;
    }
}

void MusicVisualWidget::drawLyrics(QPainter &painter, const QRectF &rect, int basePx)
{
    if (m_lyrics.isEmpty())
        return;

    // 歌词整体水平居中；顶部留给曲目信息、右上留给唱片，底部避开频谱高光。
    const bool compact = rect.width() < 720.0;
    const qreal width = rect.width() * (compact ? 0.86 : 0.68);
    const qreal left = rect.center().x() - width / 2.0;
    const qreal top = rect.top() + rect.height() * 0.40;
    const qreal height = rect.height() * 0.24;
    if (width < 160.0 || height < 70.0)
        return;

    const int visible = 5;
    int anchor = m_lyricIndex >= 0 ? m_lyricIndex : 0;
    int first = anchor - 2;
    first = qBound(0, first, qMax(0, m_lyrics.size() - visible));

    const qreal rowHeight = height / visible;
    const qreal centerY = top + height / 2.0;

    QFont lyricFont = font();
    lyricFont.setPixelSize(basePx);

    for (int row = 0; row < visible; ++row) {
        const int index = first + row;
        if (index >= m_lyrics.size())
            break;

        const QString text = m_lyrics.at(index).trimmed();
        if (text.isEmpty())
            continue;

        const bool current = index == m_lyricIndex;
        const int distance = m_lyricIndex >= 0 ? qAbs(index - m_lyricIndex)
                                              : qAbs(row - 2);

        // 上下边缘渐隐：中间最亮，外侧两行变暗，形成舞台景深。
        const qreal rowCenter = top + (row + 0.5) * rowHeight;
        const qreal edge = qBound(0.0, 1.0 - qAbs(rowCenter - centerY) / (height / 2.0), 1.0);
        const qreal edgeAlpha = 0.30 + 0.70 * edge;
        const int alpha = qBound(40, int((current ? 255 : 118 + 42 / (distance + 1))
                                         * edgeAlpha),
                                 255);

        lyricFont.setPixelSize(current ? basePx + 4 : basePx);
        lyricFont.setBold(current);
        painter.setFont(lyricFont);

        const QRectF rowRect(left, rowCenter - rowHeight / 2.0, width, rowHeight);
        const int textWidth = int(width - 32.0);
        const QFontMetrics lyricMetrics(lyricFont);
        const QString elided = lyricMetrics.elidedText(text, Qt::ElideRight, textWidth);

        if (current) {
            // 当前句：短发光底带 + 居中下划线，配合更大加粗的字号，
            // 让它在整块歌词里明确成为视觉焦点，而不是只靠一条左侧短线。
            const qreal bandWidth =
                qMin(width, qMax<qreal>(120.0, lyricMetrics.horizontalAdvance(elided) + 48.0));
            const QRectF band(rect.center().x() - bandWidth / 2.0,
                              rowCenter - rowHeight / 2.0 + 2.0,
                              bandWidth, rowHeight - 4.0);
            QLinearGradient halo(band.left(), rowCenter, band.right(), rowCenter);
            halo.setColorAt(0.0, QColor(0, 0, 0, 0));
            halo.setColorAt(0.5, withAlpha(m_accent, 52));
            halo.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.setPen(Qt::NoPen);
            painter.setBrush(halo);
            painter.drawRoundedRect(band, 8.0, 8.0);

            const qreal underlineWidth = qBound(36.0, bandWidth * 0.34, 82.0);
            QLinearGradient underline(rect.center().x() - underlineWidth / 2.0,
                                      rowCenter, rect.center().x() + underlineWidth / 2.0,
                                      rowCenter);
            underline.setColorAt(0.0, withAlpha(m_accent, 0));
            underline.setColorAt(0.5, withAlpha(m_accent.lighter(155), 220));
            underline.setColorAt(1.0, withAlpha(m_accent, 0));
            painter.setBrush(underline);
            painter.drawRoundedRect(
                QRectF(rect.center().x() - underlineWidth / 2.0,
                       rowCenter + rowHeight / 2.0 - 3.0, underlineWidth, 2.4),
                1.2, 1.2);
        }

        painter.setPen(QColor(0, 0, 0, qMin(180, alpha)));
        painter.drawText(rowRect.translated(0, 1.5),
                         Qt::AlignHCenter | Qt::AlignVCenter, elided);

        QColor color = current ? QColor(248, 252, 255) : QColor(215, 228, 255);
        color.setAlpha(alpha);
        painter.setPen(color);
        painter.drawText(rowRect, Qt::AlignHCenter | Qt::AlignVCenter, elided);
    }
}
