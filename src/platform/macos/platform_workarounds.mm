#include "interfaces/platform_workarounds.h"
#import <AppKit/AppKit.h>
#include <QDebug>
#include <mutex>
#import <objc/runtime.h>
namespace waibusnap
{
namespace
{
IMP originalClickCount = nullptr;
NSInteger safeClickCount(NSEvent* event, SEL selector)
{
    switch (event.type)
    {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeMouseEntered:
    case NSEventTypeMouseExited:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
    case NSEventTypeOtherMouseDragged:
        return reinterpret_cast<NSInteger (*)(id, SEL)>(originalClickCount)(event, selector);
    default:
        // KitDefined 等非鼠标事件不能读取 AppKit 的 clickCount。
        return 1;
    }
}
}
void installPlatformCompatibilityWorkarounds()
{
    // QTBUG-147449：macOS 27 手势回调的 currentEvent 可能是非鼠标事件。
    // 参照 qtbase 6.12 / 6.11 分支 6192d9edd0 官方修复的事件类型检查。
    // 当前锁定 Qt 6.11.2，先在本进程绕行；升级至 >=6.12.0 或含该提交的
    // 6.11.x 后删除此绕行及其安装入口，不再全局替换 NSEvent 方法。
    static std::once_flag installed;
    std::call_once(installed,
                   []
                   {
                       const Method method =
                           class_getInstanceMethod([NSEvent class], @selector(clickCount));
                       if (!method)
                           qFatal("无法安装 macOS 托盘兼容绕行：NSEvent.clickCount 不存在。");
                       originalClickCount = method_getImplementation(method);
                       method_setImplementation(method, reinterpret_cast<IMP>(safeClickCount));
                   });
}
}
