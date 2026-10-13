#include "interfaces/scroll_injector.h"
namespace waibusnap
{
namespace
{
class WindowsScrollInjector final : public ScrollInjector
{
  public:
    bool hasAccessibilityPermission() const override { return false; }
    void requestAccessibilityPermission() override {}
    bool postScroll(ScrollInjection) override { return false; }
};
}
std::unique_ptr<ScrollInjector> createScrollInjector()
{
    return std::make_unique<WindowsScrollInjector>();
}
}
