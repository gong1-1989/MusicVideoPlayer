#ifndef LISTMETRICS_H
#define LISTMETRICS_H

/// 列表分页与窗口可见行数之间的纯计算，便于脱离 QWidget 做回归测试。
namespace ListMetrics {

/// 按列表当前视口高度计算一页能完整显示的行数。
/// rowHeight <= 0 时回落到 1 行；上限 200 行，避免异常高度造成一次拷贝过多条目。
int pageSizeForViewport(int viewportHeight, int rowHeight, int spacing = 0);

} // namespace ListMetrics

#endif // LISTMETRICS_H
