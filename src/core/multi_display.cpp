#include "core/multi_display.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>
namespace waibusnap
{
namespace
{
struct Row
{
    int top = 0;
    int bottom = 0;
    int left = 0;
    QVector<MultiDisplayEntry> entries;
};
bool validDisplay(const DisplayTarget& display)
{
    const qreal dpr = display.devicePixelRatio;
    return display.id != 0 && !display.logicalGeometry.isEmpty() && std::isfinite(dpr) && dpr > 0;
}
QSize physicalSize(const DisplayTarget& display)
{
    const qreal width = display.logicalGeometry.width() * display.devicePixelRatio;
    const qreal height = display.logicalGeometry.height() * display.devicePixelRatio;
    const auto maximum = double(std::numeric_limits<int>::max());
    if (!std::isfinite(width) || !std::isfinite(height) || width > maximum || height > maximum)
        return {};
    return {qRound(width), qRound(height)};
}
int scaled(double value, qreal scale)
{
    const double result = value * scale;
    const auto maximum = double(std::numeric_limits<int>::max());
    if (!std::isfinite(result) || result > maximum ||
        result < double(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::max();
    return qRound(result);
}
QString extensionOf(const QString& extension)
{
    QString clean = extension.trimmed();
    while (clean.startsWith(QLatin1Char('.')))
        clean.remove(0, 1);
    return clean.isEmpty() ? QStringLiteral("png") : clean.toLower();
}
}
QString multiDisplayBaseName(const QDateTime& timestamp)
{
    return QStringLiteral("WaibuSnap_%1")
        .arg(timestamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")));
}
QStringList multiDisplayFileNames(const QString& base, int displayCount, bool stitch,
                                  const QString& extension)
{
    QStringList names;
    if (base.isEmpty() || displayCount <= 0 || displayCount > 99)
        return names;
    const QString suffix = extensionOf(extension);
    const auto append = [&names, &base, &suffix](int index)
    {
        names.append(
            QStringLiteral("%1_%2.%3").arg(base).arg(index, 2, 10, QLatin1Char('0')).arg(suffix));
    };
    if (stitch)
        append(0);
    for (int index = 0; index < displayCount; ++index)
        append(index + 1);
    return names;
}
QString multiDisplayPrefix(const QString& directory, const QDateTime& timestamp, int displayCount,
                           bool stitch, const QString& extension)
{
    if (directory.isEmpty() || displayCount <= 0)
        return {};
    const QDir folder(directory);
    for (int attempt = 0; attempt < multiDisplayPrefixAttempts; ++attempt)
    {
        const auto base = multiDisplayBaseName(timestamp.addMSecs(attempt));
        const auto names = multiDisplayFileNames(base, displayCount, stitch, extension);
        if (names.isEmpty())
            return {};
        const bool conflict =
            std::any_of(names.begin(), names.end(),
                        [&folder](const QString& name)
                        {
                            const QString path = folder.filePath(name);
                            return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
                        });
        if (!conflict)
            return base;
    }
    return {};
}
MultiDisplayLayout buildMultiDisplayLayout(const QVector<DisplayTarget>& displays)
{
    MultiDisplayLayout layout;
    QVector<DisplayTarget> valid;
    valid.reserve(displays.size());
    for (const auto& display : displays)
        if (validDisplay(display) && !physicalSize(display).isEmpty())
            valid.append(display);
    if (valid.isEmpty())
        return layout;
    std::sort(valid.begin(), valid.end(),
              [](const DisplayTarget& first, const DisplayTarget& second)
              {
                  if (first.logicalGeometry.top() != second.logicalGeometry.top())
                      return first.logicalGeometry.top() < second.logicalGeometry.top();
                  if (first.logicalGeometry.left() != second.logicalGeometry.left())
                      return first.logicalGeometry.left() < second.logicalGeometry.left();
                  return first.id < second.id;
              });
    // 垂直投影有交集即同排，行区间取并集；按 top 预排序保证行只在下方扩展。
    QVector<Row> rows;
    for (const auto& display : valid)
    {
        const auto& geometry = display.logicalGeometry;
        const int top = geometry.top();
        const int bottom = geometry.top() + geometry.height();
        auto row = std::find_if(
            rows.begin(), rows.end(), [top, bottom](const Row& candidate)
            { return std::min(candidate.bottom, bottom) > std::max(candidate.top, top); });
        if (row == rows.end())
        {
            Row created;
            created.top = top;
            created.bottom = bottom;
            created.left = geometry.left();
            created.entries.append({display, physicalSize(display)});
            rows.append(created);
            continue;
        }
        row->top = std::min(row->top, top);
        row->bottom = std::max(row->bottom, bottom);
        row->left = std::min(row->left, geometry.left());
        row->entries.append({display, physicalSize(display)});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& first, const Row& second)
              {
                  if (first.top != second.top)
                      return first.top < second.top;
                  return first.left < second.left;
              });
    QRect bounds;
    qreal scale = 0;
    for (auto& row : rows)
    {
        std::sort(row.entries.begin(), row.entries.end(),
                  [](const MultiDisplayEntry& first, const MultiDisplayEntry& second)
                  {
                      if (first.target.logicalGeometry.left() !=
                          second.target.logicalGeometry.left())
                          return first.target.logicalGeometry.left() <
                                 second.target.logicalGeometry.left();
                      return first.target.id < second.target.id;
                  });
        for (const auto& entry : row.entries)
        {
            const auto& geometry = entry.target.logicalGeometry;
            bounds = bounds.isNull() ? geometry : bounds.united(geometry);
            scale = std::max(scale, entry.target.devicePixelRatio);
            layout.ordered.append(entry);
        }
    }
    if (layout.ordered.isEmpty() || bounds.isEmpty() || scale <= 0)
    {
        layout.ordered.clear();
        return layout;
    }
    layout.logicalBounds = bounds;
    layout.scale = scale;
    const qint64 canvasWidth = qint64(scaled(bounds.width(), scale));
    const qint64 canvasHeight = qint64(scaled(bounds.height(), scale));
    if (canvasWidth <= 0 || canvasHeight <= 0)
    {
        layout.ordered.clear();
        return layout;
    }
    layout.canvasPixels =
        QSize(int(std::min<qint64>(canvasWidth, std::numeric_limits<int>::max())),
              int(std::min<qint64>(canvasHeight, std::numeric_limits<int>::max())));
    layout.placements.reserve(layout.ordered.size());
    for (const auto& entry : layout.ordered)
    {
        const auto& geometry = entry.target.logicalGeometry;
        layout.placements.append(QRect(scaled(double(geometry.left() - bounds.left()), scale),
                                       scaled(double(geometry.top() - bounds.top()), scale),
                                       scaled(geometry.width(), scale),
                                       scaled(geometry.height(), scale)));
    }
    const qint64 area = canvasWidth * canvasHeight;
    if (canvasWidth > multiDisplayMaximumCanvasSide ||
        canvasHeight > multiDisplayMaximumCanvasSide || area > multiDisplayMaximumCanvasArea)
    {
        layout.stitchExceedsLimit = true;
        layout.limitReason = QStringLiteral("拼接图过大（%1×%2），请关闭「额外保存拼接图」后重试。")
                                 .arg(canvasWidth)
                                 .arg(canvasHeight);
    }
    return layout;
}
}
