#pragma once
#include <memory>
class QWindow;
namespace waibusnap
{
// 原生窗口创建后、显示前调用；无头测试不处理原生窗口。
bool configureStickerWindowBehavior(QWindow* window, bool editing = false);
// 编辑期间持有会话；析构仅在本应用仍在前台时恢复进入编辑前的应用。
class StickerFocusSession
{
  public:
    virtual ~StickerFocusSession() = default;
};
std::unique_ptr<StickerFocusSession> activateStickerEditing(QWindow* window);
}
