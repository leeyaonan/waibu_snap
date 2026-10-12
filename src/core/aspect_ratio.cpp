#include "core/aspect_ratio.h"
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
bool validRatio(const AspectRatio& ratio)
{
    return std::isfinite(ratio.width) && std::isfinite(ratio.height) && ratio.width > 0 &&
           ratio.height > 0;
}
bool validEntry(const AspectRatio& ratio)
{
    const auto valid = [](double value)
    {
        return std::isfinite(value) && value >= 0.01 && value <= 999.99 &&
               std::abs(value * 100 - std::round(value * 100)) < 1e-8;
    };
    return valid(ratio.width) && valid(ratio.height);
}
bool containsRatio(const QVector<AspectRatio>& ratios, const AspectRatio& ratio)
{
    return std::any_of(ratios.begin(), ratios.end(),
                       [&ratio](const AspectRatio& item) { return sameAspectRatio(item, ratio); });
}
QSize fittedSize(double desiredWidth, const AspectRatio& ratio, int maximumWidth, int maximumHeight)
{
    const double r = ratio.width / ratio.height;
    const double width =
        std::clamp(desiredWidth, 0.0, std::min(double(maximumWidth), maximumHeight * r));
    // 两个理想维度分别最近取整，较短维度的比例误差不超过 1 像素。
    return {std::clamp(qRound(width), 1, maximumWidth),
            std::clamp(qRound(width / r), 1, maximumHeight)};
}
QPoint pixelPoint(QPointF point, qreal scale, QSize size)
{
    return {qRound(std::clamp(point.x() * scale, 0.0, double(size.width()))),
            qRound(std::clamp(point.y() * scale, 0.0, double(size.height())))};
}
bool finitePoint(QPointF point) { return std::isfinite(point.x()) && std::isfinite(point.y()); }
bool validSelection(QRect rect, QSize size)
{
    return !rect.isEmpty() && QRect(QPoint(), size).contains(rect);
}
}
std::optional<AspectRatio> parseAspectRatio(const QString& text)
{
    static const QRegularExpression expression(
        QStringLiteral("^([0-9]+(?:\\.[0-9]{1,2})?)\\s*:\\s*([0-9]+(?:\\.[0-9]{1,2})?)$"));
    const auto match = expression.match(text.trimmed());
    if (!match.hasMatch())
        return {};
    const AspectRatio ratio{match.captured(1).toDouble(), match.captured(2).toDouble()};
    return validEntry(ratio) ? std::optional<AspectRatio>(ratio) : std::nullopt;
}
QString formatAspectRatio(const AspectRatio& ratio)
{
    const auto number = [](double value)
    {
        QString result = QString::number(value, 'f', 2);
        while (result.endsWith(QLatin1Char('0')))
            result.chop(1);
        if (result.endsWith(QLatin1Char('.')))
            result.chop(1);
        return result;
    };
    return number(ratio.width) + QLatin1Char(':') + number(ratio.height);
}
bool sameAspectRatio(const AspectRatio& a, const AspectRatio& b)
{
    if (!validRatio(a) || !validRatio(b))
        return false;
    const double ar = a.width / a.height;
    const double br = b.width / b.height;
    return std::abs(ar - br) <= std::max(ar, br) * (0.001 + 1e-12);
}
QVector<AspectRatio> defaultCaptureRatios()
{
    return {{1, 1}, {16, 9}, {9, 16}, {4, 3}, {3, 4}, {3, 2}, {2, 3}, {2.35, 1}};
}
QRect ratioCreatedSelection(QPointF anchor, QPointF cursor, const AspectRatio& ratio, qreal scale,
                            QSize pixelSize)
{
    if (!validRatio(ratio) || !std::isfinite(scale) || scale <= 0 || pixelSize.isEmpty() ||
        !finitePoint(anchor) || !finitePoint(cursor))
        return {};
    const QPoint a = pixelPoint(anchor, scale, pixelSize);
    const QPoint b = pixelPoint(cursor, scale, pixelSize);
    const int sx = b.x() < a.x() || a.x() == pixelSize.width() ? -1 : 1;
    const int sy = b.y() < a.y() || a.y() == pixelSize.height() ? -1 : 1;
    const double width = std::max(double(std::abs(b.x() - a.x())),
                                  std::abs(b.y() - a.y()) * ratio.width / ratio.height);
    const QSize size = fittedSize(width, ratio, sx < 0 ? a.x() : pixelSize.width() - a.x(),
                                  sy < 0 ? a.y() : pixelSize.height() - a.y());
    return {a.x() - (sx < 0 ? size.width() : 0), a.y() - (sy < 0 ? size.height() : 0), size.width(),
            size.height()};
}
QRect ratioResizedSelection(QRect initial, SelectionEdges edges, QPointF logicalDelta,
                            const AspectRatio& ratio, qreal scale, QSize pixelSize)
{
    if (!validSelection(initial, pixelSize) || !validRatio(ratio) || !std::isfinite(scale) ||
        scale <= 0 || !finitePoint(logicalDelta))
        return {};
    const bool left = edges.testFlag(SelectionEdge::Left);
    const bool right = edges.testFlag(SelectionEdge::Right);
    const bool top = edges.testFlag(SelectionEdge::Top);
    const bool bottom = edges.testFlag(SelectionEdge::Bottom);
    if ((!left && !right && !top && !bottom) || (left && right) || (top && bottom))
        return initial;
    const int ax = left ? initial.x() + initial.width() : initial.x();
    const int ay = top ? initial.y() + initial.height() : initial.y();
    const double dx =
        std::clamp(logicalDelta.x() * scale, -double(pixelSize.width()), double(pixelSize.width()));
    const double dy = std::clamp(logicalDelta.y() * scale, -double(pixelSize.height()),
                                 double(pixelSize.height()));
    const double width = std::max(0.0, initial.width() + (left ? -dx : dx));
    const double height = std::max(0.0, initial.height() + (top ? -dy : dy));
    const double r = ratio.width / ratio.height;
    if ((left || right) && (top || bottom))
    {
        const QSize size =
            fittedSize(std::max(width, height * r), ratio, left ? ax : pixelSize.width() - ax,
                       top ? ay : pixelSize.height() - ay);
        return {ax - (left ? size.width() : 0), ay - (top ? size.height() : 0), size.width(),
                size.height()};
    }
    const double cx = initial.x() + initial.width() / 2.0;
    const double cy = initial.y() + initial.height() / 2.0;
    if (left || right)
    {
        const QSize size = fittedSize(width, ratio, left ? ax : pixelSize.width() - ax,
                                      qRound(2 * std::min(cy, pixelSize.height() - cy)));
        return {ax - (left ? size.width() : 0), qRound(cy - size.height() / 2.0), size.width(),
                size.height()};
    }
    const QSize size =
        fittedSize(height * r, ratio, qRound(2 * std::min(cx, pixelSize.width() - cx)),
                   top ? ay : pixelSize.height() - ay);
    return {qRound(cx - size.width() / 2.0), ay - (top ? size.height() : 0), size.width(),
            size.height()};
}
QRect ratioNudgedSelection(QRect initial, SelectionEdge edge, SelectionNudge mode,
                           const AspectRatio& ratio, QSize pixelSize)
{
    if (mode == SelectionNudge::Move)
        return nudgedPixelSelection(initial, edge, mode, pixelSize);
    QPoint delta;
    switch (edge)
    {
    case SelectionEdge::Left:
        delta = {-1, 0};
        break;
    case SelectionEdge::Top:
        delta = {0, -1};
        break;
    case SelectionEdge::Right:
        delta = {1, 0};
        break;
    case SelectionEdge::Bottom:
        delta = {0, 1};
        break;
    }
    return ratioResizedSelection(initial, edge, mode == SelectionNudge::Shrink ? -delta : delta,
                                 ratio, 1, pixelSize);
}
QRect largestInscribedRatioRect(QRect target, const AspectRatio& ratio)
{
    if (target.isEmpty() || !validRatio(ratio))
        return {};
    const QSize size = fittedSize(target.width(), ratio, target.width(), target.height());
    return {target.x() + qRound((target.width() - size.width()) / 2.0),
            target.y() + qRound((target.height() - size.height()) / 2.0), size.width(),
            size.height()};
}
QRect ratioAdjustedSelection(QRect current, const AspectRatio& ratio, QSize pixelSize)
{
    if (!validSelection(current, pixelSize) || !validRatio(ratio))
        return {};
    const double cx = current.x() + current.width() / 2.0;
    const double cy = current.y() + current.height() / 2.0;
    const QSize size =
        fittedSize(current.width(), ratio, qRound(2 * std::min(cx, pixelSize.width() - cx)),
                   qRound(2 * std::min(cy, pixelSize.height() - cy)));
    return {qRound(cx - size.width() / 2.0), qRound(cy - size.height() / 2.0), size.width(),
            size.height()};
}
CaptureRatioSettings sanitizedRatioSettings(const CaptureRatioSettings& settings)
{
    CaptureRatioSettings result;
    for (const auto& ratio : settings.enabled)
        if (validEntry(ratio) && !containsRatio(result.enabled, ratio))
            result.enabled.append(ratio);
    for (const auto& ratio : settings.disabled)
        if (validEntry(ratio) && !containsRatio(result.enabled, ratio) &&
            !containsRatio(result.disabled, ratio))
            result.disabled.append(ratio);
    if (settings.quick)
        for (const auto& ratio : result.enabled)
            if (sameAspectRatio(ratio, *settings.quick))
                result.quick = ratio;
    if (!result.quick)
    {
        for (const auto& ratio : result.enabled)
            if (sameAspectRatio(ratio, {1, 1}))
                result.quick = ratio;
        if (!result.quick && !result.enabled.isEmpty())
            result.quick = result.enabled.first();
    }
    return result;
}
QString serializeRatioList(const QVector<AspectRatio>& ratios)
{
    QStringList result;
    for (const auto& ratio : ratios)
        result.append(formatAspectRatio(ratio));
    return result.join(QLatin1Char(','));
}
QVector<AspectRatio> parseRatioList(const QString& text)
{
    QVector<AspectRatio> result;
    for (const auto& item : text.split(QLatin1Char(',')))
        if (const auto ratio = parseAspectRatio(item))
            result.append(*ratio);
    return result;
}
}
