#include "ui/LyricsController.h"

#include "lyrics/LyricsService.h"
#include "media/MediaItem.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcLyricsUi, "mvp.lyrics.ui")

LyricsController::LyricsController(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_service(new LyricsService(http, this))
{
    connect(m_service, &LyricsService::lyricsReady, this,
            [this](const QVector<LrcLine> &lines) {
                m_lines = lines;
                m_index = -1;

                QStringList texts;
                texts.reserve(lines.size());
                for (const LrcLine &line : lines)
                    texts.append(line.text);
                emit linesLoaded(texts);

                // 歌词可能在播放中途才到达，直接按最近一次播放位置定位当前行
                updatePosition(m_lastPosition);
            });
    connect(m_service, &LyricsService::lyricsFailed, this, [](const QString &message) {
        qCWarning(lcLyricsUi) << "歌词加载失败：" << message;
    });
}

void LyricsController::setCurrentItem(const MediaItem &item)
{
    clear();
    if (item.type != MediaType::Music || item.lyricUrl.isEmpty())
        return;

    if (item.lyricUrl.isLocalFile())
        m_service->loadLocalFile(item.lyricUrl.toLocalFile());
    else
        m_service->load(item.lyricUrl);
}

void LyricsController::updatePosition(qint64 positionMs)
{
    m_lastPosition = positionMs;
    if (m_lines.isEmpty())
        return;

    const int index = LrcParser::indexAt(m_lines, positionMs);
    if (index == m_index)
        return;

    m_index = index;
    emit currentIndexChanged(index);
}

void LyricsController::clear()
{
    // 切到无歌词条目或视频时必须取消旧请求，否则旧歌词回来会覆盖新条目。
    m_service->cancel();
    m_lines.clear();
    m_index = -1;
    m_lastPosition = 0;
    emit cleared();
}

void LyricsController::refreshHighlight()
{
    emit currentIndexChanged(m_index);
}
