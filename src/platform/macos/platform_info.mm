#include "interfaces/platform_info.h"

namespace waibusnap
{
QString platformName()
{
    // 本轮仅验证 Objective-C++ 编译路径，不调用捕获或权限 API。
    return QStringLiteral("macOS");
}
}
