#pragma once
#include <QObject>
#include <QWindow>
#include <functional>
#include <memory>
namespace waibusnap
{
struct PresentationSample
{
    qint64 observedAtNs = 0;
    double refreshIntervalMs = 0;
    bool available = false;
};
class PresentationObservation
{
  public:
    virtual ~PresentationObservation() = default;
};
// 在 Qt backing store 提交后的事件循环调用；平台自行解析 QWindow，不泄露句柄。
std::unique_ptr<PresentationObservation>
observePresentation(QWindow* window, QObject* context,
                    std::function<void(PresentationSample)> completion);
}
