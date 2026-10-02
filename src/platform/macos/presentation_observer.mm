#include "interfaces/presentation_observer.h"
#include "session/monotonic_clock.h"
#import <AppKit/AppKit.h>
#include <QPointer>
#import <QuartzCore/QuartzCore.h>
// 显示刷新只能提供呈现代理，不能证明该窗口已被合成器扫描到物理屏幕。
// 正式 NF01 还需平台呈现跟踪 / 外部高帧率摄像校验，日志明确标注 proxy。
@interface WaibuPresentationTarget : NSObject
{
  @public
    QPointer<QObject> context;
    std::function<void(waibusnap::PresentationSample)> completion;
    int refreshes;
}
- (void)step:(CADisplayLink*)link;
@end
@implementation WaibuPresentationTarget
- (void)step:(CADisplayLink*)link
{
    if (++refreshes < 2)
        return;
    [link invalidate];
    auto callback = std::move(completion);
    if (context)
        callback({waibusnap::monotonicNs(), (link.targetTimestamp - link.timestamp) * 1000, true});
}
@end
namespace waibusnap
{
namespace
{
class MacObservation final : public PresentationObservation
{
  public:
    MacObservation(QWindow* window, QObject* context,
                   std::function<void(PresentationSample)> completion)
    {
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
        target_ = [[WaibuPresentationTarget alloc] init];
        target_->context = context;
        target_->completion = std::move(completion);
        target_->refreshes = 0;
        link_ = [view.window.screen displayLinkWithTarget:target_ selector:@selector(step:)];
        if (!link_)
        {
            target_->completion({});
            target_->completion = {};
            return;
        }
        [link_ addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    ~MacObservation() override { [link_ invalidate]; }

  private:
    WaibuPresentationTarget* target_;
    CADisplayLink* link_;
};
}
std::unique_ptr<PresentationObservation>
observePresentation(QWindow* window, QObject* context,
                    std::function<void(PresentationSample)> completion)
{
    return std::make_unique<MacObservation>(window, context, std::move(completion));
}
}
