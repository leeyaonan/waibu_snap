#pragma once
#include <QImage>
#include <array>
#include <limits>
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
    // 既有字段仍表示物理像素；显示器入口统一由逻辑预算换算。
    qint64 maxLengthPx = 20000;
    qint64 maxAreaPx = 30000000;
    qreal devicePixelRatio = 1;
    qint64 workingSetBytes = 120000000;
    static ScrollStitchLimits forDisplay(qreal dpr, qint64 logicalLength = 12000,
                                         qint64 logicalArea = 6000000);
};
enum class ScrollFrameOutcome
{
    Accepted,
    Skipped,
    LimitReached,
    AxisLocked
};
enum class ScrollSkipReason
{
    None,
    NoMotion,
    ContentChange,
    LowConfidence,
    RangeExhausted,
    AxisChange,
    InvalidFrame
};
// 仅诊断使用：固定桶避免长会话日志 / 内存随帧数增长。
struct ScrollDistribution
{
    qint64 count = 0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    double sum = 0;
    // <0、[0,.1)、[.1,.5)、[.5,1)、[1,2)、[2,4)、[4,8)、[8,16)、[16,32)、>=32。
    std::array<qint64, 10> bins{};
    void add(double value);
};
struct ScrollStitchDiagnostics
{
    ScrollDistribution displacement;
    ScrollDistribution separationMargin;
    ScrollDistribution continuityMargin;
    ScrollDistribution diagonalMargin;
    ScrollDistribution smoothMargin;
    ScrollDistribution referenceMargin;
    ScrollDistribution zeroMotionMargin;
    ScrollDistribution subpixelHoldMargin;
    int exactAnchors = 0;
    int strictPixelAnchors = 0;
    int stableSupportAnchors = 0;
    int rangeAnchors = 0;
    int contentAnchors = 0;
    int subpixelHolds = 0;
    int lastAnchorFrame = 0;
    int referenceAge = 0;
    int maximumReferenceAge = 0;
    int lastDelta = 0;
    int lastRange = 0;
    double lastError = 0;
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
    int hiccups = 0;
    int lowConfidence = 0;
    int gaps = 0;
    int rangeExceededFrames = 0;
    int axisChanges = 0;
    int invalidFrames = 0;
    int unverifiedTails = 0;
    int interruptions = 0;
    int noMotion = 0;
    ScrollSkipReason lastSkipReason = ScrollSkipReason::None;
    qreal devicePixelRatio = 1;
    bool memoryLimited = false;
    qint64 logicalLength() const { return qRound64(lengthPx / devicePixelRatio); }
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
    // 结束时才确认尚未恢复的尾部风险；重复调用不重复记账。
    void finalize();
    const ScrollStitchDiagnostics& diagnostics() const { return diagnostics_; }

  private:
    void initializeContent(const QImage& frame, ScrollAxis axis, int leading, int trailing);
    bool extend(const QImage& frame, int from, int count, bool prepend);
    ScrollFrameOutcome skip(ScrollSkipReason reason);
    void anchor(QImage frame, QImage gray, QImage coarse, int position);
    void recordMatch(int delta, int range, double error);
    bool hasStableSupport(const QImage& gray, ScrollAxis axis, int delta) const;
    qint64 canvasAreaBudget() const;
    ScrollStitchLimits limits_;
    ScrollStitchStats stats_;
    ScrollStitchDiagnostics diagnostics_;
    QImage previous_;
    QImage previousGray_;
    QImage previousCoarse_;
    QImage canvas_;
    int leading_ = 0;
    int trailing_ = 0;
    int currentPosition_ = 0;
    int referencePosition_ = 0;
    int minimumPosition_ = 0;
    int maximumPosition_ = 0;
    int previousMotion_ = 0;
    bool skipping_ = false;
    bool pendingTail_ = false;
    bool finalized_ = false;
};
}
