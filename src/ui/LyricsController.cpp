/// @file LyricsController.cpp
/// @brief 歌词加载、缓存服务桥接与当前行高亮定位。
#include "ui/LyricsController.h"

#include "lyrics/LyricsService.h"
#include "media/MediaItem.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcLyricsUi, "mvp.lyrics.ui")

/// @brief 构造控制器并连接 LyricsService 的加载结果。
/// @param http 共享 HTTP 客户端，服务内部不拥有它。
/// @param parent 父对象。
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

/// @brief 切换当前条目并加载对应歌词。
/// @param item 当前播放条目；只有音乐且带 lyricUrl 时才发起加载。
/// @note 远程条目的 file/UNC 歌词会被拒绝：远程源不能借歌词字段读取本地文件。
void LyricsController::setCurrentItem(const MediaItem &item)
{
    clear();
    if (item.type != MediaType::Music || item.lyricUrl.isEmpty())
        return;

    if (item.lyricUrl.isLocalFile()) {
        // 远程条目只能使用 http(s) 歌词；file/UNC 是远程源可控的本地读取入口。
        if (item.online)
            return;
        m_service->loadLocalFile(item.lyricUrl.toLocalFile());
    } else {
        m_service->load(item.lyricUrl, item);
    }
}

/// @brief 根据播放位置更新当前高亮行。
/// @param positionMs 当前播放位置（毫秒）。
/// @note 只在行下标变化时发信号，避免逐帧刷新界面；歌词未加载时只记录位置。
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

/// @brief 清空当前歌词与请求状态。
/// @note 切换条目时必须取消旧请求，否则旧歌词回来会覆盖新条目。
void LyricsController::clear()
{
    // 切到无歌词条目或视频时必须取消旧请求，否则旧歌词回来会覆盖新条目。
    m_service->cancel();
    m_lines.clear();
    m_index = -1;
    m_lastPosition = 0;
    emit cleared();
}

/// @brief 主题/字号变化后重新发出当前行下标，让视觉层重绘高亮。
void LyricsController::refreshHighlight()
{
    emit currentIndexChanged(m_index);
}
