#pragma once
#include <QKeySequence>
#include <QString>
#include <array>
#include <functional>
#include <memory>
namespace waibusnap
{
enum class HotkeyAction
{
    Screenshot,
    HistoryPrevious,
    HistoryNext,
    HistoryOpen,
    QuickGunnerShutter
};
constexpr std::array<HotkeyAction, 5> hotkeyActions = {
    HotkeyAction::Screenshot, HotkeyAction::HistoryPrevious, HotkeyAction::HistoryNext,
    HotkeyAction::HistoryOpen, HotkeyAction::QuickGunnerShutter};
struct HotkeyRegistration
{
    bool enabled = false;
    QString explanation;
};
class GlobalHotkey
{
  public:
    virtual ~GlobalHotkey() = default;
    // 先注册新键，成功后释放旧键；失败必须保留旧键及原处理器。
    virtual HotkeyRegistration registerHotkey(HotkeyAction action, const QKeySequence& sequence,
                                              std::function<void()> handler) = 0;
    virtual void unregister(HotkeyAction action) = 0;
    void unregister()
    {
        for (const auto action : hotkeyActions)
            unregister(action);
    }
};
std::unique_ptr<GlobalHotkey> createGlobalHotkey();
}
