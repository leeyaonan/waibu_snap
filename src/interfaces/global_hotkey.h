#pragma once
#include <QKeySequence>
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
    // 先注册新键，成功后释放旧键；失败必须保留旧键及原处理器。
    virtual HotkeyRegistration registerHotkey(const QKeySequence& sequence,
                                              std::function<void()> handler) = 0;
    virtual void unregister() = 0;
};
std::unique_ptr<GlobalHotkey> createGlobalHotkey();
}
