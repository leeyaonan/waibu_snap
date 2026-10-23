#pragma once
#include <memory>
class QWindow;
namespace waibusnap
{
// 原生窗口创建后、显示前调用；无头测试不处理原生窗口。
bool configureStickerWindowBehavior(QWindow* window, bool editing = false);
// 非编辑态单击后调用：让贴图成为 key window 以接收空格等键盘输入，
// 但不激活本应用；失败时返回 false，由调用方决定回退。
bool makeStickerWindowKeyForKeyboard(QWindow* window);
// 编辑期间持有会话；析构仅在本应用仍在前台时恢复进入编辑前的应用。
class StickerFocusSession
{
  public:
    virtual ~StickerFocusSession() = default;
};
std::unique_ptr<StickerFocusSession> activateStickerEditing(QWindow* window);
}
