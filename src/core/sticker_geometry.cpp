#include "core/sticker_geometry.h"
#include <algorithm>
#include <cmath>
namespace waibusnap
{
qreal clampedStickerScale(qreal scale)
{
    return std::isfinite(scale) ? std::clamp(scale, minimumStickerScale, maximumStickerScale) : 1;
}
qreal steppedStickerScale(qreal scale, int steps)
{
    // 25 个百分点一档；使用浮点计算，极端步数也不会造成整数溢出。
    return clampedStickerScale(clampedStickerScale(scale) + qreal(steps) * 0.25);
}
QSizeF stickerLogicalSize(QSize pixels, qreal scale, qreal dpr)
{
    if (pixels.isEmpty() || !std::isfinite(scale) || scale <= 0 || !std::isfinite(dpr) || dpr <= 0)
        return {};
    const QSizeF size(pixels.width() * clampedStickerScale(scale) / dpr,
                      pixels.height() * clampedStickerScale(scale) / dpr);
    // Qt 顶层窗口有整数尺寸上限，拒绝无法表示的几何，不能溢出为负尺寸。
    if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() > 16777215 ||
        size.height() > 16777215)
        return {};
    return size;
}
QSize stickerWindowSize(QSize pixels, qreal scale, qreal dpr)
{
    const QSizeF size = stickerLogicalSize(pixels, scale, dpr);
    if (size.isEmpty())
        return {};
    return {std::max(1, int(std::ceil(size.width()))), std::max(1, int(std::ceil(size.height())))};
}
QPointF anchoredStickerPosition(QPointF position, QSizeF before, QSizeF after, QPointF anchor)
{
    if (before.isEmpty() || after.isEmpty() || !std::isfinite(before.width()) ||
        !std::isfinite(before.height()) || !std::isfinite(after.width()) ||
        !std::isfinite(after.height()) || !std::isfinite(anchor.x()) || !std::isfinite(anchor.y()))
        return position;
    return {anchor.x() - (anchor.x() - position.x()) * after.width() / before.width(),
            anchor.y() - (anchor.y() - position.y()) * after.height() / before.height()};
}
QPoint recoveredStickerPosition(QRect window, const QVector<QRect>& screens, QRect primary)
{
    // 左上角必须可见，确保小图也可操作；大图可保持超出屏幕的原始尺寸。
    for (const QRect& screen : screens)
        if (screen.contains(window.topLeft()))
            return window.topLeft();
    if (primary.isEmpty())
        return window.topLeft();
    const int right = primary.x() + std::max(0, primary.width() - window.width());
    const int bottom = primary.y() + std::max(0, primary.height() - window.height());
    return {std::clamp(window.x(), primary.x(), right),
            std::clamp(window.y(), primary.y(), bottom)};
}
}
