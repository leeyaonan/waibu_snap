#include "interfaces/scroll_injector.h"
#import <ApplicationServices/ApplicationServices.h>
#include <QDesktopServices>
#include <QUrl>
namespace waibusnap
{
namespace
{
class MacScrollInjector final : public ScrollInjector
{
  public:
    bool hasAccessibilityPermission() const override { return AXIsProcessTrusted(); }
    void requestAccessibilityPermission() override
    {
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")));
    }
    bool postScroll(ScrollInjection request) override
    {
        if (!hasAccessibilityPermission())
            return false;
        CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2,
                                                         request.dy, request.dx);
        if (!event)
            return false;
        // Qt 与 CG 均以主屏左上为逻辑坐标原点，负坐标副屏无需 DPR 再缩放。
        CGEventSetLocation(event, CGPointMake(request.globalPoint.x(), request.globalPoint.y()));
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        return true;
    }
};
}
std::unique_ptr<ScrollInjector> createScrollInjector()
{
    return std::make_unique<MacScrollInjector>();
}
}
