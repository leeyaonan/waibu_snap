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
}
