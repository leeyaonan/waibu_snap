#pragma once
#include <QPointF>
#include <QRect>
#include <QSizeF>
#include <QVector>
namespace waibusnap
{
inline constexpr qreal minimumStickerScale = 0.25;
inline constexpr qreal maximumStickerScale = 4;
qreal clampedStickerScale(qreal scale);
qreal steppedStickerScale(qreal scale, int steps);
QSizeF stickerLogicalSize(QSize pixels, qreal scale, qreal dpr);
QSize stickerWindowSize(QSize pixels, qreal scale, qreal dpr);
QPointF anchoredStickerPosition(QPointF position, QSizeF before, QSizeF after, QPointF anchor);
QPoint recoveredStickerPosition(QRect window, const QVector<QRect>& screens, QRect primary);
QPoint clipboardStickerPosition(QRect available, QSize windowSize, quint64 sequence);
}
