#include "core/window_snapping.h"
#include "core/selection_geometry.h"
namespace waibusnap
{
QVector<QRect> localWindowRects(const QVector<QRect>& windows, QRect displayGeometry)
{
    QVector<QRect> result;
    if (displayGeometry.isEmpty())
        return result;
    for (const QRect& window : windows)
    {
        const QRect clipped = window.intersected(displayGeometry);
        if (!clipped.isEmpty())
            result.append(clipped.translated(-displayGeometry.topLeft()));
    }
    return result;
}
int windowAt(const QVector<QRect>& windows, QPointF position)
{
    for (qsizetype i = 0; i < windows.size(); ++i)
    {
        const QRect& window = windows[i];
        // QRect::right() 是最后一个整数像素；命中按半开逻辑区间判断。
        if (!window.isEmpty() && position.x() >= window.x() && position.y() >= window.y() &&
            position.x() < window.x() + window.width() &&
            position.y() < window.y() + window.height())
            return int(i);
    }
    return -1;
}
QRect windowPixelSelection(QRect logicalWindow, qreal scale, QSize pixelSize)
{
    if (logicalWindow.isEmpty())
        return {};
    return normalizedPixelSelection(logicalWindow.topLeft(),
                                    QPointF(logicalWindow.x() + logicalWindow.width(),
                                            logicalWindow.y() + logicalWindow.height()),
                                    scale, pixelSize);
}
}
