#include "core/annotation.h"
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTransform>
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
int AnnotationWidthWheel::adjust(int value, QPoint angleDelta, QPoint pixelDelta, int minimum,
                                 int maximum)
{
    const bool angle = !angleDelta.isNull();
    const int unit = angle ? 120 : 15;
    if (unit_ != unit)
        reset();
    unit_ = unit;
    const qint64 total = qint64(remainder_) + (angle ? angleDelta.y() : pixelDelta.y());
    const qint64 steps = total / unit;
    remainder_ = int(total % unit);
    const int next = int(std::clamp(qint64(value) + steps, qint64(minimum), qint64(maximum)));
    if ((next == maximum && total > 0) || (next == minimum && total < 0))
        remainder_ = 0;
    return next;
}
bool Annotation::isVisible() const
{
    const auto finite = [](QPointF point)
    { return std::isfinite(point.x()) && std::isfinite(point.y()); };
    if (!finite(first) || !finite(last) || !std::isfinite(rotation))
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
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    return font;
}
namespace
{
QPointF rotateAround(QPointF point, QPointF center, qreal degrees)
{
    if (qFuzzyIsNull(degrees))
        return point;
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(degrees);
    transform.translate(-center.x(), -center.y());
    return transform.map(point);
}
qreal pointSegmentDistance(QPointF point, QPointF first, QPointF last)
{
    const QPointF segment = last - first;
    const qreal lengthSquared = segment.x() * segment.x() + segment.y() * segment.y();
    if (lengthSquared <= 1e-12)
        return std::hypot(point.x() - first.x(), point.y() - first.y());
    const QPointF offset = point - first;
    const qreal ratio = std::clamp(
        (offset.x() * segment.x() + offset.y() * segment.y()) / lengthSquared, qreal(0), qreal(1));
    const QPointF projection = first + segment * ratio;
    return std::hypot(point.x() - projection.x(), point.y() - projection.y());
}
QRectF textBounds(const Annotation& annotation)
{
    QTextDocument document;
    document.setUndoRedoEnabled(false);
    document.setDocumentMargin(0);
    QFont baseFont = annotationFont(annotation.style);
    baseFont.setUnderline(false);
    document.setDefaultFont(baseFont);
    document.setPlainText(annotation.text);
    QTextCharFormat format;
    format.setFont(baseFont);
    format.setFontWeight(annotation.style.bold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(annotation.style.italic);
    format.setFontUnderline(annotation.style.underline);
    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    cursor.mergeCharFormat(format);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    document.setDefaultTextOption(option);
    return {annotation.first, document.size()};
}
}
QRectF annotationBaseBounds(const Annotation& annotation)
{
    QRectF bounds;
    switch (annotation.type)
    {
    case AnnotationType::Rectangle:
    case AnnotationType::Ellipse:
        bounds = QRectF(annotation.first, annotation.last).normalized();
        if (annotation.style.lineWidth > 0)
            bounds.adjust(-annotation.style.lineWidth / 2.0, -annotation.style.lineWidth / 2.0,
                          annotation.style.lineWidth / 2.0, annotation.style.lineWidth / 2.0);
        break;
    case AnnotationType::Line:
    case AnnotationType::Arrow:
    {
        bounds = QRectF(annotation.first, annotation.last).normalized();
        if (annotation.style.lineWidth > 0)
            bounds.adjust(-annotation.style.lineWidth / 2.0, -annotation.style.lineWidth / 2.0,
                          annotation.style.lineWidth / 2.0, annotation.style.lineWidth / 2.0);
        if (annotation.type == AnnotationType::Arrow)
            for (const QPointF point : arrowHead(annotation))
                bounds = bounds.united(QRectF(point, QSizeF(0, 0)));
        break;
    }
    case AnnotationType::Freehand:
        if (annotation.points.isEmpty())
            bounds = QRectF(annotation.first, annotation.first);
        else
        {
            qreal left = annotation.points.first().x();
            qreal top = annotation.points.first().y();
            qreal right = left;
            qreal bottom = top;
            for (const QPointF point : annotation.points)
            {
                left = std::min(left, point.x());
                top = std::min(top, point.y());
                right = std::max(right, point.x());
                bottom = std::max(bottom, point.y());
            }
            bounds = QRectF(QPointF(left, top), QPointF(right, bottom));
        }
        if (annotation.style.lineWidth > 0)
            bounds.adjust(-annotation.style.lineWidth / 2.0, -annotation.style.lineWidth / 2.0,
                          annotation.style.lineWidth / 2.0, annotation.style.lineWidth / 2.0);
        break;
    case AnnotationType::Text:
        bounds = textBounds(annotation);
        break;
    case AnnotationType::Cover:
    case AnnotationType::Mosaic:
        bounds = QRectF(annotation.first, annotation.last).normalized();
        break;
    }
    return bounds;
}
QPointF annotationCenter(const Annotation& annotation)
{
    return annotationBaseBounds(annotation).center();
}
QPointF annotationFromBase(const Annotation& annotation, QPointF point)
{
    return rotateAround(point, annotationCenter(annotation), annotation.rotation);
}
QPointF annotationToBase(const Annotation& annotation, QPointF point)
{
    return rotateAround(point, annotationCenter(annotation), -annotation.rotation);
}
QPolygonF annotationCorners(const Annotation& annotation)
{
    const QRectF bounds = annotationBaseBounds(annotation);
    QPolygonF corners;
    corners << annotationFromBase(annotation, bounds.topLeft())
            << annotationFromBase(annotation, bounds.topRight())
            << annotationFromBase(annotation, bounds.bottomRight())
            << annotationFromBase(annotation, bounds.bottomLeft());
    return corners;
}
QRectF annotationBounds(const Annotation& annotation)
{
    if (qFuzzyIsNull(annotation.rotation))
        return annotationBaseBounds(annotation);
    return annotationCorners(annotation).boundingRect();
}
QPointF annotationRotationHandle(const Annotation& annotation, qreal offset)
{
    const QRectF bounds = annotationBaseBounds(annotation);
    const QPointF topCenter(bounds.center().x(), bounds.top());
    return annotationFromBase(annotation, topCenter + QPointF(0, -offset));
}
void translateAnnotation(Annotation& annotation, QPointF delta)
{
    annotation.first += delta;
    annotation.last += delta;
    for (QPointF& point : annotation.points)
        point += delta;
}
qreal normalizedRotation(qreal degrees)
{
    if (!std::isfinite(degrees))
        return 0;
    degrees = std::fmod(degrees, 360.0);
    if (degrees >= 180.0)
        degrees -= 360.0;
    if (degrees < -180.0)
        degrees += 360.0;
    return qFuzzyIsNull(degrees) ? 0 : degrees;
}
qreal snapAnnotationRotation(qreal degrees, qreal step)
{
    if (!std::isfinite(degrees) || !std::isfinite(step) || step <= 0)
        return normalizedRotation(degrees);
    return normalizedRotation(std::round(degrees / step) * step);
}
void setAnnotationRotation(Annotation& annotation, qreal degrees)
{
    annotation.rotation = normalizedRotation(degrees);
}
bool annotationHitTest(const Annotation& annotation, QPointF point, qreal tolerance)
{
    if (!annotation.isVisible())
        return false;
    const QPointF local = annotationToBase(annotation, point);
    const QRectF bounds = annotationBaseBounds(annotation);
    if (!bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(local))
        return false;
    switch (annotation.type)
    {
    case AnnotationType::Rectangle:
    case AnnotationType::Cover:
    case AnnotationType::Mosaic:
    case AnnotationType::Text:
        return true;
    case AnnotationType::Ellipse:
    {
        const qreal radiusX = bounds.width() / 2.0;
        const qreal radiusY = bounds.height() / 2.0;
        if (radiusX <= 0 || radiusY <= 0)
            return false;
        const qreal dx = (local.x() - bounds.center().x()) / (radiusX + tolerance);
        const qreal dy = (local.y() - bounds.center().y()) / (radiusY + tolerance);
        return dx * dx + dy * dy <= 1.0;
    }
    case AnnotationType::Line:
    case AnnotationType::Arrow:
        if (pointSegmentDistance(local, annotation.first, annotation.last) <=
            annotation.style.lineWidth / 2.0 + tolerance)
            return true;
        if (annotation.type == AnnotationType::Arrow)
        {
            const QPolygonF head = arrowHead(annotation);
            if (head.size() == 3 && head.containsPoint(local, Qt::OddEvenFill))
                return true;
        }
        return false;
    case AnnotationType::Freehand:
        if (annotation.points.size() == 1)
            return std::hypot(local.x() - annotation.points.first().x(),
                              local.y() - annotation.points.first().y()) <=
                   annotation.style.lineWidth / 2.0 + tolerance;
        for (qsizetype index = 1; index < annotation.points.size(); ++index)
            if (pointSegmentDistance(local, annotation.points[index - 1],
                                     annotation.points[index]) <=
                annotation.style.lineWidth / 2.0 + tolerance)
                return true;
        return false;
    }
    return false;
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
AnnotationHistory::AnnotationHistory() { states_.append(QVector<Annotation>{}); }
const QVector<Annotation>& AnnotationHistory::annotations() const { return states_.at(current_); }
void AnnotationHistory::truncateRedo()
{
    if (states_.size() > current_ + 1)
        states_.resize(current_ + 1);
}
void AnnotationHistory::trimCapacity()
{
    while (states_.size() > capacity + 1)
    {
        states_.removeFirst();
        --current_;
    }
    if (current_ < 0)
        current_ = 0;
}
void AnnotationHistory::appendState(QVector<Annotation> state)
{
    states_.append(std::move(state));
    current_ = states_.size() - 1;
    trimCapacity();
}
bool AnnotationHistory::add(Annotation annotation)
{
    if (editing_ || !annotation.isVisible())
        return false;
    truncateRedo();
    QVector<Annotation> next = states_.at(current_);
    next.append(std::move(annotation));
    appendState(std::move(next));
    return true;
}
bool AnnotationHistory::setLastLineWidth(int width)
{
    if (editing_ || states_.at(current_).isEmpty())
        return false;
    Annotation& last = states_[current_].last();
    if (!annotationUsesLineWidth(last.type))
        return false;
    width = std::clamp(width, annotationMinimumWidth, annotationMaximumWidth);
    if (last.style.lineWidth == width)
        return false;
    last.style.lineWidth = width;
    truncateRedo();
    return true;
}
bool AnnotationHistory::setLastTextStyle(int textSize, bool bold, bool italic, bool underline)
{
    if (editing_ || states_.at(current_).isEmpty() ||
        states_.at(current_).last().type != AnnotationType::Text)
        return false;
    Annotation& last = states_[current_].last();
    textSize = std::clamp(textSize, annotationMinimumTextSize, annotationMaximumTextSize);
    if (last.style.textSize == textSize && last.style.bold == bold && last.style.italic == italic &&
        last.style.underline == underline)
        return false;
    last.style.textSize = textSize;
    last.style.bold = bold;
    last.style.italic = italic;
    last.style.underline = underline;
    truncateRedo();
    return true;
}
bool AnnotationHistory::beginEdit()
{
    if (editing_)
        return false;
    editBefore_ = states_.at(current_);
    editChanged_ = false;
    editing_ = true;
    return true;
}
bool AnnotationHistory::updateAnnotation(qsizetype index, Annotation annotation)
{
    if (!editing_ || index < 0 || index >= states_.at(current_).size())
        return false;
    states_[current_][index] = std::move(annotation);
    editChanged_ = true;
    return true;
}
bool AnnotationHistory::commitEdit()
{
    if (!editing_)
        return false;
    const bool changed = editChanged_;
    if (!changed)
    {
        editing_ = false;
        editBefore_.clear();
        return false;
    }
    QVector<Annotation> after = states_.at(current_);
    truncateRedo();
    states_[current_] = editBefore_;
    states_.append(std::move(after));
    current_ = states_.size() - 1;
    trimCapacity();
    editing_ = false;
    editChanged_ = false;
    editBefore_.clear();
    return true;
}
void AnnotationHistory::cancelEdit()
{
    if (!editing_)
        return;
    states_[current_] = editBefore_;
    editing_ = false;
    editChanged_ = false;
    editBefore_.clear();
}
bool AnnotationHistory::undo()
{
    if (!canUndo())
        return false;
    --current_;
    return true;
}
bool AnnotationHistory::redo()
{
    if (!canRedo())
        return false;
    ++current_;
    return true;
}
void AnnotationHistory::clear()
{
    states_.clear();
    states_.append(QVector<Annotation>{});
    current_ = 0;
    editing_ = false;
    editChanged_ = false;
    editBefore_.clear();
}
}
