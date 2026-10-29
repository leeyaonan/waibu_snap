#include "interfaces/tray_icon.h"
#include <QPainter>
namespace waibusnap
{
QIcon createTrayIcon(TrayIconState state)
{
    QIcon icon(QStringLiteral(":/icons/tray/waibusnap-tray-mask.png"));
    // 显式保留 Retina 档位，不依赖创建图标时所在屏幕的 DPR。
    icon.addFile(QStringLiteral(":/icons/tray/waibusnap-tray-mask@2x.png"));
    icon.setIsMask(true);
    if (state != TrayIconState::Idle)
    {
        icon = QIcon();
        for (int scale : {1, 2})
        {
            QPixmap pixels(QStringLiteral(":/icons/tray/waibusnap-tray-color%1.png")
                               .arg(scale == 2 ? QStringLiteral("@2x") : QString()));
            pixels.setDevicePixelRatio(1);
            QPainter painter(&pixels);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(30, 30, 30), scale));
            painter.setBrush(state == TrayIconState::Recording ? QColor(255, 59, 48)
                                                               : QColor(255, 159, 10));
            painter.drawEllipse(QRectF(pixels.width() - 7 * scale, scale, 6 * scale, 6 * scale));
            painter.end();
            pixels.setDevicePixelRatio(scale);
            icon.addPixmap(pixels);
        }
        icon.setIsMask(false);
    }
    return icon;
}
}
