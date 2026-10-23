#include "ui/annotation_toolbar_style.h"
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QWidget>
namespace waibusnap
{
namespace annotation_toolbar
{
QString pillStyle(const QString& objectName, int radius)
{
    return QStringLiteral("QWidget#%1 { background: #202020; border: 1px solid #3a3a3a; "
                          "border-radius: %2px; }")
               .arg(objectName)
               .arg(radius) +
           flatButtonStyle();
}
QString flatButtonStyle()
{
    return QStringLiteral("QPushButton { background: transparent; color: #f2f2f2; padding: 0; "
                          "border: none; border-radius: 6px; }"
                          "QPushButton:hover { background: #3a3a3a; }"
                          "QPushButton:checked { background: #a65316; }"
                          "QPushButton:disabled { background: transparent; }");
}
QPushButton* makeButton(QWidget* parent, const QString& objectName, const QString& toolTip,
                        ToolbarIcon icon, bool checkable)
{
    auto* button = new QPushButton(parent);
    button->setObjectName(objectName);
    button->setText(QString());
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    button->setIcon(toolbarIcon(icon));
    button->setIconSize(QSize(iconSize, iconSize));
    button->setFixedSize(buttonSize, buttonSize);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCheckable(checkable);
    return button;
}
QLabel* makeSeparator(QWidget* parent)
{
    auto* separator = new QLabel(parent);
    separator->setStyleSheet(QStringLiteral("background: #4a4a4a; border: none;"));
    separator->setFixedSize(1, 22);
    return separator;
}
}
}
