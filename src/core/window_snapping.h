#pragma once
#include <QPointF>
#include <QRect>
#include <QVector>
namespace waibusnap
{
QVector<QRect> localWindowRects(const QVector<QRect>& windows, QRect displayGeometry);
int windowAt(const QVector<QRect>& windows, QPointF position);
QRect windowPixelSelection(QRect logicalWindow, qreal scale, QSize pixelSize);
}
