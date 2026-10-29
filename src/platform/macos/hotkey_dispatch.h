#pragma once
#import <Carbon/Carbon.h>
#include <array>
#include <functional>
namespace waibusnap
{
// 与 Carbon 注册分离，允许构造原生事件验证过期 id / 签名，不占用真实系统热键。
struct MacHotkeyCallback
{
    UInt32 id = 0;
    std::function<void()> handler;
};
template <size_t N>
inline OSStatus dispatchMacHotkeyEvent(EventRef event,
                                       const std::array<MacHotkeyCallback, N>& callbacks)
{
    EventHotKeyID id = {};
    const auto status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                                          nullptr, sizeof(id), nullptr, &id);
    if (status != noErr || id.signature != 0x5742534e)
        return eventNotHandledErr;
    for (const auto& callback : callbacks)
        if (callback.id == id.id && callback.handler)
        {
            // 回调可注销自身或触发退出，复制后调用，避免正在执行的函数被销毁。
            const auto handler = callback.handler;
            handler();
            return noErr;
        }
    return eventNotHandledErr;
}
}
