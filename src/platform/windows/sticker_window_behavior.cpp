#include "interfaces/sticker_window_behavior.h"
#include <QGuiApplication>
#include <QWindow>
#include <windows.h>
namespace waibusnap
{
namespace
{
class WindowsStickerFocusSession final : public StickerFocusSession
{
  public:
    WindowsStickerFocusSession() : previous_(GetForegroundWindow()) {}
    ~WindowsStickerFocusSession() override
    {
        DWORD process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        if (process == GetCurrentProcessId() && IsWindow(previous_))
            SetForegroundWindow(previous_);
    }

  private:
    HWND previous_;
};
}
bool configureStickerWindowBehavior(QWindow* window, bool) { return window != nullptr; }
std::unique_ptr<StickerFocusSession> activateStickerEditing(QWindow* window)
{
    if (!window || QGuiApplication::platformName() != QStringLiteral("windows"))
        return {};
    auto session = std::make_unique<WindowsStickerFocusSession>();
    SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
    return session;
}
}
