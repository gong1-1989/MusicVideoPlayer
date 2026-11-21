#include "ui/LyricsController.h"

#include "lyrics/LyricsService.h"
#include "media/MediaItem.h"
#include "ui/ThemeManager.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLoggingCategory>
#include <QWidget>

Q_LOGGING_CATEGORY(lcLyricsUi, "mvp.lyrics.ui")

LyricsController::LyricsController(HttpClient *http, const LyricsUi &ui, QObject *parent)
    : QObject(parent)
    , m_ui(ui)
    , m_service(new LyricsService(http, this))
{
    Q_ASSERT(m_ui.panel != nullptr);
    Q_ASSERT(m_ui.list != nullptr);

    connect(m_service, &LyricsService::lyricsReady, this,
            [this](const QVector<LrcLine> &lines) {
                m_lines = lines;
                m_index = -1;
                m_ui.list->clear();
                for (const LrcLine &line : lines) {
                    auto *item = new QListWidgetItem(line.text);
                    item->setTextAlignment(Qt::AlignCenter);
                    m_ui.list->addItem(item);
                }
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
    m_ui.panel->setVisible(item.type == MediaType::Music);
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

    // 拖回第一句之前时 index=-1，也要清掉旧高亮，不能保留上一句的颜色
    if (m_index >= 0 && m_index < m_ui.list->count()) {
        if (auto *previous = m_ui.list->item(m_index))
            previous->setForeground(QBrush());
    }

    m_index = index;
    if (index >= 0)
        applyHighlight();
}

void LyricsController::showPanel()
{
    m_ui.panel->show();
}

void LyricsController::hidePanel()
{
    m_ui.panel->hide();
}

void LyricsController::clear()
{
    // 切到无歌词条目或视频时必须取消旧请求，否则旧歌词回来会覆盖新条目面板。
    m_service->cancel();
    m_lines.clear();
    m_index = -1;
    m_lastPosition = 0;
    m_ui.list->clear();
}

void LyricsController::refreshHighlight()
{
    applyHighlight();
}

void LyricsController::applyHighlight()
{
    if (m_index < 0 || m_index >= m_ui.list->count())
        return;

    auto *current = m_ui.list->item(m_index);
    if (current == nullptr)
        return;

    current->setForeground(QBrush(ThemeManager::instance().current().accentColor));
    m_ui.list->scrollToItem(current, QAbstractItemView::PositionAtCenter);
}
