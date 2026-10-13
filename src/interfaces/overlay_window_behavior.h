#pragma once
class QWindow;
namespace waibusnap
{
// 幂等抬升冻结覆盖层；空指针、无原生窗口与离屏环境安全返回。
void raiseOverlayAboveSystemChrome(QWindow* window);
// 将冻结覆盖层临时降回常规层级，使系统面板（保存 / 打开等）显示在覆盖层之上；
// 系统面板结束后必须调用 raiseOverlayAboveSystemChrome 恢复。
// 空指针、无原生窗口与离屏环境安全返回。
void lowerOverlayBelowSystemPanels(QWindow* window);
}
