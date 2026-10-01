#pragma once
#include "interfaces/display_topology.h"
#include <QImage>
#include <functional>
#include <memory>
namespace waibusnap
{
enum class CaptureError
{
    None,
    Permission,
    Unavailable,
    DisplayChanged,
    NotImplemented
};
struct CaptureFrame
{
    DisplayTarget display;
    QImage pixels;
    qint64 capturedAtNs = 0;
};
struct CaptureResult
{
    CaptureFrame frame;
    CaptureError error = CaptureError::None;
    QString explanation;
};
class CaptureProvider
{
  public:
    virtual ~CaptureProvider() = default;
    virtual bool hasPermission() const = 0;
    virtual bool requestPermission() = 0;
    // 异步结果在 GUI 线程交付；取消会话后调用方仍须防止迟到回调。
    virtual void capture(const DisplayTarget& display,
                         std::function<void(CaptureResult)> completion) = 0;
};
std::unique_ptr<CaptureProvider> createCaptureProvider();
}
