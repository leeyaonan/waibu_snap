#include "interfaces/capture_provider.h"
#include "interfaces/global_hotkey.h"
#include "interfaces/window_enumerator.h"
#include "session/monotonic_clock.h"
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <QColorSpace>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <array>
#include <cmath>
namespace waibusnap
{
namespace
{
QString permissionLaunchHint()
{
    return QStringLiteral("若已授权仍失败：macOS 27 起请从访达或用 open 命令启动应用"
                          "（从终端直接执行不会继承授权）。");
}
quint64 layoutVersion()
{
    // 仅按需读取布局，不在空闲期间轮询。
    quint64 hash = 1469598103934665603ULL;
    for (NSScreen* screen in NSScreen.screens)
    {
        const CGRect bounds =
            CGDisplayBounds([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
        for (qint64 value :
             {qint64([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]),
              qint64(bounds.origin.x), qint64(bounds.origin.y), qint64(bounds.size.width),
              qint64(bounds.size.height), qint64(screen.backingScaleFactor * 1000)})
        {
            hash = (hash ^ quint64(value)) * 1099511628211ULL;
        }
    }
    return hash;
}
DisplayTarget targetForScreen(NSScreen* screen)
{
    const NSRect frame = screen.frame;
    const CGFloat primaryTop = NSMaxY(NSScreen.screens.firstObject.frame);
    // Cocoa 原点在主屏左下；Qt 原点在主屏左上，逐屏转换，允许负坐标。
    return {[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue],
            QRect(qRound(frame.origin.x), qRound(primaryTop - NSMaxY(frame)),
                  qRound(frame.size.width), qRound(frame.size.height)),
            screen.backingScaleFactor,
            layoutVersion()};
}
class MacDisplays final : public DisplayTopology
{
  public:
    DisplayResult cursorDisplay() const override
    {
        const NSPoint cursor = NSEvent.mouseLocation;
        for (NSScreen* screen in NSScreen.screens)
        {
            if (NSMouseInRect(cursor, screen.frame, NO))
            {
                const auto target = targetForScreen(screen);
                for (QScreen* qtScreen : QGuiApplication::screens())
                {
                    if (qtScreen->geometry() == target.logicalGeometry &&
                        qFuzzyCompare(qtScreen->devicePixelRatio(), target.devicePixelRatio))
                    {
                        return {target, {}};
                    }
                }
                return {{}, QStringLiteral("系统与 Qt 显示器坐标不一致，请重新启动后重试。")};
            }
        }
        return {{}, QStringLiteral("未找到鼠标所在显示器。")};
    }
    bool stillMatches(const DisplayTarget& target) const override
    {
        if (target.layoutVersion != layoutVersion())
            return false;
        for (NSScreen* screen in NSScreen.screens)
        {
            const auto current = targetForScreen(screen);
            if (current.id == target.id)
                return current.logicalGeometry == target.logicalGeometry &&
                       qFuzzyCompare(current.devicePixelRatio, target.devicePixelRatio);
        }
        return false;
    }
};
class MacWindowEnumerator final : public WindowEnumerator
{
  public:
    WindowListResult visibleWindows(const DisplayTarget&) const override
    {
        NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID));
        if (!list)
            return {{}, QStringLiteral("窗口吸附不可用，请手动框选")};
        QVector<QRect> windows;
        for (NSDictionary* info in list)
        {
            NSNumber* layer = info[(__bridge NSString*)kCGWindowLayer];
            NSNumber* alpha = info[(__bridge NSString*)kCGWindowAlpha];
            NSNumber* owner = info[(__bridge NSString*)kCGWindowOwnerPID];
            NSDictionary* bounds = info[(__bridge NSString*)kCGWindowBounds];
            CGRect rectangle = CGRectZero;
            if (!layer || layer.intValue != 0 || !alpha || !(alpha.doubleValue > 0) || !owner ||
                owner.longLongValue == QCoreApplication::applicationPid() || !bounds ||
                !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds,
                                                        &rectangle) ||
                !std::isfinite(rectangle.origin.x) || !std::isfinite(rectangle.origin.y) ||
                !std::isfinite(rectangle.size.width) || !std::isfinite(rectangle.size.height) ||
                rectangle.size.width <= 0 || rectangle.size.height <= 0)
                continue;
            // CG 全局坐标与 Qt 逻辑坐标同构；不读取窗口名，也不据此推断权限。
            const int x = qRound(CGRectGetMinX(rectangle));
            const int y = qRound(CGRectGetMinY(rectangle));
            const QRect window(x, y, qRound(CGRectGetMaxX(rectangle)) - x,
                               qRound(CGRectGetMaxY(rectangle)) - y);
            if (!window.isEmpty())
                windows.append(window);
        }
        return {windows, {}};
    }
};
class MacHotkey final : public GlobalHotkey
{
  public:
    ~MacHotkey() override { unregister(); }
    void unregister() override
    {
        if (hotkey_)
            UnregisterEventHotKey(hotkey_);
        hotkey_ = nullptr;
        if (eventHandler_)
            RemoveEventHandler(eventHandler_);
        eventHandler_ = nullptr;
        handler_ = {};
        sequence_ = {};
    }
    HotkeyRegistration registerHotkey(const QKeySequence& sequence,
                                      std::function<void()> handler) override
    {
        const QString name = sequence.toString(QKeySequence::NativeText);
        const QString bindingState = hotkey_
                                         ? QStringLiteral("原绑定保留。")
                                         : QStringLiteral("当前未启用截图快捷键，可使用托盘入口。");
        UInt32 code = 0;
        UInt32 modifiers = 0;
        if (!keyMapping(sequence, code, modifiers))
            return {false,
                    QStringLiteral("无法映射截图键 %1，请使用 F1–F12 或带修饰键的字母 / 数字；%2")
                        .arg(name, bindingState)};
        // 系统截图组合不能靠注册成功证明可用，不尝试抢占。
        if (modifiers == (cmdKey | shiftKey) &&
            (code == kVK_ANSI_3 || code == kVK_ANSI_4 || code == kVK_ANSI_5))
            return {false,
                    QStringLiteral("截图键 %1 可能被系统截图占用，无法确认可用；请换一个组合，%2")
                        .arg(name, bindingState)};
        const QString enabled = QStringLiteral("截图键 %1 已启用（Fn 模式由系统决定）。").arg(name);
        if (hotkey_ && sequence == sequence_)
        {
            handler_ = std::move(handler);
            return {true, enabled};
        }
        OSStatus status = noErr;
        if (!eventHandler_)
        {
            const EventTypeSpec eventType = {kEventClassKeyboard, kEventHotKeyPressed};
            status = InstallApplicationEventHandler(&MacHotkey::onEvent, 1, &eventType, this,
                                                    &eventHandler_);
        }
        EventHotKeyRef replacement = nullptr;
        const UInt32 replacementId = currentId_ + 1;
        if (status == noErr)
            status = RegisterEventHotKey(code, modifiers, {signature, replacementId},
                                         GetApplicationEventTarget(), kEventHotKeyExclusive,
                                         &replacement);
        if (status != noErr)
        {
            if (!hotkey_ && eventHandler_)
            {
                RemoveEventHandler(eventHandler_);
                eventHandler_ = nullptr;
            }
            return {false, QStringLiteral("截图键 %1 无法启用（系统错误 "
                                          "%2），可能被系统或其他应用占用；%3")
                               .arg(name)
                               .arg(status)
                               .arg(hotkey_ ? QStringLiteral("原绑定保留，可使用托盘截图。")
                                            : bindingState)};
        }
        const EventHotKeyRef previous = hotkey_;
        hotkey_ = replacement;
        currentId_ = replacementId;
        sequence_ = sequence;
        handler_ = std::move(handler);
        if (previous)
            UnregisterEventHotKey(previous);
        return {true, enabled};
    }

  private:
    static bool keyMapping(const QKeySequence& sequence, UInt32& code, UInt32& modifiers)
    {
        if (sequence.isEmpty() || sequence.count() != 1)
            return false;
        const auto combination = sequence[0];
        const auto qtModifiers = combination.keyboardModifiers();
        if (qtModifiers &
            ~(Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier | Qt::ShiftModifier))
            return false;
        // Qt 在 macOS 上 Control 表示 Command，Meta 表示物理 Control。
        if (qtModifiers.testFlag(Qt::ControlModifier))
            modifiers |= cmdKey;
        if (qtModifiers.testFlag(Qt::MetaModifier))
            modifiers |= controlKey;
        if (qtModifiers.testFlag(Qt::AltModifier))
            modifiers |= optionKey;
        if (qtModifiers.testFlag(Qt::ShiftModifier))
            modifiers |= shiftKey;
        const int key = combination.key();
        constexpr std::array<UInt32, 12> functions = {kVK_F1, kVK_F2,  kVK_F3,  kVK_F4,
                                                      kVK_F5, kVK_F6,  kVK_F7,  kVK_F8,
                                                      kVK_F9, kVK_F10, kVK_F11, kVK_F12};
        constexpr std::array<UInt32, 26> letters = {
            kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
            kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
            kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
            kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z};
        constexpr std::array<UInt32, 10> digits = {kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3,
                                                   kVK_ANSI_4, kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7,
                                                   kVK_ANSI_8, kVK_ANSI_9};
        if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
            code = functions[size_t(key - Qt::Key_F1)];
        else if (!(modifiers & (cmdKey | controlKey | optionKey)))
            return false;
        else if (key >= Qt::Key_A && key <= Qt::Key_Z)
            code = letters[size_t(key - Qt::Key_A)];
        else if (key >= Qt::Key_0 && key <= Qt::Key_9)
            code = digits[size_t(key - Qt::Key_0)];
        else
            return false;
        return true;
    }
    static OSStatus onEvent(EventHandlerCallRef, EventRef event, void* data)
    {
        EventHotKeyID id = {};
        const OSStatus status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                                                  nullptr, sizeof(id), nullptr, &id);
        auto* hotkey = static_cast<MacHotkey*>(data);
        if (status != noErr || id.signature != signature || id.id != hotkey->currentId_ ||
            !hotkey->hotkey_ || !hotkey->handler_)
            return eventNotHandledErr;
        // 直接进入共享触发入口，避免队列延迟被排除在 t0 之后。
        const auto handler = hotkey->handler_;
        handler();
        return noErr;
    }
    std::function<void()> handler_;
    EventHotKeyRef hotkey_ = nullptr;
    EventHandlerRef eventHandler_ = nullptr;
    QKeySequence sequence_;
    UInt32 currentId_ = 0;
    static constexpr UInt32 signature = 0x5742534e;
};
void deliver(std::function<void(CaptureResult)> completion, CaptureResult result)
{
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [completion = std::move(completion), result = std::move(result)]() mutable
        { completion(std::move(result)); }, Qt::QueuedConnection);
}
class MacCapture final : public CaptureProvider
{
  public:
    bool hasPermission() const override { return CGPreflightScreenCaptureAccess(); }
    bool requestPermission() override { return CGRequestScreenCaptureAccess(); }
    QString permissionExplanation() const override
    {
        return QStringLiteral(
                   "尚未获得屏幕录制权限，请在系统设置 → 隐私与安全性 → "
                   "屏幕与系统音频录制中允许 WaibuSnap，然后重试；系统要求时请重启应用。") +
               permissionLaunchHint();
    }
    void capture(const DisplayTarget& target,
                 std::function<void(CaptureResult)> completion) override
    {
        // Objective-C block 会保留引用变量本身，异步请求必须持有独立值快照。
        const DisplayTarget requested = target;
        if (!hasPermission())
        {
            deliver(std::move(completion), {{}, CaptureError::Permission, permissionExplanation()});
            return;
        }
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent* content,
                                                         NSError* contentError) {
                                       @autoreleasepool
                                       {
                                           SCDisplay* display = nil;
                                           for (SCDisplay* candidate in content.displays)
                                               if (candidate.displayID == requested.id)
                                               {
                                                   display = candidate;
                                                   break;
                                               }
                                           if (contentError || !display)
                                           {
                                               deliver(completion,
                                                       {{},
                                                        CaptureError::Unavailable,
                                                        QStringLiteral(
                                                            "无法读取显示器捕获信息（错误 "
                                                            "%1），请检查权限与显示器连接后重试。")
                                                            .arg(contentError ? contentError.code
                                                                              : -1)});
                                               return;
                                           }
                                           SCContentFilter* filter =
                                               [[SCContentFilter alloc] initWithDisplay:display
                                                                       excludingWindows:@[]];
                                           SCStreamConfiguration* configuration =
                                               [[SCStreamConfiguration alloc] init];
                                           configuration.captureResolution =
                                               SCCaptureResolutionBest;
                                           configuration.width =
                                               size_t(std::llround(filter.contentRect.size.width *
                                                                   filter.pointPixelScale));
                                           configuration.height =
                                               size_t(std::llround(filter.contentRect.size.height *
                                                                   filter.pointPixelScale));
                                           configuration.showsCursor = NO;
                                           configuration.colorSpaceName = kCGColorSpaceSRGB;
                                           [SCScreenshotManager
                                               captureImageWithFilter:filter
                                                        configuration:configuration
                                                    completionHandler:^(CGImageRef image,
                                                                        NSError* captureError) {
                                                      @autoreleasepool
                                                      {
                                                          if (captureError || !image)
                                                          {
                                                              const bool denied =
                                                                  !CGPreflightScreenCaptureAccess();
                                                              deliver(
                                                                  completion,
                                                                  {{},
                                                                   denied
                                                                       ? CaptureError::Permission
                                                                       : CaptureError::Unavailable,
                                                                   QStringLiteral(
                                                                       "单帧捕获失败（错误 "
                                                                       "%1），请检查屏幕录制授权；"
                                                                       "重新授权后重试，系统要求时"
                                                                       "重启应用。")
                                                                           .arg(captureError
                                                                                    ? captureError
                                                                                          .code
                                                                                    : -1) +
                                                                       permissionLaunchHint()});
                                                              return;
                                                          }
                                                          const int width =
                                                              int(CGImageGetWidth(image));
                                                          const int height =
                                                              int(CGImageGetHeight(image));
                                                          if (width !=
                                                                  qRound(
                                                                      requested.logicalGeometry
                                                                          .width() *
                                                                      requested.devicePixelRatio) ||
                                                              height !=
                                                                  qRound(
                                                                      requested.logicalGeometry
                                                                          .height() *
                                                                      requested.devicePixelRatio))
                                                          {
                                                              deliver(
                                                                  completion,
                                                                  {{},
                                                                   CaptureError::DisplayChanged,
                                                                   QStringLiteral(
                                                                       "捕获像素 %1 × %2 与显示器 "
                                                                       "backing 尺寸 %3 × %4 "
                                                                       "不匹配。")
                                                                       .arg(width)
                                                                       .arg(height)
                                                                       .arg(qRound(
                                                                           requested.logicalGeometry
                                                                               .width() *
                                                                           requested
                                                                               .devicePixelRatio))
                                                                       .arg(qRound(
                                                                           requested.logicalGeometry
                                                                               .height() *
                                                                           target
                                                                               .devicePixelRatio))});
                                                              return;
                                                          }
                                                          QImage pixels(
                                                              width, height,
                                                              QImage::Format_ARGB32_Premultiplied);
                                                          CGColorSpaceRef space =
                                                              CGColorSpaceCreateWithName(
                                                                  kCGColorSpaceSRGB);
                                                          CGContextRef context =
                                                              CGBitmapContextCreate(
                                                                  pixels.bits(), width, height, 8,
                                                                  size_t(pixels.bytesPerLine()),
                                                                  space,
                                                                  kCGImageAlphaPremultipliedFirst |
                                                                      kCGBitmapByteOrder32Little);
                                                          CGColorSpaceRelease(space);
                                                          if (!context)
                                                          {
                                                              deliver(completion,
                                                                      {{},
                                                                       CaptureError::Unavailable,
                                                                       QStringLiteral(
                                                                           "捕获像素分配失败。")});
                                                              return;
                                                          }
                                                          // Core Graphics 在绘制时转换色彩；QImage
                                                          // 第 0 行对应桌面顶端。
                                                          CGContextDrawImage(
                                                              context,
                                                              CGRectMake(0, 0, width, height),
                                                              image);
                                                          CGContextRelease(context);
                                                          pixels.setColorSpace(QColorSpace::SRgb);
                                                          pixels.setDevicePixelRatio(
                                                              requested.devicePixelRatio);
                                                          deliver(completion,
                                                                  {{requested, std::move(pixels),
                                                                    monotonicNs()},
                                                                   CaptureError::None,
                                                                   {}});
                                                      }
                                                    }];
                                       }
                                     }];
    }
};
}
std::unique_ptr<GlobalHotkey> createGlobalHotkey() { return std::make_unique<MacHotkey>(); }
std::unique_ptr<WindowEnumerator> createWindowEnumerator()
{
    return std::make_unique<MacWindowEnumerator>();
}
std::unique_ptr<DisplayTopology> createDisplayTopology() { return std::make_unique<MacDisplays>(); }
std::unique_ptr<CaptureProvider> createCaptureProvider() { return std::make_unique<MacCapture>(); }
}
