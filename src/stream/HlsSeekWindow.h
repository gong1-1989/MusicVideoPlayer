#ifndef HLSSEEKWINDOW_H
#define HLSSEEKWINDOW_H

#include <QtGlobal>

/// HLS 深拖动预取窗口。
///
/// 必须用播放列表索引计算，不能用分片文件名反推：实测索引 152 对应
/// 文件 0000143.ts，按文件名做 target+2 / target-9 都会整段错位。
/// FFmpeg seek 后会从目标索引开始顺序打开约 10 片，因此窗口是
/// targetIndex..targetIndex+aheadSegments，并裁到播放列表末尾。
namespace HlsSeekWindow {

struct Window
{
    int first = -1;
    int last = -2;

    bool isValid() const { return first >= 0 && last >= first; }
};

inline Window forTarget(int targetIndex, int segmentCount, int aheadSegments)
{
    Window window;
    if (targetIndex < 0 || segmentCount <= 0 || aheadSegments < 0
        || targetIndex >= segmentCount) {
        return window;
    }

    window.first = targetIndex;
    window.last = qMin(segmentCount - 1, targetIndex + aheadSegments);
    return window;
}

} // namespace HlsSeekWindow

#endif // HLSSEEKWINDOW_H
