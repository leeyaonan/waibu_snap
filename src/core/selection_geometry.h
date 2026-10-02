#pragma once
#include <QFlags>
#include <QPointF>
#include <QRect>
#include <QSize>
namespace waibusnap
{
// 返回半开源像素矩形；两个端点采用相同的最近边缘取整，夹取在起始屏内。
QRect normalizedPixelSelection(QPointF first, QPointF second, qreal scale, QSize pixelSize);
enum class SelectionEdge
{
    Left = 1,
    Top = 2,
    Right = 4,
    Bottom = 8
};
Q_DECLARE_FLAGS(SelectionEdges, SelectionEdge)
Q_DECLARE_OPERATORS_FOR_FLAGS(SelectionEdges)
// 拖动从按下时的矩形计算，避免累计取整误差；移动保持尺寸不变。
QRect movedPixelSelection(QRect initial, QPointF logicalDelta, qreal scale, QSize pixelSize);
// 边角越过对侧时夹停在对侧前 1 个物理像素，不翻转手柄；对侧边缘保持固定。
QRect resizedPixelSelection(QRect initial, SelectionEdges edges, QPointF logicalDelta, qreal scale,
                            QSize pixelSize);
}
