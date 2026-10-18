#include "interfaces/overlay_window_behavior.h"
#include <QGuiApplication>
#include <QVariant>
#include <QWindow>
#include <qt_windows.h>
namespace waibusnap
{
void raiseOverlayAboveSystemChrome(QWindow* window)
{
    if (window)
        window->setProperty("waibusnapOverlayLowered", false);
    // Windows 现有 topmost 已覆盖任务栏，无需额外窗口层级操作。
}
void lowerOverlayBelowSystemPanels(QWindow* window)
{
    if (window)
        window->setProperty("waibusnapOverlayLowered", true);
    // Windows 无同等抬升机制，系统面板不受影响，无需临时降级。
}
bool reassertOverlayWindow(QWindow* window, bool activate)
{
    if (!window || !window->handle() || !window->isVisible() ||
        window->property("waibusnapOverlayLowered").toBool() ||
        QGuiApplication::platformName() != QStringLiteral("windows"))
        return false;
    const auto native = reinterpret_cast<HWND>(window->winId());
    const bool ordered = SetWindowPos(native, HWND_TOPMOST, 0, 0, 0, 0,
                                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (activate)
        SetForegroundWindow(native);
    return ordered;
}
std::unique_ptr<OverlayWindowObservation> observeOverlayWindow(QWindow*, QObject*,
                                                               std::function<void(const QString&)>)
{
    // Windows 本轮由会话看门狗检查原生可见性，不订阅 macOS 系统事件。
    return {};
}
}
