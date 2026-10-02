#include "interfaces/capture_provider.h"
#include "interfaces/global_hotkey.h"
#include "session/monotonic_clock.h"
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <QColorSpace>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <cmath>
namespace waibusnap
{
namespace
{
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
class MacHotkey final : public GlobalHotkey
{
  public:
    ~MacHotkey() override
    {
        if (hotkey_)
            UnregisterEventHotKey(hotkey_);
        if (eventHandler_)
            RemoveEventHandler(eventHandler_);
    }
    HotkeyRegistration registerF1(std::function<void()> handler) override
    {
        if (hotkey_)
            return {true, QStringLiteral("F1 已启用（Fn 模式由系统决定）。")};
        handler_ = std::move(handler);
        const EventTypeSpec eventType = {kEventClassKeyboard, kEventHotKeyPressed};
        OSStatus status = InstallApplicationEventHandler(&MacHotkey::onEvent, 1, &eventType, this,
                                                         &eventHandler_);
        if (status == noErr)
        {
            const EventHotKeyID id = {0x5742534e, 1};
            status = RegisterEventHotKey(kVK_F1, 0, id, GetApplicationEventTarget(), 0, &hotkey_);
        }
        if (status != noErr)
        {
            if (eventHandler_)
            {
                RemoveEventHandler(eventHandler_);
                eventHandler_ = nullptr;
            }
            return {false, QStringLiteral(
                               "F1 无法启用（系统错误 %1），请使用托盘截图。此原型尚无改键设置。")
                               .arg(status)};
        }
        return {true, QStringLiteral("F1 已启用（Fn 模式由系统决定）。")};
    }

  private:
    static OSStatus onEvent(EventHandlerCallRef, EventRef event, void* data)
    {
        EventHotKeyID id = {};
        const OSStatus status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                                                  nullptr, sizeof(id), nullptr, &id);
        if (status != noErr || id.signature != 0x5742534e || id.id != 1)
            return eventNotHandledErr;
        // 直接进入共享触发入口，避免队列延迟被排除在 t0 之后。
        static_cast<MacHotkey*>(data)->handler_();
        return noErr;
    }
    std::function<void()> handler_;
    EventHotKeyRef hotkey_ = nullptr;
    EventHandlerRef eventHandler_ = nullptr;
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
    void capture(const DisplayTarget& target,
                 std::function<void(CaptureResult)> completion) override
    {
        // Objective-C block 会保留引用变量本身，异步请求必须持有独立值快照。
        const DisplayTarget requested = target;
        if (!hasPermission())
        {
            deliver(std::move(completion),
                    {{},
                     CaptureError::Permission,
                     QStringLiteral(
                         "尚未获得屏幕录制权限，请在系统设置 → 隐私与安全性 → "
                         "屏幕与系统音频录制中允许 WaibuSnap，然后重试；系统要求时请重启应用。")});
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
                                                                                ? captureError.code
                                                                                : -1)});
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
std::unique_ptr<DisplayTopology> createDisplayTopology() { return std::make_unique<MacDisplays>(); }
std::unique_ptr<CaptureProvider> createCaptureProvider() { return std::make_unique<MacCapture>(); }
}
