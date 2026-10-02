#pragma once
#include <QString>
#include <functional>
#include <memory>
namespace waibusnap
{
struct HotkeyRegistration
{
    bool enabled = false;
    QString explanation;
};
class GlobalHotkey
{
  public:
    virtual ~GlobalHotkey() = default;
    virtual HotkeyRegistration registerF1(std::function<void()> handler) = 0;
};
std::unique_ptr<GlobalHotkey> createGlobalHotkey();
}
