#include "interfaces/tray_icon.h"
namespace waibusnap
{
QIcon createTrayIcon()
{
    QIcon icon(QStringLiteral(":/icons/tray/waibusnap-tray-color.png"));
    // Windows 使用彩色版；显式保留高 DPI 档位，不启用模板标记。
    icon.addFile(QStringLiteral(":/icons/tray/waibusnap-tray-color@2x.png"));
    return icon;
}
}
