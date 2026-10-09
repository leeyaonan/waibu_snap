#pragma once
#include "core/selection_geometry.h"
namespace waibusnap
{
enum class SelectionNudge
{
    Move,
    Expand,
    Shrink
};
// edge 同时表示方向键；所有运算都在半开源屏物理像素矩形中完成。
QRect nudgedPixelSelection(QRect initial, SelectionEdge edge, SelectionNudge mode, QSize pixels);
QPoint selectionNudgeAnchor(QRect selection, SelectionEdge edge, SelectionNudge mode);
inline constexpr int magnifierSampleExtent = 15;
inline constexpr int magnifierCellSize = 8;
// 贴边时平移采样窗；不足 15 像素的小屏才缩小窗口，锚点始终在窗内。
QRect magnifierSamplingRect(QPoint anchor, QSize pixels);
// 逻辑坐标放置，右下优先，越界分别向左 / 上翻转，最后夹取到覆盖层内。
QRect placedMagnifier(QPointF anchor, QSize panel, QRect bounds);
}
