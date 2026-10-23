#include "interfaces/sticker_window_behavior.h"
#include <AppKit/AppKit.h>
#include <QDebug>
#include <QGuiApplication>
#include <QWindow>
#import <objc/message.h>
namespace waibusnap
{
namespace
{
void setPreventsActivation(NSWindow* native, bool prevents)
{
    // Qt 创建的 NSPanel 初始 styleMask 不含 Nonactivating；运行中补上 mask
    // 只会改变 AppKit 的视图，不会同步 WindowServer 的 prevents-activation tag。
    // 必须显式同步 tag，否则点击贴图仍会激活本应用，在多显示器下可能触发
    // 其他应用窗口的焦点 / 层级重排。selector 缺失时保持原行为并留下诊断。
    SEL selector = @selector(_setPreventsActivation:);
    if (![native respondsToSelector:selector])
    {
        qWarning().noquote() << "贴图非激活 tag 无法同步：_setPreventsActivation: 不存在";
        return;
    }
    using Setter = void (*)(id, SEL, BOOL);
    reinterpret_cast<Setter>(objc_msgSend)(native, selector, prevents ? YES : NO);
}
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
    {
        if (!native)
            return false;
        if ([native isKindOfClass:[NSPanel class]])
            static_cast<NSPanel*>(native).becomesKeyOnlyIfNeeded = NO;
        setPreventsActivation(native, false);
        return !(native.styleMask & NSWindowStyleMaskNonactivatingPanel);
    }
    if (![native isKindOfClass:[NSPanel class]])
        return false;
    native.styleMask |= NSWindowStyleMaskNonactivatingPanel;
    native.hidesOnDeactivate = NO;
    // 非编辑态不因普通点击自动成为 key；只有单击且未拖动时由显式入口接管。
    static_cast<NSPanel*>(native).becomesKeyOnlyIfNeeded = YES;
    setPreventsActivation(native, true);
    return (native.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0;
}
bool makeStickerWindowKeyForKeyboard(QWindow* window)
{
    if (!window || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return false;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    if (!native)
        return false;
    [native makeKeyWindow];
    [native makeFirstResponder:view];
    return native.isKeyWindow;
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
