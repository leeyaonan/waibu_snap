#pragma once
#include <QPoint>
#include <QString>
#include <memory>
namespace waibusnap
{
struct ScrollInjection
{
    QPoint globalPoint;
    int dx = 0;
    int dy = 0;
};
class ScrollInjector
{
  public:
    virtual ~ScrollInjector() = default;
    virtual bool hasAccessibilityPermission() const = 0;
    virtual void requestAccessibilityPermission() = 0;
    virtual bool postScroll(ScrollInjection event) = 0;
    static QString permissionExplanation()
    {
        return QStringLiteral("自动滚动需要“辅助功能”权限来模拟滚动。请在 系统设置 → "
                              "隐私与安全性 → 辅助功能 中勾选 WaibuSnap；手动滚动不受影响。"
                              "如仍无效请重启应用。");
    }
};
std::unique_ptr<ScrollInjector> createScrollInjector();
}
