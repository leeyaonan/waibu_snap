#pragma once
#include "core/scroll_stitcher.h"
#include "interfaces/region_capture.h"
#include "interfaces/scroll_injector.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
namespace waibusnap
{
class ScrollCaptureSession final : public QObject
{
    Q_OBJECT
  public:
    explicit ScrollCaptureSession(RegionCaptureRequest request, QRect controlBar,
                                  QObject* parent = nullptr);
    ~ScrollCaptureSession() override;
    void start();
    void stop();
    void complete();
    void interrupt(const QString& explanation);
    void setAutomatic(bool enabled);
    void cycleSpeed();
    void cycleDirection();
    void setControlBar(QRect rectangle) { controlBar_ = rectangle; }
    void openAccessibilitySettings();
    bool automatic() const { return automatic_; }
    int speed() const { return speed_; }
    int direction() const { return direction_; }
    const ScrollStitchStats& stats() const { return stitcher_.stats(); }
    qint64 lengthLimit() const { return stitcher_.lengthLimit(); }
    QImage takeResult() { return stitcher_.takeResult(); }
  signals:
    void statisticsChanged(waibusnap::ScrollStitchStats stats);
    void noticeChanged(QString text);
    void automaticChanged(bool enabled);
    void optionsChanged(int speed, int direction);
    void permissionMissing();
    void completed();

  private:
    friend struct ScrollCaptureTestAccess;
    ScrollCaptureSession(RegionCaptureRequest request, QRect controlBar,
                         std::unique_ptr<RegionCaptureSession> capture,
                         std::unique_ptr<ScrollInjector> injector, std::function<QPoint()> cursor,
                         ScrollStitchLimits limits, QObject* parent);
    void frameArrived(QImage frame);
    void driveAutomatic();
    RegionCaptureRequest request_;
    QRect controlBar_;
    std::unique_ptr<RegionCaptureSession> capture_;
    std::unique_ptr<ScrollInjector> injector_;
    std::function<QPoint()> cursor_;
    ScrollStitcher stitcher_;
    QTimer autoTimer_;
    QTimer frameTimeout_;
    QElapsedTimer elapsed_;
    void logDiagnostics() const;
    ScrollDistribution arrivalIntervals_;
    ScrollDistribution processingTimes_;
    qint64 lastArrivalNs_ = -1;
    qint64 firstArrivalNs_ = -1;
    int postedEvents_ = 0;
    int suspensions_ = 0;
    bool trace_ = false;
    qint64 lastMotionMs_ = 0;
    qint64 lastRefreshMs_ = -500;
    int speed_ = 1;
    int direction_ = 0;
    int probeSign_ = -1;
    int probeCount_ = 0;
    int stationaryPosts_ = 0;
    int stationaryDistance_ = 0;
    int motionSerial_ = 0;
    int postMotionSerial_ = 0;
    int reliableSerial_ = 0;
    int postReliableSerial_ = 0;
    int postFrames_ = 0;
    int postedDistance_ = 0;
    bool postPending_ = false;
    bool active_ = false;
    bool automatic_ = false;
    bool suspended_ = false;
    bool axisNoticeShown_ = false;
    bool bottomNoticeShown_ = false;
};
}
Q_DECLARE_METATYPE(waibusnap::ScrollStitchStats)
