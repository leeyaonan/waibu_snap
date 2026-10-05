#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include <QAbstractTextDocumentLayout>
#include <QPainterPath>
#include <QTextDocument>
namespace waibusnap
{
void paintAnnotation(QPainter& painter, const Annotation& annotation)
{
    if (!annotation.isVisible())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QPen(annotation.style.color, annotation.style.lineWidth, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (annotation.type)
    {
    case AnnotationType::Rectangle:
        painter.drawRect(QRectF(annotation.first, annotation.last).normalized());
        break;
    case AnnotationType::Ellipse:
        painter.drawEllipse(QRectF(annotation.first, annotation.last).normalized());
        break;
    case AnnotationType::Line:
        painter.drawLine(annotation.first, annotation.last);
        break;
    case AnnotationType::Arrow:
        painter.drawLine(annotation.first, annotation.last);
        painter.setPen(Qt::NoPen);
        painter.setBrush(annotation.style.color);
        painter.drawPolygon(QPolygonF(arrowHead(annotation)));
        break;
    case AnnotationType::Freehand:
        if (annotation.points.size() == 1)
            painter.drawPoint(annotation.points.first());
        else
        {
            QPainterPath path(annotation.points.first());
            for (qsizetype index = 1; index < annotation.points.size(); ++index)
                path.lineTo(annotation.points[index]);
            painter.drawPath(path);
        }
        break;
    case AnnotationType::Text:
    {
        QTextDocument document;
        document.setUndoRedoEnabled(false);
        document.setDocumentMargin(0);
        document.setDefaultFont(annotationFont(annotation.style));
        document.setPlainText(annotation.text);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        document.setDefaultTextOption(option);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, annotation.style.color);
        painter.translate(annotation.first);
        document.documentLayout()->draw(&painter, context);
        break;
    }
    }
    painter.restore();
}
void paintAnnotations(QPainter& painter, const QVector<Annotation>& annotations)
{
    for (const Annotation& annotation : annotations)
        paintAnnotation(painter, annotation);
}
QImage renderAnnotatedSelection(const QImage& frozen, const QVector<Annotation>& annotations,
                                QRect pixels)
{
    QImage image = cropFrozenSelection(frozen, pixels);
    if (image.isNull() || annotations.isEmpty())
        return image;
    // QPainter 不支持某些源图格式；合成缓冲使用带透明度的标准格式并保留色彩空间。
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.translate(-pixels.x(), -pixels.y());
    paintAnnotations(painter, annotations);
    return image;
}
}
