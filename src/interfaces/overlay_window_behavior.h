#pragma once
class QWindow;
namespace waibusnap
{
// 幂等抬升冻结覆盖层；空指针、无原生窗口与离屏环境安全返回。
void raiseOverlayAboveSystemChrome(QWindow* window);
}
