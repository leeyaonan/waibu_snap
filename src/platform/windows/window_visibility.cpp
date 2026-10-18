#include "interfaces/window_visibility.h"
#include <QGuiApplication>
#include <QWindow>
#include <qt_windows.h>
namespace waibusnap
{
WindowVisibility queryWindowVisibility(QWindow* window)
{
    WindowVisibility result;
    if (!window || !window->handle() ||
        QGuiApplication::platformName() != QStringLiteral("windows"))
        return result;
    const auto native = reinterpret_cast<HWND>(window->winId());
    result.windowNumber = quint64(window->winId());
    result.available = IsWindow(native) != FALSE;
    // Windows 初版只检查原生可见 / 最小化，不能证明未被遮挡或未被 DWM cloak。
    result.onScreen = result.available && IsWindowVisible(native) && !IsIconic(native);
    return result;
}
}
