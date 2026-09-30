#include "interfaces/platform_info.h"

namespace waibusnap
{
QString platformName()
{
    // 后续 Win32 / DXGI 实现只能放在本目录。
    return QStringLiteral("Windows");
}
}
