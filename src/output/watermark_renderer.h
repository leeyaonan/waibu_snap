#pragma once
#include "core/watermark.h"
#include <QImage>
#include <QPainter>
#include <QSizeF>
namespace waibusnap
{
// 100% 缩放时的水印外接尺寸；图片使用传入的规范化资产图。
QSizeF watermarkBaseSize(const WatermarkAsset& asset, const QImage& assetImage);
// 单点排布的初始归一化中心；斜铺返回 (0.5, 0.5)，调用方不使用。
QPointF watermarkInitialCenter(WatermarkLayout layout, QSizeF watermarkSize, QSize outputSize);
void paintWatermark(QPainter& painter, QSize outputSize, const WatermarkAsset& asset,
                    const QImage& assetImage, const WatermarkInstance& instance);
// 在 base 副本上合成水印；base 为空或水印不可见时返回原图副本。
QImage composeWatermark(const QImage& base, const WatermarkAsset& asset, const QImage& assetImage,
                        const WatermarkInstance& instance);
}
