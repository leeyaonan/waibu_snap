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
                  int templateLimit = 96, int across = 0)
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
    const int columnStart = std::max(0, -across), columnEnd = std::min(w, w - across);
    if (columnEnd <= columnStart)
        return std::numeric_limits<double>::infinity();
    const uchar* beforeBits = before.constBits();
    const uchar* afterBits = after.constBits();
    const qsizetype beforeStride = before.bytesPerLine();
    const qsizetype afterStride = after.bytesPerLine();
    if (axis == ScrollAxis::Vertical)
        for (int row = start; row < end; row += rowStep)
        {
            const uchar* first = beforeBits + (row + delta) * beforeStride;
            const uchar* second = afterBits + row * afterStride;
            for (int column = columnStart; column < columnEnd; column += columnStep)
                total += std::abs(int(first[column + across]) - int(second[column]));
        }
    else
        for (int column = columnStart; column < columnEnd; column += columnStep)
        {
            const uchar* first = beforeBits + (column + across) * beforeStride + start + delta;
            const uchar* second = afterBits + column * afterStride + start;
            for (int row = 0; row < end - start; row += rowStep)
                total += std::abs(int(first[row]) - int(second[row]));
        }
    samples = (end - start) * ((columnEnd - columnStart + columnStep - 1) / columnStep);
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
    double separationMargin = std::numeric_limits<double>::quiet_NaN();
    double continuityMargin = std::numeric_limits<double>::quiet_NaN();
    double diagonalMargin = std::numeric_limits<double>::infinity();
    double smoothMargin = -1;
    double referenceMargin = std::numeric_limits<double>::quiet_NaN();
};
Match matchFrames(const QImage& before, const QImage& after, const QImage& beforeGray,
                  const QImage& afterGray, const QImage& beforeCoarse, const QImage& afterCoarse,
                  ScrollAxis axis, int lockedLeading, int lockedTrailing, bool locked,
                  int previousMotion, int currentOffset, bool shortOverlap = false)
{
    auto [leading, trailing] = locked ? std::pair<int, int>{lockedLeading, lockedTrailing}
                                      : staticBands(before, after, axis);
    const int n = length(after, axis);
    // 64px 小区域仍保留至少半帧重叠；常规区域按 96px / 25% 门槛。
    const int overlap = shortOverlap ? 16 : std::min(n / 2, std::max(96, n / 4));
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
    result.smoothMargin = smoothMaximumError - result.error;
    if (result.error > smoothMaximumError)
        return result;
    double second = std::numeric_limits<double>::infinity();
    for (const auto& candidate : refined)
        if (std::abs(candidate.first - result.delta) > ambiguityRadius)
            second = std::min(second, candidate.second);
    result.separationMargin = std::min(second - result.error - minimumSeparation,
                                       second * confidenceRatio - result.error);
    result.continuityMargin =
        (!previousMotion || result.error < 1.0)
            ? 96
            : std::max(96, std::abs(previousMotion) * 4) - std::abs(result.delta - currentOffset);
    const bool separated =
        second - result.error >= minimumSeparation && result.error <= second * confidenceRatio;
    const bool continuous =
        !previousMotion ||
        std::abs(result.delta - currentOffset) <= std::max(96, std::abs(previousMotion) * 4) ||
        result.error < 1.0;
    const double overlapError =
        difference(beforeGray, afterGray, axis, result.delta, leading, trailing, true);
    result.smoothMargin = smoothMaximumError - std::max(result.error, overlapError);
    result.referenceMargin = referenceTolerance - std::max(result.error, overlapError);
    const bool strict = result.error <= maximumError && overlapError <= maximumError;
    const bool smooth = result.error <= smoothMaximumError && overlapError <= smoothMaximumError;
    result.reliable = separated && continuous && (strict || smooth);
    // 白底会稀释斜向误差；若另一轴移动能显著降低整段残差，拒绝一维假匹配。
    if (result.reliable && overlapError > referenceTolerance)
        for (int across = -32; across <= 32; ++across)
        {
            if (!across)
                continue;
            const double diagonalTemplate = difference(beforeGray, afterGray, axis, result.delta,
                                                       leading, trailing, false, 64, 96, across);
            if (result.error - diagonalTemplate < minimumSeparation ||
                diagonalTemplate > result.error * confidenceRatio)
                continue;
            const double diagonalError = difference(beforeGray, afterGray, axis, result.delta,
                                                    leading, trailing, true, 64, 96, across);
            result.diagonalMargin = std::min(
                result.diagonalMargin, std::max(diagonalError - (overlapError - minimumSeparation),
                                                diagonalError - overlapError * confidenceRatio));
            if (overlapError - diagonalError >= minimumSeparation &&
                diagonalError <= overlapError * confidenceRatio)
            {
                result.reliable = false;
                break;
            }
        }
    result.exact = result.error <= referenceTolerance && overlapError <= referenceTolerance;
    return result;
}
}
void ScrollDistribution::add(double value)
{
    if (!std::isfinite(value))
        return;
    ++count;
    minimum = std::min(minimum, value);
    maximum = std::max(maximum, value);
    sum += value;
    constexpr double boundaries[] = {0, .1, .5, 1, 2, 4, 8, 16, 32};
    ++bins[size_t(std::upper_bound(std::begin(boundaries), std::end(boundaries), value) -
                  std::begin(boundaries))];
}
ScrollStitchLimits ScrollStitchLimits::forDisplay(qreal dpr, qint64 logicalLength,
                                                  qint64 logicalArea)
{
    dpr = std::isfinite(dpr) ? std::clamp(dpr, qreal(1), qreal(4)) : 1;
    return {qRound64(std::clamp<qint64>(logicalLength, 1, 1000000) * dpr),
            qRound64(std::clamp<qint64>(logicalArea, 1, 100000000) * dpr * dpr), dpr};
}
ScrollStitcher::ScrollStitcher(ScrollStitchLimits limits) : limits_(limits)
{
    limits_.maxLengthPx = std::clamp<qint64>(limits_.maxLengthPx, 1, 4000000);
    limits_.maxAreaPx = std::clamp<qint64>(limits_.maxAreaPx, 1, 30000000);
    limits_.workingSetBytes = std::clamp<qint64>(limits_.workingSetBytes, 1, 120000000);
    stats_.devicePixelRatio = std::isfinite(limits_.devicePixelRatio)
                                  ? std::clamp(limits_.devicePixelRatio, qreal(1), qreal(4))
                                  : 1;
}
qint64 ScrollStitcher::canvasAreaBudget() const
{
    // 为源帧、像素参考、灰度 / 粗图及局部验证留 20 字节 / 源像素余量。
    const qint64 reserve = qint64(previous_.width()) * previous_.height() * 20;
    return std::min(limits_.maxAreaPx, std::max<qint64>(0, limits_.workingSetBytes - reserve) / 4);
}
qint64 ScrollStitcher::lengthLimit() const
{
    const int width = breadth(previous_, stats_.axis);
    return width ? std::min(limits_.maxLengthPx, canvasAreaBudget() / width) : limits_.maxLengthPx;
}
void ScrollStitcher::addWarning()
{
    ++stats_.interruptions;
    ++stats_.warnings;
    pendingTail_ = false;
}
void ScrollStitcher::finalize()
{
    if (finalized_)
        return;
    finalized_ = true;
    if (pendingTail_ && !skipping_)
    {
        ++stats_.unverifiedTails;
        ++stats_.warnings;
    }
}
ScrollFrameOutcome ScrollStitcher::skip(ScrollSkipReason reason)
{
    ++stats_.skipped;
    stats_.lastSkipReason = reason;
    stats_.lastFrameReliable =
        reason == ScrollSkipReason::NoMotion || reason == ScrollSkipReason::ContentChange;
    if (reason == ScrollSkipReason::NoMotion)
        ++stats_.noMotion;
    else if (reason == ScrollSkipReason::ContentChange)
        ++stats_.hiccups;
    else if (reason == ScrollSkipReason::LowConfidence)
        ++stats_.lowConfidence;
    else if (reason == ScrollSkipReason::RangeExhausted)
        ++stats_.rangeExceededFrames;
    else if (reason == ScrollSkipReason::AxisChange)
        ++stats_.axisChanges;
    else if (reason == ScrollSkipReason::InvalidFrame)
        ++stats_.invalidFrames;
    if (reason == ScrollSkipReason::RangeExhausted && !skipping_)
    {
        ++stats_.gaps;
        ++stats_.warnings;
        skipping_ = true;
    }
    pendingTail_ = !stats_.lastFrameReliable;
    if (stats_.lastFrameReliable)
        skipping_ = false;
    return ScrollFrameOutcome::Skipped;
}
void ScrollStitcher::anchor(QImage frame, QImage gray, QImage coarse, int position)
{
    referencePosition_ = position;
    previous_ = std::move(frame);
    previousGray_ = std::move(gray);
    previousCoarse_ = std::move(coarse);
    diagnostics_.lastAnchorFrame = stats_.frames;
    diagnostics_.referenceAge = 0;
}
void ScrollStitcher::recordMatch(int delta, int range, double error)
{
    diagnostics_.lastDelta = delta;
    diagnostics_.lastRange = range;
    diagnostics_.lastError = error;
}
bool ScrollStitcher::hasStableSupport(const QImage& gray, ScrollAxis axis, int delta) const
{
    // 8×8 分区须有足够且分散的二维纹理逐像素同相；空白背景不能证明静止。
    // 不用“小平均误差”重锚，避免 0.1 / 0.2px 滚动反复取整丢失。
    const int n = length(gray, axis), w = breadth(gray, axis);
    const int start = std::max(leading_, leading_ - delta);
    const int end = std::min(n - trailing_, n - trailing_ - delta);
    if (end - start < 32 || w < 32)
        return false;
    int informative = 0, stable = 0;
    std::array<bool, 8> rows{}, columns{};
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
        {
            int differences = 0, samples = 0, alongTexture = 0, acrossTexture = 0;
            for (int row = start + r * (end - start) / 8;
                 row < start + (r + 1) * (end - start) / 8 - 1; ++row)
                for (int column = c * w / 8; column < (c + 1) * w / 8 - 1;
                     column += std::max(1, w / 128))
                {
                    const int a = luminance(previousGray_, axis, row + delta, column);
                    const int b = luminance(gray, axis, row, column);
                    differences += std::abs(a - b) > 1;
                    alongTexture +=
                        std::abs(a - luminance(previousGray_, axis, row + delta + 1, column)) > 8;
                    acrossTexture +=
                        std::abs(a - luminance(previousGray_, axis, row + delta, column + 1)) > 8;
                    ++samples;
                }
            if (alongTexture < 8 || acrossTexture < 8)
                continue;
            ++informative;
            if (differences <= samples / 200)
            {
                ++stable;
                rows[size_t(r)] = true;
                columns[size_t(c)] = true;
            }
        }
    return informative >= 12 && stable * 10 >= informative * 6 &&
           std::count(rows.begin(), rows.end(), true) >= 4 &&
           std::count(columns.begin(), columns.end(), true) >= 4;
}
void ScrollStitcher::initializeContent(const QImage& frame, ScrollAxis axis, int leading,
                                       int trailing)
{
    stats_.axis = axis;
    leading_ = leading;
    trailing_ = trailing;
    stats_.lengthPx = std::min<qint64>(length(frame, axis) - leading - trailing, lengthLimit());
    stats_.memoryLimited =
        canvasAreaBudget() < limits_.maxAreaPx &&
        lengthLimit() <
            std::min(limits_.maxLengthPx, limits_.maxAreaPx / std::max(1, breadth(frame, axis)));
    // 一次预留最终容量，避免几何扩容时旧 / 新整图并存突破内存预算。
    canvas_ = {};
    const QSize capacity = axis == ScrollAxis::Vertical ? QSize(frame.width(), int(lengthLimit()))
                                                        : QSize(int(lengthLimit()), frame.height());
    canvas_ = QImage(capacity, QImage::Format_RGB32);
    canvas_.setColorSpace(frame.colorSpace());
    if (!canvas_.isNull())
    {
        QPainter painter(&canvas_);
        painter.drawImage(QPoint(), frame, band(frame, axis, leading, int(stats_.lengthPx)));
    }
    else
        stats_.lengthPx = 0;
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
    if (canvas_.isNull())
        return false;
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
    if (stats_.limitReached || finalized_)
        return ScrollFrameOutcome::LimitReached;
    ++stats_.frames;
    stats_.lastMotionPx = 0;
    recordMatch(0, 0, 0);
    stats_.lastFrameReliable = false;
    stats_.lastSkipReason = ScrollSkipReason::None;
    diagnostics_.referenceAge = stats_.frames - diagnostics_.lastAnchorFrame;
    diagnostics_.maximumReferenceAge =
        std::max(diagnostics_.maximumReferenceAge, diagnostics_.referenceAge);
    if (source.isNull() || (!previous_.isNull() && source.size() != previous_.size()))
        return skip(ScrollSkipReason::InvalidFrame);
    QImage frame = source.convertToFormat(QImage::Format_RGB32);
    frame.setDevicePixelRatio(1);
    if (frame.isNull())
        return skip(ScrollSkipReason::LowConfidence);
    QImage gray = grayFrame(frame);
    if (gray.isNull())
        return skip(ScrollSkipReason::LowConfidence);
    QImage coarse = coarseFrame(gray);
    if (coarse.isNull())
        return skip(ScrollSkipReason::LowConfidence);
    if (previous_.isNull())
    {
        anchor(frame, gray, coarse, 0);
        initializeContent(frame, ScrollAxis::Vertical, 0, 0);
        if (canvas_.isNull())
        {
            stats_.limitReached = true;
            skip(ScrollSkipReason::LowConfidence);
            return ScrollFrameOutcome::LimitReached;
        }
        pendingTail_ = false;
        skipping_ = false;
        ++stats_.accepted;
        stats_.lastFrameReliable = true;
        stats_.limitReached = stats_.lengthPx < frame.height() || stats_.lengthPx >= lengthLimit();
        return stats_.limitReached ? ScrollFrameOutcome::LimitReached
                                   : ScrollFrameOutcome::Accepted;
    }
    // 重复帧不构成质量警告；不刷新接受基准，微小逐步运动可累积到可测量位移。
    const double zeroError = difference(previousGray_, gray, stats_.axis, 0, 0, 0, true);
    diagnostics_.zeroMotionMargin.add(staticTolerance - zeroError);
    if (referencePosition_ == currentPosition_ && zeroError <= staticTolerance)
        return skip(ScrollSkipReason::NoMotion);
    if (referencePosition_ == currentPosition_ && zeroError <= smoothMaximumError &&
        hasStableSupport(gray, stats_.axis, 0))
    {
        anchor(std::move(frame), std::move(gray), std::move(coarse), currentPosition_);
        ++diagnostics_.contentAnchors;
        return skip(ScrollSkipReason::ContentChange);
    }
    Match motion = matchFrames(previous_, frame, previousGray_, gray, previousCoarse_, coarse,
                               stats_.axis, leading_, trailing_, stats_.motionDetected,
                               previousMotion_, currentPosition_ - referencePosition_);
    bool strictPixelMatch = motion.exact;
    // 局部动画可能盖住中部模板：四条独立窄带中至少两条同意，且整幅有同相纹理支持。
    if (!motion.reliable)
    {
        QVector<Match> votes;
        for (int strip = 0; strip < 6; ++strip)
        {
            const int width = breadth(frame, stats_.axis);
            QRect region = stats_.axis == ScrollAxis::Vertical
                               ? QRect(strip * width / 4, 0, width / 4, frame.height())
                               : QRect(0, strip * width / 4, frame.width(), width / 4);
            if (strip >= 4)
            {
                const int n = length(frame, stats_.axis);
                region = band(frame, stats_.axis, strip == 4 ? 0 : n * 3 / 4, n / 4);
            }
            const QImage a = previousGray_.copy(region), b = gray.copy(region);
            const Match vote = matchFrames(a, b, a, b, coarseFrame(a), coarseFrame(b), stats_.axis,
                                           strip < 4 ? leading_ : 0, strip < 4 ? trailing_ : 0,
                                           stats_.motionDetected, previousMotion_,
                                           currentPosition_ - referencePosition_);
            if (!vote.reliable || !vote.exact || std::abs(vote.delta) > motion.range)
                continue;
            if (std::any_of(votes.begin(), votes.end(),
                            [&](const Match& other) { return other.delta == vote.delta; }) &&
                hasStableSupport(gray, stats_.axis, vote.delta))
            {
                const int leading = motion.leading, trailing = motion.trailing,
                          range = motion.range;
                motion = vote;
                motion.leading = leading;
                motion.trailing = trailing;
                motion.range = range;
                break;
            }
            votes.append(vote);
        }
    }
    if (motion.reliable && !motion.exact && hasStableSupport(gray, stats_.axis, motion.delta))
        motion.exact = true;
    const ScrollAxis otherAxis =
        stats_.axis == ScrollAxis::Vertical ? ScrollAxis::Horizontal : ScrollAxis::Vertical;
    const Match orthogonal =
        !stats_.motionDetected || !motion.reliable
            ? matchFrames(previous_, frame, previousGray_, gray, previousCoarse_, coarse, otherAxis,
                          0, 0, false, 0, 0)
            : Match{};
    // 诊断记录实际可能选中的轴，不把首次横向匹配写成失败的竖向数据。
    const Match& measured = !stats_.motionDetected && orthogonal.reliable && orthogonal.delta &&
                                    (!motion.reliable || orthogonal.error + 1.0 < motion.error)
                                ? orthogonal
                                : motion;
    recordMatch(measured.delta, measured.range, measured.error);
    diagnostics_.separationMargin.add(measured.separationMargin);
    diagnostics_.continuityMargin.add(measured.continuityMargin);
    diagnostics_.diagonalMargin.add(measured.diagonalMargin);
    diagnostics_.smoothMargin.add(measured.smoothMargin);
    diagnostics_.referenceMargin.add(measured.referenceMargin);
    // 亚像素运动的最佳整数位移仍为零时保留旧基准，运动可在后续帧累积。
    if (motion.reliable && !motion.delta && referencePosition_ == currentPosition_ &&
        (!orthogonal.reliable || !orthogonal.delta))
    {
        diagnostics_.subpixelHoldMargin.add(
            std::min({motion.separationMargin, motion.continuityMargin, motion.smoothMargin}));
        ++diagnostics_.subpixelHolds;
        return skip(ScrollSkipReason::NoMotion);
    }
    // 能在较短重叠中定位、却超出安全范围，才确认 range 缺口；其余只挂起尾部。
    if (!motion.reliable && !orthogonal.reliable)
    {
        const Match outside =
            matchFrames(previous_, frame, previousGray_, gray, previousCoarse_, coarse, stats_.axis,
                        leading_, trailing_, stats_.motionDetected, previousMotion_,
                        currentPosition_ - referencePosition_, true);
        if (outside.reliable && outside.exact && std::abs(outside.delta) > motion.range)
            return skip(ScrollSkipReason::RangeExhausted);
    }
    if (!stats_.motionDetected)
    {
        ScrollAxis chosenAxis = stats_.axis;
        if (orthogonal.reliable && orthogonal.delta &&
            (!motion.reliable || orthogonal.error + 1.0 < motion.error))
        {
            motion = orthogonal;
            strictPixelMatch = motion.exact;
            chosenAxis = otherAxis;
        }
        else if (orthogonal.reliable && orthogonal.delta && motion.reliable && motion.delta)
            return skip(ScrollSkipReason::LowConfidence);
        if (!motion.reliable)
            return skip(ScrollSkipReason::LowConfidence);
        if (chosenAxis != stats_.axis || motion.leading != leading_ || motion.trailing != trailing_)
            initializeContent(previous_, chosenAxis, motion.leading, motion.trailing);
        if (canvas_.isNull())
        {
            stats_.limitReached = true;
            skip(ScrollSkipReason::LowConfidence);
            return ScrollFrameOutcome::LimitReached;
        }
        stats_.motionDetected = true;
    }
    else if (orthogonal.reliable && orthogonal.delta && !motion.reliable)
    {
        skip(ScrollSkipReason::AxisChange);
        return ScrollFrameOutcome::AxisLocked;
    }
    if (!motion.reliable)
        return skip(ScrollSkipReason::LowConfidence);
    const int contentLength = length(frame, stats_.axis) - leading_ - trailing_;
    const int position = referencePosition_ + motion.delta;
    if (position == currentPosition_)
    {
        if (motion.exact)
        {
            anchor(std::move(frame), std::move(gray), std::move(coarse), position);
            ++diagnostics_.contentAnchors;
        }
        return skip(ScrollSkipReason::NoMotion);
    }
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
        return skip(ScrollSkipReason::LowConfidence);
    stats_.lastMotionPx = position - currentPosition_;
    currentPosition_ = position;
    previousMotion_ = stats_.lastMotionPx;
    // 软匹配逐帧取整会累积漂移；保持基准直到相位一致，预留至少两步重叠余量。
    if (motion.exact ||
        std::abs(motion.delta) >= motion.range - std::max(32, std::abs(stats_.lastMotionPx) * 2))
    {
        anchor(std::move(frame), std::move(gray), std::move(coarse), position);
        if (motion.exact)
        {
            ++diagnostics_.exactAnchors;
            if (strictPixelMatch)
                ++diagnostics_.strictPixelAnchors;
            else
                ++diagnostics_.stableSupportAnchors;
        }
        else
            ++diagnostics_.rangeAnchors;
    }
    diagnostics_.displacement.add(std::abs(stats_.lastMotionPx));
    pendingTail_ = false;
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
    finalize();
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
    previousCoarse_ = {};
    return image;
}
QImage ScrollStitcher::result() const
{
    return canvas_.copy(band(canvas_, stats_.axis, 0, int(stats_.lengthPx)));
}
}
