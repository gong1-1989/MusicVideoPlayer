#include "ui/ListMetrics.h"

#include <QtGlobal>

namespace ListMetrics {

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
