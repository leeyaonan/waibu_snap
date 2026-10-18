#include "interfaces/window_visibility.h"
#include <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <QGuiApplication>
#include <QWindow>
namespace waibusnap
{
WindowVisibility queryWindowVisibility(QWindow* window)
{
    WindowVisibility result;
    if (!window || !window->handle() || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return result;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    if (!native || native.windowNumber <= 0)
        return result;
    result.nativeVisible = native.isVisible ? 1 : 0;
    result.hidesOnDeactivate = native.hidesOnDeactivate ? 1 : 0;
    result.windowNumber = quint64(native.windowNumber);
    result.isOnActiveSpace = native.isOnActiveSpace ? 1 : 0;
    result.occlusionState = qint64(native.occlusionState);
    result.windowLevel = qint64(native.level);
    result.collectionBehavior = quint64(native.collectionBehavior);
    // 自有窗口的编号 / 在屏属性不依赖其他应用的标题或截图权限。
    NSArray* list = CFBridgingRelease(
        CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
    if (!list)
        return result;
    result.available = true;
    for (NSDictionary* info in list)
        if ([info[(__bridge NSString*)kCGWindowNumber] unsignedLongLongValue] ==
                result.windowNumber &&
            [info[(__bridge NSString*)kCGWindowOwnerPID] intValue] ==
                NSProcessInfo.processInfo.processIdentifier)
        {
            result.onScreen = true;
            break;
        }
    return result;
}
}
