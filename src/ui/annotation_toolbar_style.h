#pragma once
#include "ui/toolbar_icons.h"
#include <QString>
class QLabel;
class QPushButton;
class QWidget;
namespace waibusnap
{
namespace annotation_toolbar
{
// 截图覆盖层与贴图编辑栏共享的工具栏视觉尺度。
inline constexpr int buttonSize = 28;
inline constexpr int iconSize = 18;
inline constexpr int barHeight = 44;
inline constexpr int gap = 4;
QString pillStyle(const QString& objectName, int radius);
QString flatButtonStyle();
QPushButton* makeButton(QWidget* parent, const QString& objectName, const QString& toolTip,
                        ToolbarIcon icon, bool checkable = false);
QLabel* makeSeparator(QWidget* parent);
}
}
