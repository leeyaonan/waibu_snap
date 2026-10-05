#pragma once
class QWindow;
namespace waibusnap
{
// 原生窗口创建后、显示前调用；共享契约不暴露系统句柄。无头测试不处理原生窗口。
bool configureStickerWindowBehavior(QWindow* window);
}
