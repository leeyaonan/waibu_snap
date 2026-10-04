#pragma once

#include <QString>

namespace waibusnap
{
// 共享层定义最小适配契约；系统类型不得出现在接口中。
QString platformName();
// 单实例策略由平台实现提供，避免改变尚未实现原生托盘的 Windows 行为。
bool requiresSingleInstanceProtection();
}
