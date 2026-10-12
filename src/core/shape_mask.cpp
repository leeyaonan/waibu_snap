#include "core/shape_mask.h"
#include <QFileInfo>
#include <QPainter>
#include <QPainterPathStroker>
#include <QSet>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
QPainterPath captureShapePath(CaptureShape shape, const QRectF& box)
{
    if (box.isEmpty())
        return {};
    constexpr qreal pi = 3.14159265358979323846;
    const auto map = [&](qreal x, qreal y)
    { return QPointF(box.x() + x * box.width(), box.y() + y * box.height()); };
    const auto mapRect = [&](qreal x, qreal y, qreal w, qreal h)
    { return QRectF(map(x, y), QSizeF(w * box.width(), h * box.height())); };
    QPainterPath path;
    switch (shape)
    {
    case CaptureShape::Ellipse: // 椭圆
        path.addEllipse(box);
        break;
    case CaptureShape::RoundedRect: // 圆角矩形（圆角 = 短边 22%）
    {
        const qreal radius = 0.22 * std::min(box.width(), box.height());
        path.addRoundedRect(box, radius, radius);
        break;
    }
    case CaptureShape::Star: // 五角星（外半径 R，内半径 0.382R）
    {
        const QPointF c = box.center();
        const qreal outer = std::min(box.width(), box.height()) / 2.0;
        const qreal inner = outer * 0.382;
        for (int i = 0; i < 10; ++i)
        {
            const qreal r = (i % 2 == 0) ? outer : inner;
            const qreal a = -pi / 2 + i * pi / 5;
            const QPointF pt = c + QPointF(std::cos(a), std::sin(a)) * r;
            i == 0 ? path.moveTo(pt) : path.lineTo(pt);
        }
        path.closeSubpath();
        break;
    }
    case CaptureShape::Heart: // 心形
        path.moveTo(map(0.50, 0.94));
        path.cubicTo(map(0.04, 0.60), map(0.00, 0.30), map(0.16, 0.15));
        path.cubicTo(map(0.30, 0.02), map(0.46, 0.08), map(0.50, 0.24));
        path.cubicTo(map(0.54, 0.08), map(0.70, 0.02), map(0.84, 0.15));
        path.cubicTo(map(1.00, 0.30), map(0.96, 0.60), map(0.50, 0.94));
        path.closeSubpath();
        break;
    case CaptureShape::Smiley: // 笑脸（面盘 − 双眼 − 嘴）
    {
        path.addEllipse(box);
        QPainterPath holes;
        holes.addEllipse(mapRect(0.27, 0.31, 0.14, 0.14)); // 左眼
        holes.addEllipse(mapRect(0.59, 0.31, 0.14, 0.14)); // 右眼
        QPainterPath arc;
        arc.arcMoveTo(mapRect(0.22, 0.36, 0.56, 0.52), 200);
        arc.arcTo(mapRect(0.22, 0.36, 0.56, 0.52), 200, 140);
        QPainterPathStroker stroker;
        stroker.setWidth(0.055 * std::min(box.width(), box.height()));
        stroker.setCapStyle(Qt::RoundCap);
        holes.addPath(stroker.createStroke(arc));
        path = path.subtracted(holes);
        break;
    }
    case CaptureShape::SpeechBubble: // 对话气泡（圆角主体 + 尾巴）
    {
        const qreal radius = 0.16 * std::min(box.width(), box.height());
        QPainterPath body;
        body.addRoundedRect(mapRect(0.0, 0.0, 1.0, 0.78), radius, radius);
        QPainterPath tail;
        tail.moveTo(map(0.18, 0.72));
        tail.cubicTo(map(0.20, 0.84), map(0.22, 0.92), map(0.26, 0.98));
        tail.cubicTo(map(0.30, 0.88), map(0.36, 0.80), map(0.42, 0.74));
        tail.closeSubpath();
        path = body.united(tail);
        break;
    }
    }
    return path;
}
QString captureShapeLabel(CaptureShape shape)
{
    const QString labels[] = {QStringLiteral("椭圆"),   QStringLiteral("圆角矩形"),
                              QStringLiteral("五角星"), QStringLiteral("心形"),
                              QStringLiteral("笑脸"),   QStringLiteral("对话气泡")};
    return labels[static_cast<int>(shape)];
}
AspectRatio captureShapeNaturalRatio(CaptureShape) { return {1, 1}; }
QImage applyShapeMask(const QImage& image, const QImage& mask, bool whiteBackground)
{
    if (image.isNull() || mask.isNull())
        return {};
    QImage result = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(1);
    QImage layer = mask;
    layer.setDevicePixelRatio(1);
    {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(result.rect(), layer);
    }
    if (whiteBackground)
    {
        QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        painter.fillRect(result.rect(), Qt::white);
    }
    return result;
}
bool hasTransparentPixels(const QImage& image)
{
    if (!image.hasAlphaChannel())
        return false;
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < pixels.height(); ++y)
    {
        const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x)
            if (qAlpha(row[x]) < 255)
                return true;
    }
    return false;
}
QVector<ShapeTemplateEntry> parseShapeTemplates(const QString& text)
{
    QVector<ShapeTemplateEntry> result;
    QSet<QString> seen;
    for (const QString& item : text.split(QLatin1Char(',')))
    {
        const int colon = item.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString file = item.left(colon).trimmed();
        const QString state = item.mid(colon + 1).trimmed();
        // 只接受库内 PNG 文件名，防止路径逃逸；跨平台保守去重。
        if ((state != QStringLiteral("on") && state != QStringLiteral("off")) ||
            file.contains(QLatin1Char('/')) || file.contains(QLatin1Char('\\')) ||
            file.contains(QLatin1Char(':')) ||
            !file.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ||
            QFileInfo(file).completeBaseName().isEmpty() || seen.contains(file.toCaseFolded()))
            continue;
        seen.insert(file.toCaseFolded());
        result.append({file, QFileInfo(file).completeBaseName(), state == QStringLiteral("on")});
    }
    return result;
}
QString serializeShapeTemplates(const QVector<ShapeTemplateEntry>& entries)
{
    QStringList items;
    for (const auto& entry : entries)
        items.append(entry.file + (entry.enabled ? QStringLiteral(":on") : QStringLiteral(":off")));
    QStringList clean;
    for (const auto& entry : parseShapeTemplates(items.join(QLatin1Char(','))))
        clean.append(entry.file + (entry.enabled ? QStringLiteral(":on") : QStringLiteral(":off")));
    return clean.join(QLatin1Char(','));
}
}
