/// @file ListMetrics.cpp
/// @brief 列表分页的纯计算实现，不依赖 QWidget 或具体视图实例。
#include "ui/ListMetrics.h"

#include <QtGlobal>

namespace ListMetrics {

/// @brief 按视口高度计算一页可完整显示的行数。
/// @param viewportHeight 列表视口的可用高度（像素）。
/// @param rowHeight 单行高度（像素），必须大于 0。
/// @param spacing 相邻行之间的额外间距（像素），负数按 0 处理。
/// @return 至少 1 行，最多 200 行；输入非法时返回 1，避免除零和异常分页。
/// @note 上限 200 行用于防止异常窗口尺寸导致一次向模型请求过多数据。
int pageSizeForViewport(int viewportHeight, int rowHeight, int spacing)
{
    if (viewportHeight <= 0 || rowHeight <= 0)
        return 1;

    const int occupied = rowHeight + qMax(0, spacing);
    if (occupied <= 0)
        return 1;
    return qBound(1, viewportHeight / occupied, 200);
}

} // namespace ListMetrics
