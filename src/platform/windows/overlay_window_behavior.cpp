#include "interfaces/overlay_window_behavior.h"
namespace waibusnap
{
void raiseOverlayAboveSystemChrome(QWindow*)
{
    // Windows 现有 topmost 已覆盖任务栏，无需额外窗口层级操作。
}
}
