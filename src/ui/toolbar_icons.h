#pragma once
#include "core/annotation.h"
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
    ExtractAllText,
    Ratio,
    Shape,
    LongImage
};
// 两编辑面共享一级工具槽位、默认子工具和元数据；会话内选择由各窗口持有。
inline constexpr std::array<AnnotationType, 5> defaultAnnotationTools = {
    AnnotationType::Rectangle, AnnotationType::Arrow, AnnotationType::Freehand,
    AnnotationType::Text, AnnotationType::Mosaic};
int annotationToolSlot(AnnotationType type);
QString annotationToolName(AnnotationType type);
QString annotationToolObject(AnnotationType type);
QString annotationToolTip(AnnotationType type);
QString annotationGroupObject(int slot);
// 参数图标分别接受色点颜色、样线粗细或 A 路径缩放；包含 1x / 2x 与禁用态。
QIcon toolbarIcon(ToolbarIcon icon, QColor color = {}, qreal value = 1);
}
