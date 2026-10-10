#include "interfaces/autostart.h"
#import <ServiceManagement/ServiceManagement.h>
namespace waibusnap
{
namespace
{
class MacAutostart final : public Autostart
{
  public:
    AutostartState query() const override
    {
        @autoreleasepool
        {
            SMAppService* service = SMAppService.mainAppService;
            if (!service)
                return {false, false,
                        QStringLiteral("系统登录项服务不可用，请从应用包启动后重试。")};
            switch (service.status)
            {
            case SMAppServiceStatusNotRegistered:
                return {};
            case SMAppServiceStatusEnabled:
                return {true, false, {}};
            case SMAppServiceStatusRequiresApproval:
                return {true, true, {}};
            case SMAppServiceStatusNotFound:
                return {
                    false, false,
                    QStringLiteral("系统未找到此应用的登录项服务，请从已签名的应用包启动后重试。")};
            }
            return {false, false, QStringLiteral("系统返回未知登录项状态，请稍后重试。")};
        }
    }
    QString setEnabled(bool enabled) override
    {
        @autoreleasepool
        {
            SMAppService* service = SMAppService.mainAppService;
            if (!service)
                return QStringLiteral("系统登录项服务不可用，请从应用包启动后重试。");
            const auto state = query();
            if (!state.notice.isEmpty())
                return state.notice;
            if (enabled == state.enabled)
                return {};
            NSError* error = nil;
            const BOOL success = enabled ? [service registerAndReturnError:&error]
                                         : [service unregisterAndReturnError:&error];
            if (success)
                return {};
            const QString reason = error
                                       ? QStringLiteral("%1（%2，错误码 %3）")
                                             .arg(QString::fromNSString(error.localizedDescription),
                                                  QString::fromNSString(error.domain))
                                             .arg(error.code)
                                       : QStringLiteral("系统未提供错误详情");
            return QStringLiteral("%1登录时自动启动失败：%2")
                .arg(enabled ? QStringLiteral("开启") : QStringLiteral("关闭"), reason);
        }
    }
};
}
std::unique_ptr<Autostart> createAutostart() { return std::make_unique<MacAutostart>(); }
}
