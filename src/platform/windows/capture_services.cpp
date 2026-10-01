#include "interfaces/capture_provider.h"
#include "interfaces/global_hotkey.h"
#include "interfaces/presentation_observer.h"
#include <QTimer>
namespace waibusnap
{
namespace
{
class WindowsHotkey final : public GlobalHotkey
{
  public:
    HotkeyRegistration registerF1(std::function<void()>) override
    {
        return {false, QStringLiteral("Windows 全局热键尚未实现，请使用托盘入口。")};
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
std::unique_ptr<GlobalHotkey> createGlobalHotkey() { return std::make_unique<WindowsHotkey>(); }
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
}
