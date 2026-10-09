#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include <QAbstractTextDocumentLayout>
#include <QColorSpace>
#include <QPainterPath>
#include <QTextDocument>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
QRectF outwardPixelBounds(QRectF region)
{
    region = region.normalized();
    const qreal left = std::floor(region.left());
    const qreal top = std::floor(region.top());
    return {left, top, std::ceil(region.right()) - left, std::ceil(region.bottom()) - top};
}
}
int mosaicBlockSize(QSizeF pixels)
{
    if (!std::isfinite(pixels.width()) || !std::isfinite(pixels.height()))
        return 4;
    return int(std::clamp(std::round(std::min(pixels.width(), pixels.height()) / 12), qreal(4),
                          qreal(48)));
}
MosaicPatch pixelateRegion(const QImage& base, QRectF region)
{
    if (base.isNull() || !std::isfinite(region.left()) || !std::isfinite(region.top()) ||
        !std::isfinite(region.right()) || !std::isfinite(region.bottom()) || region.width() == 0 ||
        region.height() == 0)
        return {};
    const QRectF bounds = outwardPixelBounds(region);
    const QRect clipped = bounds.intersected(QRectF(base.rect())).toRect();
    if (clipped.isEmpty())
        return {};
    const int block = mosaicBlockSize(bounds.size());
    // 裁剪后仍锚定原区域左上角；边界块只平均基图内的像素，不补透明或黑边。
    const int offsetX = int(std::fmod(clipped.x() - bounds.x(), block));
    const int offsetY = int(std::fmod(clipped.y() - bounds.y(), block));
    const QImage source = base.copy(clipped).convertToFormat(QImage::Format_RGB32);
    QImage patch(source.size(), QImage::Format_RGB32);
    patch.setColorSpace(base.colorSpace());
    for (int top = -offsetY; top < patch.height(); top += block)
        for (int left = -offsetX; left < patch.width(); left += block)
        {
            const QRect cell = QRect(left, top, block, block).intersected(patch.rect());
            quint32 red = 0, green = 0, blue = 0;
            for (int y = cell.top(); y <= cell.bottom(); ++y)
            {
                const auto* row = reinterpret_cast<const QRgb*>(source.constScanLine(y));
                for (int x = cell.left(); x <= cell.right(); ++x)
                {
                    red += qRed(row[x]);
                    green += qGreen(row[x]);
                    blue += qBlue(row[x]);
                }
            }
            // 每个网格单元以面积平均平滑缩为一个像素，再以最近邻扩展为原块。
            const int count = cell.width() * cell.height();
            const QRgb average = qRgb((red + count / 2) / count, (green + count / 2) / count,
                                      (blue + count / 2) / count);
            for (int y = cell.top(); y <= cell.bottom(); ++y)
            {
                auto* row = reinterpret_cast<QRgb*>(patch.scanLine(y));
                std::fill_n(row + cell.left(), cell.width(), average);
            }
        }
    patch.setDevicePixelRatio(1);
    return {clipped, patch};
}
namespace
{
void paintAnnotationWithPatch(QPainter& painter, const Annotation& annotation, const QImage& base,
                              const MosaicPatch* cached)
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
    case AnnotationType::Cover:
    {
        // 物理像素半开边界向外取整，所有触及像素均由不透明纯色替换。
        const QRectF pixels = outwardPixelBounds(QRectF(annotation.first, annotation.last));
        QColor color = annotation.style.color;
        color.setAlpha(255);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setOpacity(1);
        painter.setPen(Qt::NoPen);
        painter.fillRect(pixels, color);
        break;
    }
    case AnnotationType::Mosaic:
    {
        const auto patch =
            cached ? *cached : pixelateRegion(base, QRectF(annotation.first, annotation.last));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setOpacity(1);
        painter.drawImage(patch.pixels.topLeft(), patch.image);
        break;
    }
    }
    painter.restore();
}
}
void paintAnnotation(QPainter& painter, const Annotation& annotation, const QImage& base)
{
    paintAnnotationWithPatch(painter, annotation, base, nullptr);
}
void paintAnnotations(QPainter& painter, const QVector<Annotation>& annotations, const QImage& base,
                      AnnotationRenderCache* cache)
{
    if (cache)
    {
        if (cache->baseKey_ != base.cacheKey())
        {
            cache->baseKey_ = base.cacheKey();
            cache->entries_.clear();
        }
        cache->entries_.resize(annotations.size());
    }
    for (qsizetype index = 0; index < annotations.size(); ++index)
    {
        const Annotation& annotation = annotations[index];
        const MosaicPatch* patch = nullptr;
        if (cache && annotation.type == AnnotationType::Mosaic && annotation.isVisible())
        {
            auto& entry = cache->entries_[index];
            const QRectF region(annotation.first, annotation.last);
            if (entry.region != region)
            {
                entry.region = region;
                entry.patch = pixelateRegion(base, region);
            }
            patch = &entry.patch;
        }
        paintAnnotationWithPatch(painter, annotation, base, patch);
    }
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
    paintAnnotations(painter, annotations, frozen);
    return image;
}
}
