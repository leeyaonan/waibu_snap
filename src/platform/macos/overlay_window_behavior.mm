#include "interfaces/overlay_window_behavior.h"
#include <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>
namespace waibusnap
{
void raiseOverlayAboveSystemChrome(QWindow* window)
{
    if (!window || !window->handle() || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    if (native)
        [native setLevel:NSStatusWindowLevel];
}
void lowerOverlayBelowSystemPanels(QWindow* window)
{
    if (!window || !window->handle() || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    // 独立系统面板低于抬升后的覆盖层；临时降级，面板结束后必须配对抬升恢复。
    if (native)
        [native setLevel:NSNormalWindowLevel];
}
}
