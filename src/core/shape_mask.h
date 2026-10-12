#pragma once
#include "core/aspect_ratio.h"
#include <QImage>
#include <QPainterPath>
#include <QStringList>
namespace waibusnap
{
enum class CaptureShape
{
    Ellipse,
    RoundedRect,
    Star,
    Heart,
    Smiley,
    SpeechBubble
};
QPainterPath captureShapePath(CaptureShape shape, const QRectF& box);
QString captureShapeLabel(CaptureShape shape);
AspectRatio captureShapeNaturalRatio(CaptureShape shape);
QImage applyShapeMask(const QImage& image, const QImage& mask, bool whiteBackground);
bool hasTransparentPixels(const QImage& image);
struct ShapeTemplateEntry
{
    QString file;
    QString label;
    bool enabled = true;
};
struct ShapeTemplateImport
{
    QString error;
    QVector<ShapeTemplateEntry> imported;
};
QString serializeShapeTemplates(const QVector<ShapeTemplateEntry>& entries);
QVector<ShapeTemplateEntry> parseShapeTemplates(const QString& text);
}
