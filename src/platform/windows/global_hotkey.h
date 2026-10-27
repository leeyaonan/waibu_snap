#pragma once
#include "interfaces/global_hotkey.h"
#include <QAbstractNativeEventFilter>
#include <optional>
#include <qt_windows.h>
namespace waibusnap
{
struct WindowsHotkeyChord
{
    UINT key = 0;
    UINT modifiers = 0;
};
std::optional<WindowsHotkeyChord> windowsHotkeyMapping(const QKeySequence& sequence);
// 注入边界只包裹系统调用；测试仍执行生产注册、替换、释放与消息分发逻辑。
struct WindowsHotkeyApi
{
    // 空值表示成功；失败时保留 GetLastError 原值（即使为 0）。
    std::function<std::optional<DWORD>(int, UINT, UINT)> registerKey;
    std::function<void(int)> unregisterKey;
};
class WindowsHotkey final : public GlobalHotkey, public QAbstractNativeEventFilter
{
  public:
    explicit WindowsHotkey(WindowsHotkeyApi api);
    ~WindowsHotkey() override;
    WindowsHotkey(const WindowsHotkey&) = delete;
    WindowsHotkey& operator=(const WindowsHotkey&) = delete;
    using GlobalHotkey::unregister;
    HotkeyRegistration registerHotkey(HotkeyAction action, const QKeySequence& sequence,
                                      std::function<void()> handler) override;
    void unregister(HotkeyAction action) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

  private:
    struct Binding
    {
        int id = 0;
        WindowsHotkeyChord chord;
        std::function<void()> handler;
    };
    WindowsHotkeyApi api_;
    std::array<Binding, hotkeyActions.size()> bindings_;
};
}
