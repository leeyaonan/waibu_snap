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
QRect movedPixelSelection(QRect initial, QPointF logicalDelta, qreal scale, QSize pixelSize)
{
    if (scale <= 0 || initial.isEmpty() || !QRect(QPoint(0, 0), pixelSize).contains(initial))
        return {};
    return {std::clamp(initial.x() + qRound(logicalDelta.x() * scale), 0,
                       pixelSize.width() - initial.width()),
            std::clamp(initial.y() + qRound(logicalDelta.y() * scale), 0,
                       pixelSize.height() - initial.height()),
            initial.width(), initial.height()};
}
QRect resizedPixelSelection(QRect initial, SelectionEdges edges, QPointF logicalDelta, qreal scale,
                            QSize pixelSize)
{
    if (scale <= 0 || initial.isEmpty() || !QRect(QPoint(0, 0), pixelSize).contains(initial))
        return {};
    int left = initial.x();
    int top = initial.y();
    int right = left + initial.width();
    int bottom = top + initial.height();
    const int dx = qRound(logicalDelta.x() * scale);
    const int dy = qRound(logicalDelta.y() * scale);
    if (edges.testFlag(SelectionEdge::Left))
        left = std::clamp(left + dx, 0, right - 1);
    else if (edges.testFlag(SelectionEdge::Right))
        right = std::clamp(right + dx, left + 1, pixelSize.width());
    if (edges.testFlag(SelectionEdge::Top))
        top = std::clamp(top + dy, 0, bottom - 1);
    else if (edges.testFlag(SelectionEdge::Bottom))
        bottom = std::clamp(bottom + dy, top + 1, pixelSize.height());
    return {left, top, right - left, bottom - top};
}
}
