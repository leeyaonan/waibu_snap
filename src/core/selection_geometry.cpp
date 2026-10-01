#include "core/selection_geometry.h"
#include <algorithm>
namespace waibusnap
{
QRect normalizedPixelSelection(QPointF first, QPointF second, qreal scale, QSize pixelSize)
{
    if (scale <= 0 || pixelSize.isEmpty())
        return {};
    const int ax = std::clamp(qRound(first.x() * scale), 0, pixelSize.width());
    const int ay = std::clamp(qRound(first.y() * scale), 0, pixelSize.height());
    const int bx = std::clamp(qRound(second.x() * scale), 0, pixelSize.width());
    const int by = std::clamp(qRound(second.y() * scale), 0, pixelSize.height());
    return {std::min(ax, bx), std::min(ay, by), std::abs(bx - ax), std::abs(by - ay)};
}
}
