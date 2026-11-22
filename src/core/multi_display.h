#pragma once
#include "interfaces/display_topology.h"
#include <QDateTime>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
namespace waibusnap
{
// 拼接画布硬上限：任一边与总像素；超限整组放弃，不静默降采样。
inline constexpr int multiDisplayMaximumCanvasSide = 16384;
inline constexpr qint64 multiDisplayMaximumCanvasArea = 268435456;
// 前缀顺延上限：同毫秒冲突时逐毫秒后移，用尽即失败。
inline constexpr int multiDisplayPrefixAttempts = 200;
struct MultiDisplayEntry
{
    DisplayTarget target;
    // 该屏物理像素尺寸（logicalGeometry × 自身 DPR 取整）。
    QSize pixelSize;
};
struct MultiDisplayLayout
{
    // 已按系统排布排序；下标 0 对应 _01（开启拼接时拼接图占 _00）。
    QVector<MultiDisplayEntry> ordered;
    // 全部屏幕逻辑区域的并集包围盒。
    QRect logicalBounds;
    // 组内最大 devicePixelRatio，拼接图统一按该倍率渲染。
    qreal scale = 1.0;
    // 拼接画布物理像素；logicalBounds × scale 整体取整。
    QSize canvasPixels;
    // 与 ordered 一一对应的拼接目标矩形，相对画布左上、单位为物理像素。
    QVector<QRect> placements;
    bool stitchExceedsLimit = false;
    // 超限时可直接展示的文案；未超限为空。
    QString limitReason;
    bool isValid() const { return !ordered.isEmpty() && !canvasPixels.isEmpty(); }
};
// 纯函数：排序、并集包围盒、统一倍率、画布几何与上限；不访问系统。
MultiDisplayLayout buildMultiDisplayLayout(const QVector<DisplayTarget>& displays);
// 单次触发的前缀（不含序号与后缀）：WaibuSnap_yyyy-MM-dd_HH-mm-ss-zzz。
QString multiDisplayBaseName(const QDateTime& timestamp);
// 按序号生成整组文件名；开启拼接时 0 号为拼接图，单屏从 1 号起。
QStringList multiDisplayFileNames(const QString& base, int displayCount, bool stitch,
                                  const QString& extension);
// 逐毫秒顺延直到本组全部目标文件名都不冲突；用尽返回空串。
QString multiDisplayPrefix(const QString& directory, const QDateTime& timestamp, int displayCount,
                           bool stitch, const QString& extension);
}
