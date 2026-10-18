#include "interfaces/overlay_window_behavior.h"
#include <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QWindow>
namespace waibusnap
{
namespace
{
NSWindow* nativeWindow(QWindow* window)
{
    if (!window || !window->handle() || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return nil;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    return view.window;
}
struct NotificationState
{
    QPointer<QObject> context;
    std::function<void(const QString&)> callback;
    bool queued = false;
    bool alive = true;
};
class MacOverlayObservation final : public OverlayWindowObservation
{
  public:
    MacOverlayObservation(NSWindow* window, QObject* context,
                          std::function<void(const QString&)> changed)
        : state_(std::make_shared<NotificationState>())
    {
        state_->context = context;
        state_->callback = std::move(changed);
        tokens_ = [NSMutableArray array];
        add(NSWorkspace.sharedWorkspace.notificationCenter,
            NSWorkspaceActiveSpaceDidChangeNotification, nil);
        add(NSWorkspace.sharedWorkspace.notificationCenter, NSWorkspaceDidWakeNotification, nil);
        add(NSNotificationCenter.defaultCenter, NSApplicationDidChangeScreenParametersNotification,
            nil);
        add(NSNotificationCenter.defaultCenter, NSWindowDidChangeOcclusionStateNotification,
            window);
    }
    ~MacOverlayObservation() override
    {
        state_->alive = false;
        for (NSArray* entry in tokens_)
            [entry[0] removeObserver:entry[1]];
    }

  private:
    void add(NSNotificationCenter* center, NSNotificationName name, id object)
    {
        const auto state = state_;
        id token = [center addObserverForName:name
                                       object:object
                                        queue:NSOperationQueue.mainQueue
                                   usingBlock:^(NSNotification* notification) {
                                     if (!state->alive || !state->context || state->queued)
                                         return;
                                     state->queued = true;
                                     const QString reason =
                                         QString::fromNSString(notification.name);
                                     QTimer::singleShot(100, state->context,
                                                        [state, reason]
                                                        {
                                                            state->queued = false;
                                                            if (state->alive && state->context)
                                                                state->callback(reason);
                                                        });
                                   }];
        [tokens_ addObject:@[ center, token ]];
    }
    std::shared_ptr<NotificationState> state_;
    NSMutableArray* tokens_;
};
}

void raiseOverlayAboveSystemChrome(QWindow* window)
{
    if (!window || !window->handle() || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    if (native)
    {
        window->setProperty("waibusnapOverlayLowered", false);
        [native setLevel:NSStatusWindowLevel];
    }
}
void lowerOverlayBelowSystemPanels(QWindow* window)
{
    if (!window || !window->handle() || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    NSWindow* native = view.window;
    // 独立系统面板低于抬升后的覆盖层；临时降级，面板结束后必须配对抬升恢复。
    if (native)
    {
        window->setProperty("waibusnapOverlayLowered", true);
        [native setLevel:NSNormalWindowLevel];
    }
}
bool reassertOverlayWindow(QWindow* window, bool activate)
{
    NSWindow* native = nativeWindow(window);
    if (!native || !window->isVisible() || window->property("waibusnapOverlayLowered").toBool())
        return false;
    [native setLevel:NSStatusWindowLevel];
    [native orderFrontRegardless];
    if (activate)
    {
        if (!NSApp.isActive)
            [NSApp activateIgnoringOtherApps:YES];
        [native makeKeyWindow];
    }
    return true;
}
std::unique_ptr<OverlayWindowObservation>
observeOverlayWindow(QWindow* window, QObject* context, std::function<void(const QString&)> changed)
{
    NSWindow* native = nativeWindow(window);
    if (!native || !context)
        return {};
    return std::make_unique<MacOverlayObservation>(native, context, std::move(changed));
}
}
