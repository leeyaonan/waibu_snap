#include "platform/windows/global_hotkey.h"
#include <QCoreApplication>
#include <QDebug>
namespace waibusnap
{
namespace
{
constexpr int firstId = 0x5700;
constexpr int lastId = 0xbfff;
// 每轮四个连续 ID，槽位顺序与 hotkeyActions 一致；整个进程不复用旧 token。
int nextIdBlock = firstId;
std::optional<size_t> actionIndex(HotkeyAction action)
{
    for (size_t i = 0; i < hotkeyActions.size(); ++i)
        if (hotkeyActions[i] == action)
            return i;
    return {};
}
bool sameChord(const WindowsHotkeyChord& a, const WindowsHotkeyChord& b)
{
    return a.key == b.key && a.modifiers == b.modifiers;
}
QString registrationError(DWORD error, const WindowsHotkeyChord& chord)
{
    if (chord.key == VK_F12)
        return QStringLiteral("F12 由 Windows 调试器保留，无法用作全局热键");
    switch (error)
    {
    case ERROR_HOTKEY_ALREADY_REGISTERED:
        return QStringLiteral("该组合已被其他应用或系统占用");
    case ERROR_INVALID_HOTKEY:
    case ERROR_INVALID_PARAMETER:
        return QStringLiteral("Windows 不支持该热键组合");
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("Windows 拒绝注册该热键，可能是系统保留组合");
    default:
        return QStringLiteral("Windows 注册热键失败");
    }
}
}
std::optional<WindowsHotkeyChord> windowsHotkeyMapping(const QKeySequence& sequence)
{
    if (sequence.isEmpty() || sequence.count() != 1)
        return {};
    const auto combination = sequence[0];
    const auto modifiers = combination.keyboardModifiers();
    if (modifiers & ~(Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier | Qt::ShiftModifier))
        return {};
    WindowsHotkeyChord chord;
    // Windows 上 Qt Meta 明确表示 Win 键，不能沿用 macOS 的 Control 互换规则。
    if (modifiers.testFlag(Qt::ControlModifier))
        chord.modifiers |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::MetaModifier))
        chord.modifiers |= MOD_WIN;
    if (modifiers.testFlag(Qt::AltModifier))
        chord.modifiers |= MOD_ALT;
    if (modifiers.testFlag(Qt::ShiftModifier))
        chord.modifiers |= MOD_SHIFT;
    const auto key = combination.key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        chord.key = VK_F1 + UINT(key - Qt::Key_F1);
    else if (!(chord.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
        return {};
    else if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
        chord.key = UINT(key);
    else
        return {};
    return chord;
}
WindowsHotkey::WindowsHotkey(WindowsHotkeyApi api) : api_(std::move(api))
{
    if (auto* app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
}
WindowsHotkey::~WindowsHotkey()
{
    if (auto* app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
    unregister();
}
HotkeyRegistration WindowsHotkey::registerHotkey(HotkeyAction action, const QKeySequence& sequence,
                                                 std::function<void()> handler)
{
    const auto index = actionIndex(action);
    if (!index)
        return {false, QStringLiteral("未知热键动作，未修改任何绑定。")};
    auto& binding = bindings_[*index];
    const auto state = binding.id ? QStringLiteral("原绑定保留。")
                                  : QStringLiteral("此动作未启用，可使用托盘入口。");
    const auto chord = windowsHotkeyMapping(sequence);
    if (!chord)
        return {false, QStringLiteral("无法映射快捷键；%1").arg(state)};
    for (size_t i = 0; i < bindings_.size(); ++i)
        if (i != *index && bindings_[i].id && sameChord(bindings_[i].chord, *chord))
            return {false, QStringLiteral("快捷键与其他动作重复；%1").arg(state)};
    const auto enabled = QStringLiteral("快捷键 %1 已启用（Meta 对应 Win 键）。")
                             .arg(sequence.toString(QKeySequence::NativeText));
    if (binding.id && sameChord(binding.chord, *chord))
    {
        binding.handler = std::move(handler);
        return {true, enabled};
    }
    if (nextIdBlock + int(hotkeyActions.size()) - 1 > lastId)
        return {false, QStringLiteral("本次运行的热键标识已用尽，请重启应用；%1").arg(state)};
    const int replacementId = nextIdBlock + int(*index);
    nextIdBlock += int(hotkeyActions.size());
    const auto error = api_.registerKey(replacementId, chord->modifiers | MOD_NOREPEAT, chord->key);
    if (error != ERROR_SUCCESS)
        return {false, QStringLiteral("%1（系统错误码 %2）；%3")
                           .arg(registrationError(error, *chord))
                           .arg(error)
                           .arg(state)};
    const int previousId = binding.id;
    binding = {replacementId, *chord, std::move(handler)};
    if (previousId)
        api_.unregisterKey(previousId);
    return {true, enabled};
}
void WindowsHotkey::unregister(HotkeyAction action)
{
    const auto index = actionIndex(action);
    if (!index)
        return;
    const int id = bindings_[*index].id;
    bindings_[*index] = {};
    if (id)
        api_.unregisterKey(id);
}
bool WindowsHotkey::nativeEventFilter(const QByteArray& type, void* message, qintptr* result)
{
    if ((type != "windows_dispatcher_MSG" && type != "windows_generic_MSG") || !message)
        return false;
    const auto& event = *static_cast<const MSG*>(message);
    if (event.message != WM_HOTKEY || event.hwnd || event.wParam < firstId || event.wParam > lastId)
        return false;
    const size_t index = (event.wParam - firstId) % hotkeyActions.size();
    const auto& binding = bindings_[index];
    if (event.wParam != WPARAM(binding.id) || !binding.handler ||
        HIWORD(event.lParam) != binding.chord.key ||
        LOWORD(event.lParam) != binding.chord.modifiers)
        return false;
    if (result)
        *result = 0;
    // 回调可注销自身或触发退出，调用前复制，之后不再访问绑定与对象。
    const auto handler = binding.handler;
    handler();
    return true;
}
std::unique_ptr<GlobalHotkey> createGlobalHotkey()
{
    return std::make_unique<WindowsHotkey>(WindowsHotkeyApi{
        [](int id, UINT modifiers, UINT key) -> DWORD
        {
            if (RegisterHotKey(nullptr, id, modifiers, key))
                return ERROR_SUCCESS;
            const auto error = GetLastError();
            return error == ERROR_SUCCESS ? ERROR_INVALID_HOTKEY : error;
        },
        [](int id)
        {
            if (!UnregisterHotKey(nullptr, id))
                qWarning().noquote()
                    << QStringLiteral("注销 Windows 热键失败：id=%1，系统错误码 %2")
                           .arg(id)
                           .arg(GetLastError());
        }});
}
}
