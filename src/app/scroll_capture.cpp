#include "app/scroll_capture.h"
#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QWindow>
namespace waibusnap
{
ScrollCaptureSession::ScrollCaptureSession(RegionCaptureRequest request, QRect controlBar,
                                           QObject* parent)
    : ScrollCaptureSession(
          request, controlBar, createRegionCaptureSession(request), createScrollInjector(),
          [] { return QCursor::pos(); }, {}, parent)
{
}
ScrollCaptureSession::ScrollCaptureSession(RegionCaptureRequest request, QRect controlBar,
                                           std::unique_ptr<RegionCaptureSession> capture,
                                           std::unique_ptr<ScrollInjector> injector,
                                           std::function<QPoint()> cursor,
                                           ScrollStitchLimits limits, QObject* parent)
    : QObject(parent), request_(std::move(request)), controlBar_(controlBar),
      capture_(std::move(capture)), injector_(std::move(injector)), cursor_(std::move(cursor)),
      stitcher_(limits)
{
    autoTimer_.setInterval(100);
    connect(&autoTimer_, &QTimer::timeout, this, &ScrollCaptureSession::driveAutomatic);
    frameTimeout_.setSingleShot(true);
    connect(&frameTimeout_, &QTimer::timeout, this,
            [this] { interrupt(QStringLiteral("采集已中断：持续未收到画面，已保留当前结果。")); });
}
ScrollCaptureSession::~ScrollCaptureSession() { stop(); }
void ScrollCaptureSession::start()
{
    if (active_)
        return;
    active_ = true;
    elapsed_.start();
    lastMotionMs_ = 0;
    qInfo("截长图开始采集：模式=手动");
    frameTimeout_.start(5000);
    if (!capture_)
    {
        interrupt(QStringLiteral("截长图暂不支持当前平台（Windows 后补）。"));
        return;
    }
    QPointer<ScrollCaptureSession> self(this);
    capture_->start(
        [self](QImage image)
        {
            if (self)
                self->frameArrived(std::move(image));
        },
        [self](QString error)
        {
            if (self)
                self->interrupt(error);
        });
}
void ScrollCaptureSession::stop()
{
    active_ = false;
    automatic_ = false;
    autoTimer_.stop();
    frameTimeout_.stop();
    postPending_ = false;
    if (capture_)
        capture_->stop();
}
void ScrollCaptureSession::complete()
{
    if (!active_)
        return;
    stop();
    emit completed();
}
void ScrollCaptureSession::interrupt(const QString& explanation)
{
    if (!active_)
        return;
    stitcher_.addWarning();
    qWarning().noquote() << "截长图采集中断：" << explanation;
    emit noticeChanged(explanation);
    complete();
}
void ScrollCaptureSession::frameArrived(QImage image)
{
    if (!active_)
        return;
    frameTimeout_.start(5000);
    const int warningsBefore = stitcher_.stats().warnings;
    const auto outcome = stitcher_.addFrame(image);
    if (stitcher_.stats().lastFrameReliable)
        ++reliableSerial_;
    if (stitcher_.stats().lastMotionPx)
    {
        ++motionSerial_;
        lastMotionMs_ = elapsed_.elapsed();
        bottomNoticeShown_ = false;
        if (!suspended_)
            emit noticeChanged({});
    }
    else if (!automatic_ && stitcher_.stats().motionDetected &&
             stitcher_.stats().lastFrameReliable && elapsed_.elapsed() - lastMotionMs_ >= 2500 &&
             !bottomNoticeShown_)
    {
        bottomNoticeShown_ = true;
        emit noticeChanged(QStringLiteral("似乎已滚动到底部；点击“完成”结束，或继续滚动。"));
    }
    if (outcome == ScrollFrameOutcome::AxisLocked && !axisNoticeShown_ && !suspended_)
    {
        axisNoticeShown_ = true;
        emit noticeChanged(QStringLiteral("检测到另一方向移动，已忽略；如需更换方向请重新开始。"));
    }
    else if (outcome == ScrollFrameOutcome::Skipped && !stitcher_.stats().lastFrameReliable &&
             stitcher_.stats().warnings > warningsBefore && !suspended_)
        // 匹配失败无法区分快速缺口与内容变化，不据此声称滚动速度过快。
        emit noticeChanged(QStringLiteral("画面重叠不足或内容变化，部分帧未拼接。"));
    if (elapsed_.elapsed() - lastRefreshMs_ >= 500 || outcome == ScrollFrameOutcome::LimitReached)
    {
        lastRefreshMs_ = elapsed_.elapsed();
        emit statisticsChanged(stitcher_.stats());
    }
    if (outcome == ScrollFrameOutcome::LimitReached)
    {
        emit noticeChanged(QStringLiteral("已达长度上限，已保留当前结果。"));
        complete();
    }
}
void ScrollCaptureSession::setAutomatic(bool enabled)
{
    if (!active_)
        return;
    if (enabled && (!injector_ || !injector_->hasAccessibilityPermission()))
    {
        automatic_ = false;
        autoTimer_.stop();
        postPending_ = false;
        qInfo("截长图自动滚动：辅助功能未授权，已提示");
        emit automaticChanged(false);
        emit permissionMissing();
        return;
    }
    automatic_ = enabled;
    suspended_ = false;
    stationaryPosts_ = 0;
    stationaryDistance_ = 0;
    postPending_ = false;
    probeCount_ = 0;
    probeSign_ = -1;
    emit automaticChanged(enabled);
    if (enabled)
    {
        qInfo("截长图自动滚动：辅助功能已授权，开始投递");
        autoTimer_.start();
    }
    else
    {
        autoTimer_.stop();
        emit noticeChanged({});
    }
}
void ScrollCaptureSession::cycleSpeed()
{
    speed_ = (speed_ + 1) % 3;
    emit optionsChanged(speed_, direction_);
}
void ScrollCaptureSession::cycleDirection()
{
    direction_ = (direction_ + 1) % 3;
    probeSign_ = -1;
    probeCount_ = 0;
    stationaryPosts_ = 0;
    stationaryDistance_ = 0;
    postPending_ = false;
    emit optionsChanged(speed_, direction_);
}
void ScrollCaptureSession::openAccessibilitySettings()
{
    if (injector_)
        injector_->requestAccessibilityPermission();
}
void ScrollCaptureSession::driveAutomatic()
{
    if (!active_ || !automatic_)
        return;
    if (!injector_->hasAccessibilityPermission())
    {
        setAutomatic(false);
        emit noticeChanged(QStringLiteral("辅助功能权限已撤销，已切回手动滚动。"));
        return;
    }
    const QPoint cursor = cursor_();
    const QPointF local = (QPointF(cursor) - request_.display.logicalGeometry.topLeft()) *
                          request_.display.devicePixelRatio;
    bool ownWindow = controlBar_.contains(cursor);
    for (QWindow* window : QGuiApplication::topLevelWindows())
        ownWindow |= window->isVisible() &&
                     !window->flags().testFlag(Qt::WindowTransparentForInput) &&
                     window->geometry().contains(cursor);
    if (!QRectF(request_.pixelRect).contains(local) || ownWindow)
    {
        if (!suspended_)
            emit noticeChanged(QStringLiteral("自动滚动暂挂：请将鼠标移到采集区域内。"));
        suspended_ = true;
        postPending_ = false;
        stationaryPosts_ = 0;
        stationaryDistance_ = 0;
        return;
    }
    if (suspended_)
        emit noticeChanged({});
    suspended_ = false;
    if (postPending_)
    {
        // 尚未有采集响应时绝不继续累积投递，也不把缺帧 / 低置信帧当作到底。
        if (stats().frames <= postFrames_)
            return;
        const bool moved = motionSerial_ != postMotionSerial_;
        if (moved)
        {
            stationaryPosts_ = 0;
            stationaryDistance_ = 0;
            probeCount_ = 0;
        }
        else if (reliableSerial_ != postReliableSerial_)
        {
            if (stats().motionDetected)
            {
                ++stationaryPosts_;
                stationaryDistance_ += postedDistance_;
                if (stationaryPosts_ >= 2 && stationaryDistance_ >= 60)
                {
                    complete();
                    return;
                }
            }
            else if (direction_ == 0)
            {
                ++probeCount_;
                if (probeCount_ == 5)
                    probeSign_ = 1;
                else if (probeCount_ >= 10)
                {
                    emit noticeChanged(QStringLiteral(
                        "未检测到滚动，请确认光标位于可滚动内容上，或手动选择方向。"));
                    probeCount_ = 0;
                    probeSign_ = -1;
                }
            }
        }
        else
        {
            stationaryPosts_ = 0;
            stationaryDistance_ = 0;
        }
    }
    const int steps[] = {30, 60, 120};
    const int step = steps[speed_];
    ScrollAxis axis = direction_ == 2          ? ScrollAxis::Horizontal
                      : direction_ == 1        ? ScrollAxis::Vertical
                      : stats().motionDetected ? stats().axis
                                               : ScrollAxis::Vertical;
    const int delta = step * (direction_ == 0 ? probeSign_ : -1);
    if (!injector_->postScroll({cursor, axis == ScrollAxis::Horizontal ? delta : 0,
                                axis == ScrollAxis::Vertical ? delta : 0}))
    {
        setAutomatic(false);
        emit noticeChanged(QStringLiteral("自动滚动投递失败，已切回手动滚动。"));
        return;
    }
    postPending_ = true;
    postedDistance_ = step;
    postFrames_ = stats().frames;
    postMotionSerial_ = motionSerial_;
    postReliableSerial_ = reliableSerial_;
}
}
