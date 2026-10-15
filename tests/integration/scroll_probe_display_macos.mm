#include "interfaces/display_topology.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <QGuiApplication>
#include <QScreen>
// 仅测试入口枚举屏幕，生产仍由光标决定触发屏；版本令牌沿用真实平台提供器。
waibusnap::DisplayResult scrollProbeDisplay(int index)
{
    auto result = waibusnap::createDisplayTopology()->cursorDisplay();
    const auto screens = QGuiApplication::screens();
    if (!result.error.isEmpty() || index < 0 || index >= screens.size())
        return {{}, QStringLiteral("屏幕序号无效或无法取得布局令牌。")};
    const auto* chosen = screens[index];
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    for (NSScreen* screen in NSScreen.screens)
    {
        const NSRect frame = screen.frame;
        const QRect geometry(qRound(frame.origin.x), qRound(top - NSMaxY(frame)),
                             qRound(frame.size.width), qRound(frame.size.height));
        if (geometry != chosen->geometry())
            continue;
        result.target.id = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        result.target.logicalGeometry = geometry;
        result.target.devicePixelRatio = screen.backingScaleFactor;
        result.target.availableLogicalGeometry = chosen->availableGeometry();
        return result;
    }
    return {{}, QStringLiteral("系统与 Qt 屏幕不匹配。")};
}

// 仅在取证命令明确指定浏览器时置前；不改变生产采集的焦点策略。
bool scrollProbeActivate(const QString& bundleId)
{
    if (bundleId != QStringLiteral("com.google.Chrome") &&
        bundleId != QStringLiteral("com.apple.Safari"))
        return false;
    for (NSRunningApplication* browser in
         [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleId.toNSString()])
        return [browser activateFromApplication:NSRunningApplication.currentApplication
                                        options:NSApplicationActivateAllWindows];
    return false;
}
