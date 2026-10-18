#include "ui/annotation_color_palette.h"
#include "core/annotation.h"
#include "ui/toolbar_icons.h"
#include <QPushButton>
namespace waibusnap
{
AnnotationColorPalette::AnnotationColorPalette(QWidget* parent, const QString& prefix,
                                               std::function<void(int)> choose)
    : QWidget(parent)
{
    setObjectName(prefix + QStringLiteral("Palette"));
    setFixedSize(164, 68);
    setStyleSheet(
        QStringLiteral("QPushButton { background: transparent; border: 1px solid transparent; "
                       "border-radius: 6px; }"
                       "QPushButton:hover { background: #3a3a3a; }"
                       "QPushButton:checked { background: #705030; border: 1px solid #ffc078; }"));
    for (int index = 0; index < int(annotationColors().size()); ++index)
    {
        auto* button = new QPushButton(this);
        button->setObjectName(prefix + QString::number(index) + QStringLiteral("Button"));
        button->setIcon(toolbarIcon(ToolbarIcon::Color, annotationColors()[index]));
        button->setIconSize(QSize(18, 18));
        button->setFixedSize(28, 28);
        button->move(4 + index % 5 * 32, 4 + index / 5 * 32);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        buttons_.append(button);
        connect(button, &QPushButton::clicked, this, [choose, index] { choose(index); });
    }
    setColor(annotationColors().front());
}
void AnnotationColorPalette::setColor(const QColor& color)
{
    for (int index = 0; index < buttons_.size(); ++index)
    {
        auto* button = buttons_[index];
        const bool selected = color == annotationColors()[index];
        button->setChecked(selected);
        const QString tip = QStringLiteral("标注颜色：%1%2")
                                .arg(annotationColorNames()[index],
                                     selected ? QStringLiteral("（已选）") : QString());
        button->setToolTip(tip);
        button->setAccessibleName(tip);
    }
}
}
