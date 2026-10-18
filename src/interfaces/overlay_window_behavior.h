#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
class QWindow;
namespace waibusnap
{
// 幂等抬升冻结覆盖层；空指针、无原生窗口与离屏环境安全返回。
void raiseOverlayAboveSystemChrome(QWindow* window);
// 将冻结覆盖层临时降回常规层级，使系统面板（保存 / 打开等）显示在覆盖层之上；
// 系统面板结束后必须调用 raiseOverlayAboveSystemChrome 恢复。
// 空指针、无原生窗口与离屏环境安全返回。
void lowerOverlayBelowSystemPanels(QWindow* window);
// 尊重 lower/raise 的面板配对状态；长图辅助窗传 false，禁止抢回目标应用焦点。
bool reassertOverlayWindow(QWindow* window, bool activate);
class OverlayWindowObservation
{
  public:
    virtual ~OverlayWindowObservation() = default;
};
// 合并系统事件并排入 GUI 事件循环；析构注销所有原生通知与待执行回调。
std::unique_ptr<OverlayWindowObservation>
observeOverlayWindow(QWindow* window, QObject* context,
                     std::function<void(const QString&)> changed);
}
