#pragma once
#include <QColor>
#include <QIcon>
namespace waibusnap
{
enum class ToolbarIcon
{
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Freehand,
    Text,
    Cover,
    Mosaic,
    Ocr,
    Color,
    LineWidth,
    TextSize,
    Undo,
    Redo,
    Copy,
    Save,
    Pin,
    Cancel,
    ExtractAllText
};
// 参数图标分别接受色点颜色、样线粗细或 A 路径缩放；包含 1x / 2x 与禁用态。
QIcon toolbarIcon(ToolbarIcon icon, QColor color = {}, qreal value = 1);
}
