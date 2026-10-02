#pragma once
#include "interfaces/capture_provider.h"
#include <QVector>
#include <memory>
namespace waibusnap
{
struct WindowListResult
{
    // 主屏左上为原点、y 向下的全局逻辑外框，按前到后排列。
    QVector<QRect> windows;
    QString error;
};
class WindowEnumerator
{
  public:
    virtual ~WindowEnumerator() = default;
    virtual WindowListResult visibleWindows(const DisplayTarget& target) const = 0;
};
std::unique_ptr<WindowEnumerator> createWindowEnumerator();
}
