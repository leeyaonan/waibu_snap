#include "interfaces/sticker_window_behavior.h"
#include <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>
namespace waibusnap
{
bool configureStickerWindowBehavior(QWindow* window)
{
    if (!window)
        return false;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return true;
    // Qt Tool 在 Cocoa 上是 NSPanel。拒绝成为 key window 并不阻止应用激活，
    // 增加非激活面板样式，点击 / 拖动仍将键盘交给前台应用。
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    if (![native isKindOfClass:[NSPanel class]])
        return false;
    native.styleMask |= NSWindowStyleMaskNonactivatingPanel;
    native.hidesOnDeactivate = NO;
    return (native.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0;
}
}
