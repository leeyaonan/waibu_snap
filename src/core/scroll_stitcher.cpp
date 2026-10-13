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
constexpr int downsample = 4;
constexpr int coarseStep = 8;
constexpr int refinementRadius = 8;
constexpr int ambiguityRadius = 2;
constexpr double maximumError = 9.0;
constexpr double minimumSeparation = 0.8;
constexpr double staticTolerance = 1.0;
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
    return image
        .scaled(std::max(1, image.width() / downsample), std::max(1, image.height() / downsample),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
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
                  int leading, int trailing, bool wholeOverlap = false)
{
    const int n = length(after, axis), w = breadth(after, axis);
    int start = std::max(leading, leading - delta);
    int end = std::min(n - trailing, n - trailing - delta);
    if (end <= start)
        return std::numeric_limits<double>::infinity();
    if (!wholeOverlap)
    {
        const int templateLength = std::min(end - start, std::clamp(n / 4, 8, 96));
        start += (end - start - templateLength) / 2;
        end = start + templateLength;
    }
    double total = 0;
    int samples = 0;
    const bool gray = before.format() == QImage::Format_Grayscale8;
    const int rowStep = std::max(1, (end - start) / (gray ? 8 : 32));
    const int columnStep = std::max(2, w / (gray ? 24 : 64));
    const uchar* beforeBits = before.constBits();
    const uchar* afterBits = after.constBits();
    const qsizetype beforeStride = before.bytesPerLine();
    const qsizetype afterStride = after.bytesPerLine();
    const bool vertical = axis == ScrollAxis::Vertical;
    for (int row = start; row < end; row += rowStep)
        for (int column = 0; column < w; column += columnStep)
        {
            const int bx = vertical ? column : row + delta;
            const int by = vertical ? row + delta : column;
            const int ax = vertical ? column : row;
            const int ay = vertical ? row : column;
            const int first =
                gray ? beforeBits[by * beforeStride + bx]
                     : qGray(reinterpret_cast<const QRgb*>(beforeBits + by * beforeStride)[bx]);
            const int second =
                gray ? afterBits[ay * afterStride + ax]
                     : qGray(reinterpret_cast<const QRgb*>(afterBits + ay * afterStride)[ax]);
            total += std::abs(first - second);
            ++samples;
        }
    return total / std::max(1, samples);
}
struct Match
{
    int delta = 0;
    int leading = 0;
    int trailing = 0;
    double error = std::numeric_limits<double>::infinity();
    bool reliable = false;
};
Match matchFrames(const QImage& before, const QImage& after, const QImage& beforeGray,
                  const QImage& afterGray, ScrollAxis axis, int lockedLeading, int lockedTrailing,
                  bool locked, int previousMotion)
{
    auto [leading, trailing] = locked ? std::pair<int, int>{lockedLeading, lockedTrailing}
                                      : staticBands(before, after, axis);
    const int n = length(after, axis);
    // 64px 小区域仍保留至少半帧重叠；常规区域按 96px / 25% 门槛。
    const int overlap = std::min(n / 2, std::max(96, n / 4));
    const int range = n - leading - trailing - overlap;
    Match result{0, leading, trailing};
    if (range < 1)
        return result;
    const double scale = double(length(afterGray, axis)) / n;
    const int grayLeading = int(std::ceil(leading * scale));
    const int grayTrailing = int(std::ceil(trailing * scale));
    double coarseBest = std::numeric_limits<double>::infinity();
    int coarseDelta = 0;
    QVector<std::pair<int, double>> candidates;
    for (int delta = -range; delta <= range; delta += coarseStep)
    {
        double score = std::numeric_limits<double>::infinity();
        // 粗扫 8px 对应约 2 个灰度像素；检查相邻灰度位置，避免高频内容落在格点间隙。
        for (int neighbor = -1; neighbor <= 1; ++neighbor)
            score = std::min(score, difference(beforeGray, afterGray, axis,
                                               qRound(delta * scale) + neighbor, grayLeading,
                                               grayTrailing));
        candidates.append({delta, score});
        if (score < coarseBest)
        {
            coarseBest = score;
            coarseDelta = delta;
        }
    }
    // 静止是独立候选，不依赖粗扫的余数。
    candidates.append({0, difference(beforeGray, afterGray, axis, 0, grayLeading, grayTrailing)});
    if (candidates.last().second < coarseBest)
    {
        coarseDelta = 0;
        coarseBest = candidates.last().second;
    }
    QVector<std::pair<int, double>> refined;
    for (int delta = std::max(-range, coarseDelta - refinementRadius);
         delta <= std::min(range, coarseDelta + refinementRadius); ++delta)
    {
        const double score = difference(before, after, axis, delta, leading, trailing);
        refined.append({delta, score});
        if (score < result.error)
        {
            result.error = score;
            result.delta = delta;
        }
    }
    if (!result.delta || result.error > maximumError)
        return result;
    double second = std::numeric_limits<double>::infinity();
    for (const auto& candidate : refined)
        if (std::abs(candidate.first - result.delta) > ambiguityRadius)
            second = std::min(second, candidate.second);
    // 除局部峰外再次精扫远处竞争峰，保留行高周期 / 相似纹理的拒绝保护。
    for (const auto& candidate : candidates)
        if (std::abs(candidate.first - result.delta) > refinementRadius * 2 &&
            candidate.second < coarseBest + 3.0)
            for (int delta = std::max(-range, candidate.first - 4);
                 delta <= std::min(range, candidate.first + 4); ++delta)
                second =
                    std::min(second, difference(before, after, axis, delta, leading, trailing));
    const bool separated =
        second - result.error >= minimumSeparation && result.error <= second * confidenceRatio;
    const bool continuous = !previousMotion ||
                            std::abs(result.delta) <= std::max(96, std::abs(previousMotion) * 4) ||
                            result.error < 1.0;
    result.reliable =
        separated && continuous &&
        difference(before, after, axis, result.delta, leading, trailing, true) <= maximumError;
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
    if (previous_.isNull())
    {
        previous_ = frame;
        previousGray_ = gray;
        initializeContent(frame, ScrollAxis::Vertical, 0, 0);
        ++stats_.accepted;
        stats_.lastFrameReliable = true;
        stats_.limitReached = stats_.lengthPx < frame.height() || stats_.lengthPx >= lengthLimit();
        return stats_.limitReached ? ScrollFrameOutcome::LimitReached
                                   : ScrollFrameOutcome::Accepted;
    }
    // 重复帧不构成质量警告；不刷新接受基准，微小逐步运动可累积到可测量位移。
    if (difference(previous_, frame, stats_.axis, 0, 0, 0, true) <= staticTolerance)
        return skip(false);
    Match motion = matchFrames(previous_, frame, previousGray_, gray, stats_.axis, leading_,
                               trailing_, stats_.motionDetected, previousMotion_);
    const ScrollAxis otherAxis =
        stats_.axis == ScrollAxis::Vertical ? ScrollAxis::Horizontal : ScrollAxis::Vertical;
    const Match orthogonal =
        !stats_.motionDetected || !motion.reliable
            ? matchFrames(previous_, frame, previousGray_, gray, otherAxis, 0, 0, false, 0)
            : Match{};
    if (!stats_.motionDetected)
    {
        ScrollAxis chosenAxis = stats_.axis;
        if (orthogonal.reliable && (!motion.reliable || orthogonal.error + 1.0 < motion.error))
        {
            motion = orthogonal;
            chosenAxis = otherAxis;
        }
        else if (orthogonal.reliable && motion.reliable)
            return skip(true);
        if (!motion.reliable)
            return skip(true);
        initializeContent(previous_, chosenAxis, motion.leading, motion.trailing);
        stats_.motionDetected = true;
    }
    else if (orthogonal.reliable && !motion.reliable)
    {
        skip(true);
        return ScrollFrameOutcome::AxisLocked;
    }
    if (!motion.reliable)
        return skip(true);
    const int contentLength = length(frame, stats_.axis) - leading_ - trailing_;
    const int position = currentPosition_ + motion.delta;
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
    currentPosition_ = position;
    previousMotion_ = motion.delta;
    stats_.lastMotionPx = motion.delta;
    previous_ = std::move(frame);
    previousGray_ = std::move(gray);
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
