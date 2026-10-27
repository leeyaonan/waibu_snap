#pragma once
#include "app/app_settings.h"
namespace waibusnap
{
// 读取 / 配置与系统注册分离；只有模式激活时才占用快门键。
class QuickGunnerHotkey
{
  public:
    QuickGunnerHotkey(AppSettings settings, GlobalHotkey& hotkey, std::function<void()> handler);
    void load();
    QKeySequence currentSequence() const { return preferredSequence_; }
    QString change(const QKeySequence& sequence);
    QString activate();
    void deactivate();
    bool registered() const { return registered_; }
    QString explanation() const { return explanation_; }

  private:
    AppSettings settings_;
    GlobalHotkey& hotkey_;
    std::function<void()> handler_;
    QKeySequence preferredSequence_;
    bool registered_ = false;
    QString explanation_;
};
}
