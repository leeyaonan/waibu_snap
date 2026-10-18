#include "core/annotation.h"
#include <algorithm>
#include <cmath>
namespace waibusnap
{
const std::array<QColor, annotationColorCount>& annotationColors()
{
    static const std::array<QColor, annotationColorCount> colors = {
        QColor(255, 70, 70),   QColor(255, 146, 43), QColor(255, 210, 40),
        QColor(54, 201, 107),  QColor(34, 198, 207), QColor(40, 160, 255),
        QColor(180, 119, 255), QColor(Qt::black),    QColor(Qt::white)};
    return colors;
}
const std::array<QString, annotationColorCount>& annotationColorNames()
{
    static const std::array<QString, annotationColorCount> names = {
        QStringLiteral("红"), QStringLiteral("橙"), QStringLiteral("黄"),
        QStringLiteral("绿"), QStringLiteral("青"), QStringLiteral("蓝"),
        QStringLiteral("紫"), QStringLiteral("黑"), QStringLiteral("白")};
    return names;
}
bool annotationUsesLineWidth(AnnotationType type)
{
    return type == AnnotationType::Rectangle || type == AnnotationType::Ellipse ||
           type == AnnotationType::Line || type == AnnotationType::Arrow ||
           type == AnnotationType::Freehand;
}
int AnnotationWidthWheel::adjust(int width, QPoint angleDelta, QPoint pixelDelta)
{
    const bool angle = !angleDelta.isNull();
    const int unit = angle ? 120 : 15;
    if (unit_ != unit)
        reset();
    unit_ = unit;
    const qint64 total = qint64(remainder_) + (angle ? angleDelta.y() : pixelDelta.y());
    const qint64 steps = total / unit;
    remainder_ = int(total % unit);
    const int next = int(std::clamp(qint64(width) + steps, qint64(annotationMinimumWidth),
                                    qint64(annotationMaximumWidth)));
    if ((next == annotationMaximumWidth && total > 0) ||
        (next == annotationMinimumWidth && total < 0))
        remainder_ = 0;
    return next;
}
bool Annotation::isVisible() const
{
    const auto finite = [](QPointF point)
    { return std::isfinite(point.x()) && std::isfinite(point.y()); };
    if (!finite(first) || !finite(last))
        return false;
    if (type != AnnotationType::Mosaic &&
        (!style.color.isValid() || style.lineWidth <= 0 || style.textSize <= 0))
        return false;
    switch (type)
    {
    case AnnotationType::Rectangle:
    case AnnotationType::Ellipse:
    case AnnotationType::Cover:
    case AnnotationType::Mosaic:
        return first.x() != last.x() && first.y() != last.y();
    case AnnotationType::Line:
    case AnnotationType::Arrow:
        return first != last;
    case AnnotationType::Freehand:
        return !points.isEmpty() && std::all_of(points.cbegin(), points.cend(), finite);
    case AnnotationType::Text:
        return !text.trimmed().isEmpty();
    }
    return false;
}
QFont annotationFont(const AnnotationStyle& style)
{
    QFont font;
    if (!style.fontFamily.isEmpty())
        font.setFamily(style.fontFamily);
    font.setPixelSize(style.textSize);
    return font;
}
QVector<QPointF> arrowHead(const Annotation& annotation)
{
    const QPointF delta = annotation.last - annotation.first;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length <= 0 || !std::isfinite(length) || annotation.style.lineWidth <= 0)
        return {};
    const QPointF unit = delta / length;
    const qreal headLength =
        std::min(length, std::max(qreal(10), qreal(annotation.style.lineWidth) * 4));
    const QPointF base = annotation.last - unit * headLength;
    const QPointF perpendicular(-unit.y(), unit.x());
    const qreal halfWidth = headLength * 0.45;
    return {annotation.last, base + perpendicular * halfWidth, base - perpendicular * halfWidth};
}
void appendFreehandPoint(Annotation& annotation, QPointF point, qreal minimumDistance)
{
    if (annotation.points.isEmpty() ||
        std::hypot(point.x() - annotation.points.last().x(),
                   point.y() - annotation.points.last().y()) >= minimumDistance)
        annotation.points.append(point);
    annotation.last = point;
}
bool AnnotationHistory::add(Annotation annotation)
{
    if (!annotation.isVisible())
        return false;
    annotations_.append(std::move(annotation));
    redo_.clear();
    firstUndoable_ = std::max(firstUndoable_, annotations_.size() - capacity);
    return true;
}
bool AnnotationHistory::undo()
{
    if (!canUndo())
        return false;
    redo_.append(annotations_.takeLast());
    return true;
}
bool AnnotationHistory::redo()
{
    if (!canRedo())
        return false;
    annotations_.append(redo_.takeLast());
    return true;
}
void AnnotationHistory::clear()
{
    annotations_.clear();
    redo_.clear();
    firstUndoable_ = 0;
}
}
