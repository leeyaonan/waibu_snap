#pragma once
#include "core/selection_assistance.h"
#include <QString>
#include <QVector>
#include <optional>
namespace waibusnap
{
struct AspectRatio
{
    double width = 1.0;
    double height = 1.0;
};
std::optional<AspectRatio> parseAspectRatio(const QString& text);
QString formatAspectRatio(const AspectRatio& ratio);
bool sameAspectRatio(const AspectRatio& a, const AspectRatio& b);
QVector<AspectRatio> defaultCaptureRatios();
// 全部返回半开物理像素矩形；独立取整两个维度，保底 1 像素并保持屏内。
QRect ratioCreatedSelection(QPointF anchor, QPointF cursor, const AspectRatio& ratio, qreal scale,
                            QSize pixelSize);
QRect ratioResizedSelection(QRect initial, SelectionEdges edges, QPointF logicalDelta,
                            const AspectRatio& ratio, qreal scale, QSize pixelSize);
QRect ratioNudgedSelection(QRect initial, SelectionEdge edge, SelectionNudge mode,
                           const AspectRatio& ratio, QSize pixelSize);
QRect largestInscribedRatioRect(QRect target, const AspectRatio& ratio);
QRect ratioAdjustedSelection(QRect current, const AspectRatio& ratio, QSize pixelSize);
struct CaptureRatioSettings
{
    QVector<AspectRatio> enabled;
    QVector<AspectRatio> disabled;
    std::optional<AspectRatio> quick;
};
CaptureRatioSettings sanitizedRatioSettings(const CaptureRatioSettings& settings);
QString serializeRatioList(const QVector<AspectRatio>& ratios);
QVector<AspectRatio> parseRatioList(const QString& text);
}
