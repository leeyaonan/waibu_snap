#include "app/quick_gunner_hotkey.h"
#include "app/hotkey_rules.h"
namespace waibusnap
{
QuickGunnerHotkey::QuickGunnerHotkey(AppSettings settings, GlobalHotkey& hotkey,
                                     std::function<void()> handler)
    : settings_(std::move(settings)), hotkey_(hotkey), handler_(std::move(handler)),
      preferredSequence_(defaultHotkey(HotkeyAction::QuickGunnerShutter))
{
}
QuickGunnerHotkey::~QuickGunnerHotkey() { deactivate(); }
void QuickGunnerHotkey::load()
{
    const auto loaded = settings_.loadHotkey(HotkeyAction::QuickGunnerShutter);
    preferredSequence_ = loaded.sequence;
    explanation_ = loaded.explanation;
}
QString QuickGunnerHotkey::change(const QKeySequence& sequence)
{
    const auto error = hotkeyValidationError(sequence);
    if (!error.isEmpty())
        return error;
    if (registered_)
    {
        const auto registration =
            hotkey_.registerHotkey(HotkeyAction::QuickGunnerShutter, sequence, handler_);
        explanation_ = registration.explanation;
        if (!registration.enabled)
            return registration.explanation;
    }
    preferredSequence_ = sequence;
    const auto saved = settings_.saveHotkey(sequence, HotkeyAction::QuickGunnerShutter);
    if (!registered_ || !saved.isEmpty())
        explanation_ = saved;
    return saved;
}
QString QuickGunnerHotkey::activate()
{
    if (registered_)
        return {};
    const auto registration =
        hotkey_.registerHotkey(HotkeyAction::QuickGunnerShutter, preferredSequence_, handler_);
    registered_ = registration.enabled;
    explanation_ = registration.explanation;
    return registered_ ? QString() : registration.explanation;
}
void QuickGunnerHotkey::deactivate()
{
    if (!registered_)
        return;
    hotkey_.unregister(HotkeyAction::QuickGunnerShutter);
    registered_ = false;
}
}
