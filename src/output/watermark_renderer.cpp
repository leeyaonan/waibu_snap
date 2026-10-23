#include "output/watermark_renderer.h"
#include <QAbstractTextDocumentLayout>
#include <QColorSpace>
#include <QTextDocument>
#include <QTextOption>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
QFont watermarkFont(const WatermarkAsset& asset, qreal pixelSize)
{
    QFont font;
    if (!asset.fontFamily.isEmpty())
        font.setFamily(asset.fontFamily);
    font.setPixelSize(std::max(1, qRound(pixelSize)));
    font.setBold(asset.bold);
    font.setItalic(asset.italic);
    return font;
}
QSizeF textDocumentSize(const WatermarkAsset& asset, qreal pixelSize)
{
    QTextDocument document;
    document.setUndoRedoEnabled(false);
    document.setDocumentMargin(0);
    document.setDefaultFont(watermarkFont(asset, pixelSize));
    document.setPlainText(asset.text);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    document.setDefaultTextOption(option);
    const QSizeF size = document.size();
    return {std::max(1.0, std::ceil(size.width())), std::max(1.0, std::ceil(size.height()))};
}
void drawTextDocument(QPainter& painter, const WatermarkAsset& asset, qreal pixelSize)
{
    QTextDocument document;
    document.setUndoRedoEnabled(false);
    document.setDocumentMargin(0);
    document.setDefaultFont(watermarkFont(asset, pixelSize));
    document.setPlainText(asset.text);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    document.setDefaultTextOption(option);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, asset.color);
    document.documentLayout()->draw(&painter, context);
}
QImage makeTile(const WatermarkAsset& asset, const QImage& assetImage, QSizeF size,
                qreal textPixelSize)
{
    const int width = std::max(1, qRound(size.width()));
    const int height = std::max(1, qRound(size.height()));
    QImage tile(width, height, QImage::Format_ARGB32_Premultiplied);
    tile.fill(Qt::transparent);
    tile.setDevicePixelRatio(1);
    if (asset.kind == WatermarkKind::Text)
    {
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        drawTextDocument(painter, asset, textPixelSize);
    }
    else
    {
        if (assetImage.isNull())
            return {};
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(tile.rect(), assetImage);
    }
    return tile;
}
void paintTiled(QPainter& painter, QSize outputSize, const WatermarkAsset& asset,
                const QImage& assetImage, const WatermarkInstance& instance, qreal scale)
{
    const QSizeF base = watermarkBaseSize(asset, assetImage);
    if (base.isEmpty())
        return;
    QSizeF tileSize(base.width() * scale, base.height() * scale);
    // 极小字号 / 极端缩放时仍保持可绘制的最小图块，避免无意义的巨量重复。
    tileSize.setWidth(std::max<qreal>(8, tileSize.width()));
    tileSize.setHeight(std::max<qreal>(8, tileSize.height()));
    const QImage tile = makeTile(asset, assetImage, tileSize, asset.fontPixelSize * scale);
    if (tile.isNull())
        return;
    const qreal tileWidth = tile.width();
    const qreal tileHeight = tile.height();
    const qreal gap = std::max<qreal>(12, std::min(tileWidth, tileHeight) * 1.5);
    const qreal stepX = tileWidth + gap;
    const qreal stepY = tileHeight + gap;
    const qreal radius = std::hypot(outputSize.width(), outputSize.height()) / 2.0 +
                         std::max(tileWidth, tileHeight) * 2.0;
    const qreal angle = instance.layout == WatermarkLayout::DiagonalForward ? 45.0 : -45.0;
    painter.save();
    painter.translate(outputSize.width() / 2.0, outputSize.height() / 2.0);
    painter.rotate(angle);
    int drawn = 0;
    for (qreal y = -radius; y <= radius && drawn < 20000; y += stepY)
        for (qreal x = -radius; x <= radius && drawn < 20000; x += stepX)
        {
            painter.drawImage(QPointF(x, y), tile);
            ++drawn;
        }
    painter.restore();
}
}
QSizeF watermarkBaseSize(const WatermarkAsset& asset, const QImage& assetImage)
{
    if (asset.kind == WatermarkKind::Text)
        return textDocumentSize(asset, asset.fontPixelSize);
    if (assetImage.isNull())
        return {};
    return QSizeF(assetImage.size());
}
QPointF watermarkInitialCenter(WatermarkLayout layout, QSizeF watermarkSize, QSize outputSize)
{
    if (outputSize.isEmpty() || watermarkSize.isEmpty() ||
        layout == WatermarkLayout::DiagonalForward || layout == WatermarkLayout::DiagonalReverse ||
        layout == WatermarkLayout::Center)
        return {0.5, 0.5};
    const qreal width = outputSize.width();
    const qreal height = outputSize.height();
    const qreal margin = std::max<qreal>(16, std::min(width, height) * 0.04);
    const auto axis = [margin](qreal output, qreal item, bool low, bool high)
    {
        if (item >= output)
            return output / 2.0;
        if (low)
            return margin + item / 2.0;
        if (high)
            return output - margin - item / 2.0;
        return output / 2.0;
    };
    const qreal x =
        axis(width, watermarkSize.width(),
             layout == WatermarkLayout::TopLeft || layout == WatermarkLayout::BottomLeft,
             layout == WatermarkLayout::TopRight || layout == WatermarkLayout::BottomRight);
    const qreal y =
        axis(height, watermarkSize.height(),
             layout == WatermarkLayout::TopLeft || layout == WatermarkLayout::TopRight,
             layout == WatermarkLayout::BottomLeft || layout == WatermarkLayout::BottomRight);
    return {std::clamp(x / width, 0.0, 1.0), std::clamp(y / height, 0.0, 1.0)};
}
void paintWatermark(QPainter& painter, QSize outputSize, const WatermarkAsset& asset,
                    const QImage& assetImage, const WatermarkInstance& instance)
{
    if (!instance.visible || outputSize.isEmpty() || instance.opacity < watermarkMinimumOpacity)
        return;
    const QSizeF base = watermarkBaseSize(asset, assetImage);
    if (base.isEmpty())
        return;
    const qreal scale =
        std::clamp(instance.scale, watermarkMinimumScale, watermarkMaximumScale) / 100.0;
    const int opacity = std::clamp(instance.opacity, 0, 100);
    painter.save();
    painter.setClipRect(QRectF(QPointF(), QSizeF(outputSize)));
    painter.setOpacity(opacity / 100.0);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (instance.layout == WatermarkLayout::DiagonalForward ||
        instance.layout == WatermarkLayout::DiagonalReverse)
    {
        paintTiled(painter, outputSize, asset, assetImage, instance, scale);
        painter.restore();
        return;
    }
    const QPointF center(instance.normalizedCenter.x() * outputSize.width(),
                         instance.normalizedCenter.y() * outputSize.height());
    const QSizeF size(base.width() * scale, base.height() * scale);
    painter.translate(center);
    painter.rotate(instance.rotation);
    if (asset.kind == WatermarkKind::Text)
        drawTextDocument(painter, asset, asset.fontPixelSize * scale);
    else
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(
            QRectF(-size.width() / 2.0, -size.height() / 2.0, size.width(), size.height()),
            assetImage);
    }
    painter.restore();
}
QImage composeWatermark(const QImage& base, const WatermarkAsset& asset, const QImage& assetImage,
                        const WatermarkInstance& instance)
{
    if (base.isNull())
        return {};
    QImage image = base.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setColorSpace(base.colorSpace());
    image.setDevicePixelRatio(1);
    QPainter painter(&image);
    paintWatermark(painter, image.size(), asset, assetImage, instance);
    return image;
}
}
