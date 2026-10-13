#pragma once
#include <QImage>
#include <optional>
namespace waibusnap
{
enum class ScrollAxis
{
    Vertical,
    Horizontal
};
struct ScrollStitchLimits
{
    qint64 maxLengthPx = 20000;
    qint64 maxAreaPx = 30000000;
};
enum class ScrollFrameOutcome
{
    Accepted,
    Skipped,
    LimitReached,
    AxisLocked
};
struct ScrollStitchStats
{
    int frames = 0;
    int accepted = 0;
    int skipped = 0;
    int warnings = 0;
    qint64 lengthPx = 0;
    ScrollAxis axis = ScrollAxis::Vertical;
    bool motionDetected = false;
    bool limitReached = false;
    // 位移与增长分开：在已采区域内往返仍是可靠运动，但不重复拼入内容。
    int lastMotionPx = 0;
    bool lastFrameReliable = false;
};
class ScrollStitcher
{
  public:
    explicit ScrollStitcher(ScrollStitchLimits limits = {});
    ScrollFrameOutcome addFrame(const QImage& frame);
    QImage result() const;
    // 定稿交出容量所有权，避免再复制一张完整长图。此后不得继续采集。
    QImage takeResult();
    const ScrollStitchStats& stats() const { return stats_; }
    qint64 lengthLimit() const;
    void addWarning();

  private:
    void initializeContent(const QImage& frame, ScrollAxis axis, int leading, int trailing);
    bool extend(const QImage& frame, int from, int count, bool prepend);
    ScrollFrameOutcome skip(bool warning);
    ScrollStitchLimits limits_;
    ScrollStitchStats stats_;
    QImage previous_;
    QImage previousGray_;
    QImage canvas_;
    int leading_ = 0;
    int trailing_ = 0;
    int currentPosition_ = 0;
    int minimumPosition_ = 0;
    int maximumPosition_ = 0;
    int previousMotion_ = 0;
    bool skipping_ = false;
};
}
