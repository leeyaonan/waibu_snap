#include "core/scroll_stitcher.h"
#include <QColorSpace>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace waibusnap
{
namespace
{
constexpr int coarseStep = 1;
constexpr int coarseScale = 2;
constexpr int refinementRadius = 8;
constexpr int ambiguityRadius = 2;
constexpr int maximumBasins = 8;
constexpr double maximumError = 9.0;
constexpr double smoothMaximumError = 30.0;
constexpr double minimumSeparation = 0.8;
constexpr double staticTolerance = 1.0;
constexpr double referenceTolerance = 0.1;
constexpr double confidenceRatio = 0.82;
int length(const QImage& image, ScrollAxis axis)
{
    return axis == ScrollAxis::Vertical ? image.height() : image.width();
}
int breadth(const QImage& image, ScrollAxis axis)
{
    return axis == ScrollAxis::Vertical ? image.width() : image.height();
}
QRect band(const QImage& image, ScrollAxis axis, int from, int count)
{
    return axis == ScrollAxis::Vertical ? QRect(0, from, image.width(), count)
                                        : QRect(from, 0, count, image.height());
}
int luminance(const QImage& image, ScrollAxis axis, int row, int column)
{
    const int x = axis == ScrollAxis::Vertical ? column : row;
    const int y = axis == ScrollAxis::Vertical ? row : column;
    if (image.format() == QImage::Format_Grayscale8)
        return image.constScanLine(y)[x];
    return qGray(reinterpret_cast<const QRgb*>(image.constScanLine(y))[x]);
}
QImage grayFrame(const QImage& image)
{
    // 保留原像素笔画；缩小平滑会使 20 / 22px 文字的假谷胜过真实位移。
    QImage gray(image.size(), QImage::Format_Grayscale8);
    if (gray.isNull())
        return {};
    const int width = image.width(), height = image.height();
    for (int row = 0; row < height; ++row)
    {
        const auto* pixels = reinterpret_cast<const QRgb*>(image.constScanLine(row));
        uchar* output = gray.scanLine(row);
        for (int column = 0; column < width; ++column)
            output[column] = uchar(qGray(pixels[column]));
    }
    return gray;
}
QImage coarseFrame(const QImage& gray)
{
    QImage coarse(std::max(1, gray.width() / coarseScale), std::max(1, gray.height() / coarseScale),
                  QImage::Format_Grayscale8);
    if (coarse.isNull())
        return {};
    const int width = coarse.width(), height = coarse.height();
    const int sourceWidth = gray.width(), sourceHeight = gray.height();
    for (int row = 0; row < height; ++row)
    {
        const uchar* first = gray.constScanLine(row * coarseScale);
        const uchar* second = gray.constScanLine(std::min(sourceHeight - 1, row * coarseScale + 1));
        uchar* output = coarse.scanLine(row);
        for (int column = 0; column < width; ++column)
        {
            const int start = column * coarseScale;
            const int next = std::min(sourceWidth - 1, start + 1);
            output[column] = uchar((first[start] + first[next] + second[start] + second[next]) / 4);
        }
    }
    return coarse;
}
std::pair<int, int> staticBands(const QImage& before, const QImage& after, ScrollAxis axis)
{
    const int n = length(after, axis), w = breadth(after, axis);
    const auto stationary = [&](int row)
    {
        double difference = 0;
        int samples = 0;
        for (int column = 0; column < w; column += std::max(2, w / 128))
        {
            difference += std::abs(luminance(before, axis, row, column) -
                                   luminance(after, axis, row, column));
            ++samples;
        }
        return difference / std::max(1, samples) <= staticTolerance;
    };
    int leading = 0, trailing = 0;
    while (leading < n * 2 / 5 && stationary(leading))
        ++leading;
    while (trailing < n * 2 / 5 && stationary(n - 1 - trailing))
        ++trailing;
    return {leading, trailing};
}
// 在合法重叠中取中部模板，另用整个重叠核对；避免窄模板撞上周期性行高。
double difference(const QImage& before, const QImage& after, ScrollAxis axis, int delta,
                  int leading, int trailing, bool wholeOverlap = false, int columns = 64,
                  int templateLimit = 96)
{
    const int n = length(after, axis), w = breadth(after, axis);
    int start = std::max(leading, leading - delta);
    int end = std::min(n - trailing, n - trailing - delta);
    if (end <= start)
        return std::numeric_limits<double>::infinity();
    if (!wholeOverlap)
    {
        const int templateLength = std::min(end - start, std::clamp(n / 4, 8, templateLimit));
        start += (end - start - templateLength) / 2;
        end = start + templateLength;
    }
    quint64 total = 0;
    int samples = 0;
    // 沿运动轴逐行采样：稀疏行步长会与文字行距同相，整排落入笔画间隙。
    constexpr int rowStep = 1;
    const int columnStep = std::max(2, w / columns);
    const uchar* beforeBits = before.constBits();
    const uchar* afterBits = after.constBits();
    const qsizetype beforeStride = before.bytesPerLine();
    const qsizetype afterStride = after.bytesPerLine();
    if (axis == ScrollAxis::Vertical)
        for (int row = start; row < end; row += rowStep)
        {
            const uchar* first = beforeBits + (row + delta) * beforeStride;
            const uchar* second = afterBits + row * afterStride;
            for (int column = 0; column < w; column += columnStep)
                total += std::abs(int(first[column]) - int(second[column]));
        }
    else
        for (int column = 0; column < w; column += columnStep)
        {
            const uchar* first = beforeBits + column * beforeStride + delta;
            const uchar* second = afterBits + column * afterStride;
            for (int row = start; row < end; row += rowStep)
                total += std::abs(int(first[row]) - int(second[row]));
        }
    samples = (end - start) * ((w + columnStep - 1) / columnStep);
    return double(total) / std::max(1, samples);
}
struct Match
{
    int delta = 0;
    int leading = 0;
    int trailing = 0;
    double error = std::numeric_limits<double>::infinity();
    bool reliable = false;
    bool exact = false;
    int range = 0;
};
Match matchFrames(const QImage& before, const QImage& after, const QImage& beforeGray,
                  const QImage& afterGray, const QImage& beforeCoarse, const QImage& afterCoarse,
                  ScrollAxis axis, int lockedLeading, int lockedTrailing, bool locked,
                  int previousMotion, int currentOffset)
{
    auto [leading, trailing] = locked ? std::pair<int, int>{lockedLeading, lockedTrailing}
                                      : staticBands(before, after, axis);
    const int n = length(after, axis);
    // 64px 小区域仍保留至少半帧重叠；常规区域按 96px / 25% 门槛。
    const int overlap = std::min(n / 2, std::max(96, n / 4));
    const int range = n - leading - trailing - overlap;
    Match result{0, leading, trailing};
    result.range = range;
    if (range < 1)
        return result;
    const double scale = double(length(afterCoarse, axis)) / n;
    const int coarseRange = int(std::floor(range * scale));
    const int coarseLeading = int(std::ceil(leading * scale));
    const int coarseTrailing = int(std::ceil(trailing * scale));
    QVector<std::pair<int, double>> candidates;
    for (int delta = -coarseRange; delta <= coarseRange; delta += coarseStep)
    {
        // 粗扫只缩减横向样本和模板长度，沿轴逐像素搜索 / 逐行采样，避免相位混叠。
        const double score = difference(beforeCoarse, afterCoarse, axis, delta, coarseLeading,
                                        coarseTrailing, false, 24, 32);
        candidates.append({qRound(delta / scale), score});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& first, const auto& second)
                     { return first.second < second.second; });
    QVector<int> basins;
    for (const auto& candidate : candidates)
    {
        if (std::all_of(basins.cbegin(), basins.cend(), [&](int delta)
                        { return std::abs(delta - candidate.first) >= refinementRadius * 2; }))
            basins.append(candidate.first);
        if (basins.size() == maximumBasins)
            break;
    }
    // 零位移与最近速度是独立种子，避免灰度相位变化把缓慢运动挤出前八盆地。
    for (int seed : {0, currentOffset + previousMotion})
        if (std::abs(seed) <= range &&
            std::all_of(basins.cbegin(), basins.cend(),
                        [&](int delta) { return std::abs(delta - seed) > refinementRadius; }))
            basins.append(seed);
    // 平滑后浅底文字可能产生假谷；保留多个独立盆地，在原像素上择全局最优。
    QVector<std::pair<int, double>> refined;
    for (int basin : basins)
        for (int delta = std::max(-range, basin - refinementRadius);
             delta <= std::min(range, basin + refinementRadius); ++delta)
        {
            const double score = difference(beforeGray, afterGray, axis, delta, leading, trailing);
            refined.append({delta, score});
            if (score < result.error)
            {
                result.error = score;
                result.delta = delta;
            }
        }
    if (result.error > smoothMaximumError)
        return result;
    double second = std::numeric_limits<double>::infinity();
    for (const auto& candidate : refined)
        if (std::abs(candidate.first - result.delta) > ambiguityRadius)
            second = std::min(second, candidate.second);
    const bool separated =
        second - result.error >= minimumSeparation && result.error <= second * confidenceRatio;
    const bool continuous =
        !previousMotion ||
        std::abs(result.delta - currentOffset) <= std::max(96, std::abs(previousMotion) * 4) ||
        result.error < 1.0;
    const double overlapError =
        difference(beforeGray, afterGray, axis, result.delta, leading, trailing, true);
    const bool strict = result.error <= maximumError && overlapError <= maximumError;
    const bool smooth = result.error <= smoothMaximumError && overlapError <= smoothMaximumError;
    result.reliable = separated && continuous && (strict || smooth);
    result.exact = result.error <= referenceTolerance && overlapError <= referenceTolerance;
    return result;
}
}
ScrollStitcher::ScrollStitcher(ScrollStitchLimits limits) : limits_(limits)
{
    limits_.maxLengthPx = std::clamp<qint64>(limits_.maxLengthPx, 1, 20000);
    limits_.maxAreaPx = std::clamp<qint64>(limits_.maxAreaPx, 1, 30000000);
}
qint64 ScrollStitcher::lengthLimit() const
{
    const int width = breadth(previous_, stats_.axis);
    return width ? std::min(limits_.maxLengthPx, limits_.maxAreaPx / width) : limits_.maxLengthPx;
}
void ScrollStitcher::addWarning() { ++stats_.warnings; }
ScrollFrameOutcome ScrollStitcher::skip(bool warning)
{
    ++stats_.skipped;
    stats_.lastFrameReliable = !warning;
    if (warning && !skipping_)
        addWarning();
    skipping_ = warning;
    return ScrollFrameOutcome::Skipped;
}
void ScrollStitcher::initializeContent(const QImage& frame, ScrollAxis axis, int leading,
                                       int trailing)
{
    stats_.axis = axis;
    leading_ = leading;
    trailing_ = trailing;
    stats_.lengthPx = std::min<qint64>(length(frame, axis) - leading - trailing, lengthLimit());
    canvas_ = stats_.lengthPx > 0 ? frame.copy(band(frame, axis, leading, int(stats_.lengthPx)))
                                  : QImage();
    canvas_.setDevicePixelRatio(1);
    minimumPosition_ = 0;
    maximumPosition_ = length(frame, axis) - leading - trailing;
    currentPosition_ = 0;
    referencePosition_ = 0;
}
bool ScrollStitcher::extend(const QImage& frame, int from, int count, bool prepend)
{
    const int requestedCount = count;
    count = int(std::min<qint64>(count, lengthLimit() - stats_.lengthPx));
    if (prepend)
        from += requestedCount - count;
    if (count <= 0)
        return false;
    const int oldLength = int(stats_.lengthPx);
    const int newLength = oldLength + count;
    if (length(canvas_, stats_.axis) < newLength)
    {
        const int capacity =
            int(std::min<qint64>(lengthLimit(), std::max(newLength, oldLength * 2)));
        const QSize size = stats_.axis == ScrollAxis::Vertical ? QSize(frame.width(), capacity)
                                                               : QSize(capacity, frame.height());
        QImage grown(size, QImage::Format_RGB32);
        grown.setColorSpace(frame.colorSpace());
        if (grown.isNull())
            return false;
        QPainter painter(&grown);
        painter.drawImage(QPoint(), canvas_, band(canvas_, stats_.axis, 0, oldLength));
        painter.end();
        canvas_ = std::move(grown);
    }
    // 前插采用按行 / 列 memmove，避免申请第二张整图；只移动已使用部分。
    if (prepend)
    {
        if (stats_.axis == ScrollAxis::Vertical)
            std::memmove(canvas_.bits() + count * canvas_.bytesPerLine(), canvas_.bits(),
                         size_t(oldLength) * size_t(canvas_.bytesPerLine()));
        else
            for (int row = 0; row < canvas_.height(); ++row)
                std::memmove(canvas_.scanLine(row) + count * 4, canvas_.scanLine(row),
                             size_t(oldLength) * 4);
    }
    QPainter painter(&canvas_);
    const QPoint position = stats_.axis == ScrollAxis::Vertical
                                ? QPoint(0, prepend ? 0 : oldLength)
                                : QPoint(prepend ? 0 : oldLength, 0);
    painter.drawImage(position, frame, band(frame, stats_.axis, from, count));
    stats_.lengthPx = newLength;
    return true;
}
ScrollFrameOutcome ScrollStitcher::addFrame(const QImage& source)
{
    if (stats_.limitReached)
        return ScrollFrameOutcome::LimitReached;
    ++stats_.frames;
    stats_.lastMotionPx = 0;
    if (source.isNull() || (!previous_.isNull() && source.size() != previous_.size()))
        return skip(true);
    QImage frame = source.convertToFormat(QImage::Format_RGB32);
    frame.setDevicePixelRatio(1);
    if (frame.isNull())
        return skip(true);
    QImage gray = grayFrame(frame);
    if (gray.isNull())
        return skip(true);
    QImage coarse = coarseFrame(gray);
    if (coarse.isNull())
        return skip(true);
    if (previous_.isNull())
    {
        previous_ = frame;
        previousGray_ = gray;
        previousCoarse_ = coarse;
        initializeContent(frame, ScrollAxis::Vertical, 0, 0);
        ++stats_.accepted;
        stats_.lastFrameReliable = true;
        stats_.limitReached = stats_.lengthPx < frame.height() || stats_.lengthPx >= lengthLimit();
        return stats_.limitReached ? ScrollFrameOutcome::LimitReached
                                   : ScrollFrameOutcome::Accepted;
    }
    // 重复帧不构成质量警告；不刷新接受基准，微小逐步运动可累积到可测量位移。
    if (referencePosition_ == currentPosition_ &&
        difference(previousGray_, gray, stats_.axis, 0, 0, 0, true) <= staticTolerance)
        return skip(false);
    Match motion = matchFrames(previous_, frame, previousGray_, gray, previousCoarse_, coarse,
                               stats_.axis, leading_, trailing_, stats_.motionDetected,
                               previousMotion_, currentPosition_ - referencePosition_);
    const ScrollAxis otherAxis =
        stats_.axis == ScrollAxis::Vertical ? ScrollAxis::Horizontal : ScrollAxis::Vertical;
    const Match orthogonal =
        !stats_.motionDetected || !motion.reliable
            ? matchFrames(previous_, frame, previousGray_, gray, previousCoarse_, coarse, otherAxis,
                          0, 0, false, 0, 0)
            : Match{};
    // 亚像素运动的最佳整数位移仍为零时保留旧基准，运动可在后续帧累积。
    if (motion.reliable && !motion.delta && referencePosition_ == currentPosition_ &&
        (!orthogonal.reliable || !orthogonal.delta))
        return skip(false);
    if (!stats_.motionDetected)
    {
        ScrollAxis chosenAxis = stats_.axis;
        if (orthogonal.reliable && orthogonal.delta &&
            (!motion.reliable || orthogonal.error + 1.0 < motion.error))
        {
            motion = orthogonal;
            chosenAxis = otherAxis;
        }
        else if (orthogonal.reliable && orthogonal.delta && motion.reliable && motion.delta)
            return skip(true);
        if (!motion.reliable)
            return skip(true);
        initializeContent(previous_, chosenAxis, motion.leading, motion.trailing);
        stats_.motionDetected = true;
    }
    else if (orthogonal.reliable && orthogonal.delta && !motion.reliable)
    {
        skip(true);
        return ScrollFrameOutcome::AxisLocked;
    }
    if (!motion.reliable)
        return skip(true);
    const int contentLength = length(frame, stats_.axis) - leading_ - trailing_;
    const int position = referencePosition_ + motion.delta;
    if (position == currentPosition_)
        return skip(false);
    bool extended = true;
    if (position < minimumPosition_)
    {
        const int count = minimumPosition_ - position;
        extended = extend(frame, leading_, count, true);
        if (extended)
            minimumPosition_ = position;
    }
    else if (position + contentLength > maximumPosition_)
    {
        const int count = position + contentLength - maximumPosition_;
        extended = extend(frame, length(frame, stats_.axis) - trailing_ - count, count, false);
        if (extended)
            maximumPosition_ = position + contentLength;
    }
    if (!extended)
        return skip(true);
    stats_.lastMotionPx = position - currentPosition_;
    currentPosition_ = position;
    previousMotion_ = stats_.lastMotionPx;
    // 软匹配逐帧取整会累积漂移；保持基准直到相位一致，预留至少两步重叠余量。
    if (motion.exact ||
        std::abs(motion.delta) >= motion.range - std::max(32, std::abs(stats_.lastMotionPx) * 2))
    {
        referencePosition_ = position;
        previous_ = std::move(frame);
        previousGray_ = std::move(gray);
        previousCoarse_ = std::move(coarse);
    }
    ++stats_.accepted;
    stats_.lastFrameReliable = true;
    skipping_ = false;
    if (stats_.lengthPx >= lengthLimit())
    {
        stats_.limitReached = true;
        return ScrollFrameOutcome::LimitReached;
    }
    return ScrollFrameOutcome::Accepted;
}
QImage ScrollStitcher::takeResult()
{
    if (canvas_.isNull() || stats_.lengthPx <= 0)
        return {};
    const QSize size = stats_.axis == ScrollAxis::Vertical
                           ? QSize(canvas_.width(), int(stats_.lengthPx))
                           : QSize(int(stats_.lengthPx), canvas_.height());
    auto* allocation = new QImage(std::move(canvas_));
    QImage image(
        allocation->bits(), size.width(), size.height(), allocation->bytesPerLine(),
        allocation->format(), [](void* owner) { delete static_cast<QImage*>(owner); }, allocation);
    image.setColorSpace(allocation->colorSpace());
    image.setDevicePixelRatio(1);
    previous_ = {};
    previousGray_ = {};
    return image;
}
QImage ScrollStitcher::result() const
{
    return canvas_.copy(band(canvas_, stats_.axis, 0, int(stats_.lengthPx)));
}
}
