#include "core/selection_assistance.h"
#include <algorithm>
namespace waibusnap
{
QRect nudgedPixelSelection(QRect initial, SelectionEdge edge, SelectionNudge mode, QSize pixels)
{
    QPoint delta;
    switch (edge)
    {
    case SelectionEdge::Left:
        delta = {-1, 0};
        break;
    case SelectionEdge::Top:
        delta = {0, -1};
        break;
    case SelectionEdge::Right:
        delta = {1, 0};
        break;
    case SelectionEdge::Bottom:
        delta = {0, 1};
        break;
    }
    if (mode == SelectionNudge::Move)
        return movedPixelSelection(initial, delta, 1, pixels);
    if (mode == SelectionNudge::Shrink)
        delta = -delta;
    return resizedPixelSelection(initial, edge, delta, 1, pixels);
}
QPoint selectionNudgeAnchor(QRect selection, SelectionEdge edge, SelectionNudge mode)
{
    if (selection.isEmpty())
        return {};
    if (mode == SelectionNudge::Move)
        return selection.topLeft();
    // QRect 的 right / bottom 是最后一个像素，避免半开边界落在源屏外。
    switch (edge)
    {
    case SelectionEdge::Left:
        return {selection.left(), selection.center().y()};
    case SelectionEdge::Top:
        return {selection.center().x(), selection.top()};
    case SelectionEdge::Right:
        return {selection.right(), selection.center().y()};
    case SelectionEdge::Bottom:
        return {selection.center().x(), selection.bottom()};
    }
    return {};
}
QRect magnifierSamplingRect(QPoint anchor, QSize pixels)
{
    if (pixels.isEmpty() || !QRect(QPoint(), pixels).contains(anchor))
        return {};
    const int width = std::min(magnifierSampleExtent, pixels.width());
    const int height = std::min(magnifierSampleExtent, pixels.height());
    return {std::clamp(anchor.x() - width / 2, 0, pixels.width() - width),
            std::clamp(anchor.y() - height / 2, 0, pixels.height() - height), width, height};
}
QRect placedMagnifier(QPointF anchor, QSize panel, QRect bounds, QRect obstacle)
{
    if (panel.isEmpty() || bounds.isEmpty())
        return {};
    int x = qRound(anchor.x()) + 16;
    int y = qRound(anchor.y()) + 18;
    if (x + panel.width() > bounds.x() + bounds.width())
        x = qRound(anchor.x()) - 16 - panel.width();
    if (y + panel.height() > bounds.y() + bounds.height())
        y = qRound(anchor.y()) - 18 - panel.height();
    const QRect result(
        std::clamp(x, bounds.x(),
                   std::max(bounds.x(), bounds.x() + bounds.width() - panel.width())),
        std::clamp(y, bounds.y(),
                   std::max(bounds.y(), bounds.y() + bounds.height() - panel.height())),
        panel.width(), panel.height());
    if (!result.intersects(obstacle))
        return result;
    // 自绘层避开子工具栏；小选区无法在锚点另一侧放下时尝试工具栏上下。
    const QPoint candidates[] = {{result.x(), qRound(anchor.y()) - panel.height() - 18},
                                 {qRound(anchor.x()) - panel.width() - 16, result.y()},
                                 {qRound(anchor.x()) + 16, result.y()},
                                 {result.x(), obstacle.top() - panel.height() - 4},
                                 {result.x(), obstacle.bottom() + 5}};
    for (QPoint position : candidates)
    {
        const QRect alternate(position, panel);
        if (bounds.contains(alternate) && !alternate.contains(anchor.toPoint()) &&
            !alternate.intersects(obstacle))
            return alternate;
    }
    return result;
}
}
