#include "interfaces/capture_provider.h"
#include "interfaces/presentation_observer.h"
#include "interfaces/region_capture.h"
#include "interfaces/window_enumerator.h"
#include <QObject>
#include <QTimer>
namespace waibusnap
{
namespace
{
class WindowsWindowEnumerator final : public WindowEnumerator
{
  public:
    WindowListResult visibleWindows(const DisplayTarget&) const override
    {
        return {{}, QStringLiteral("窗口吸附尚未实现，请手动框选")};
    }
};
class WindowsDisplays final : public DisplayTopology
{
  public:
    DisplayResult cursorDisplay() const override
    {
        return {{}, QStringLiteral("Windows 光标显示器定位尚未实现。")};
    }
    bool stillMatches(const DisplayTarget&) const override { return false; }
};
class WindowsRegionCaptureOnce final : public RegionCaptureOnce
{
  public:
    void capture(const RegionCaptureRequest&,
                 std::function<void(QImage, QString)> completion) override
    {
        QTimer::singleShot(0, &context_, [completion = std::move(completion)]
                           { completion({}, QStringLiteral("Windows 原生区域采集尚未实现")); });
    }

  private:
    QObject context_;
};
class WindowsCapture final : public CaptureProvider
{
  public:
    bool hasPermission() const override { return false; }
    bool requestPermission() override { return false; }
    void capture(const DisplayTarget&, std::function<void(CaptureResult)> completion) override
    {
        QTimer::singleShot(0,
                           [completion = std::move(completion)] {
                               completion({{},
                                           CaptureError::NotImplemented,
                                           QStringLiteral("Windows 单帧捕获尚未实现。")});
                           });
    }
};
}
std::unique_ptr<WindowEnumerator> createWindowEnumerator()
{
    return std::make_unique<WindowsWindowEnumerator>();
}
std::unique_ptr<DisplayTopology> createDisplayTopology()
{
    return std::make_unique<WindowsDisplays>();
}
std::unique_ptr<CaptureProvider> createCaptureProvider()
{
    return std::make_unique<WindowsCapture>();
}
std::unique_ptr<PresentationObservation>
observePresentation(QWindow*, QObject* context, std::function<void(PresentationSample)> completion)
{
    QTimer::singleShot(0, context, [completion = std::move(completion)] { completion({}); });
    return {};
}
std::function<void()> captureForegroundRestorer() { return {}; }
bool regionCaptureSupported() { return false; }
quint64 regionCaptureWindowId(QWindow*) { return 0; }
std::unique_ptr<RegionCaptureOnce> createRegionCaptureOnce()
{
    return std::make_unique<WindowsRegionCaptureOnce>();
}
std::unique_ptr<RegionCaptureSession> createRegionCaptureSession(RegionCaptureRequest)
{
    return {};
}

}
