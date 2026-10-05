#include "interfaces/sticker_window_behavior.h"
#include <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>
namespace waibusnap
{
namespace
{
class CocoaStickerFocusSession final : public StickerFocusSession
{
  public:
    CocoaStickerFocusSession() : previous_([NSWorkspace sharedWorkspace].frontmostApplication) {}
    ~CocoaStickerFocusSession() override
    {
        if (previous_ &&
            previous_.processIdentifier !=
                NSRunningApplication.currentApplication.processIdentifier &&
            [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier ==
                NSRunningApplication.currentApplication.processIdentifier)
        {
            [NSApp yieldActivationToApplication:previous_];
            [previous_ activateWithOptions:0];
        }
    }

  private:
    NSRunningApplication* previous_;
};
}
bool configureStickerWindowBehavior(QWindow* window, bool editing)
{
    if (!window)
        return false;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return true;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    if (editing)
        return native && !(native.styleMask & NSWindowStyleMaskNonactivatingPanel);
    if (![native isKindOfClass:[NSPanel class]])
        return false;
    native.styleMask |= NSWindowStyleMaskNonactivatingPanel;
    native.hidesOnDeactivate = NO;
    return (native.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0;
}
std::unique_ptr<StickerFocusSession> activateStickerEditing(QWindow* window)
{
    if (!window || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return {};
    auto session = std::make_unique<CocoaStickerFocusSession>();
    // 编辑入口是明确的用户动作，允许从前台应用取得输入，而非仅请求协作激活。
    [NSApp activateIgnoringOtherApps:YES];
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    [view.window makeKeyAndOrderFront:nil];
    return session;
}
}
