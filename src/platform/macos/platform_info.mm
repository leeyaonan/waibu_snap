#include "interfaces/platform_info.h"

namespace waibusnap
{
QString platformName()
{
    // 平台名称查询不触发权限或捕获。
    return QStringLiteral("macOS");
}
}
