#pragma once
#include "interfaces/display_topology.h"
#include <QImage>
#include <QVector>
#include <functional>
class QWindow;
namespace waibusnap
{
struct RegionCaptureRequest
{
    DisplayTarget display;
    QRect pixelRect;
    QVector<quint64> excludedWindowIds;
};
class RegionCaptureOnce
{
  public:
    virtual ~RegionCaptureOnce() = default;
    // GUI 线程发起与交付；析构后不得调用完成回调。
    virtual void capture(const RegionCaptureRequest&,
                         std::function<void(QImage, QString)> completion) = 0;
};
std::unique_ptr<RegionCaptureOnce> createRegionCaptureOnce();
class RegionCaptureSession
{
  public:
    virtual ~RegionCaptureSession() = default;
    // GUI 线程启动 / 停止与交付；停止后不得调用用户回调，也不得继续请求帧。
    virtual void start(std::function<void(QImage)> onFrame,
                       std::function<void(QString)> onError) = 0;
    virtual void stop() = 0;
};
// 在冻结覆盖层抢焦点前记住目标应用；采集开始时按需把交互交还该应用。
std::function<void()> captureForegroundRestorer();
bool regionCaptureSupported();
quint64 regionCaptureWindowId(QWindow* window);
std::unique_ptr<RegionCaptureSession> createRegionCaptureSession(RegionCaptureRequest request);
}
