#include "interfaces/tray_icon.h"
namespace waibusnap
{
QIcon createTrayIcon()
{
    QIcon icon(QStringLiteral(":/icons/tray/waibusnap-tray-mask.png"));
    // 显式保留 Retina 档位，不依赖创建图标时所在屏幕的 DPR。
    icon.addFile(QStringLiteral(":/icons/tray/waibusnap-tray-mask@2x.png"));
    icon.setIsMask(true);
    return icon;
}
}
