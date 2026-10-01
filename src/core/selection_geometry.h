#pragma once
#include <QPointF>
#include <QRect>
#include <QSize>
namespace waibusnap
{
// 返回半开源像素矩形；两个端点采用相同的最近边缘取整，夹取在起始屏内。
QRect normalizedPixelSelection(QPointF first, QPointF second, qreal scale, QSize pixelSize);
}
