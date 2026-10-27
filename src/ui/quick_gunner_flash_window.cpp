#include "ui/quick_gunner_flash_window.h"
#include "interfaces/overlay_window_behavior.h"
#include "interfaces/sticker_window_behavior.h"
#include <QPainter>
namespace waibusnap
{
QuickGunnerFlashWindow::QuickGunnerFlashWindow()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("quickGunnerFlashWindow"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    winId();
    configureStickerWindowBehavior(windowHandle());
    raiseOverlayAboveSystemChrome(windowHandle());
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(120);
    connect(&timer_, &QTimer::timeout, this, &QWidget::hide);
}
void QuickGunnerFlashWindow::flash(QRect logicalRegion)
{
    setGeometry(logicalRegion.adjusted(-2, -2, 2, 2));
    raiseOverlayAboveSystemChrome(windowHandle());
    setGeometry(logicalRegion.adjusted(-2, -2, 2, 2));
    show();
    timer_.start();
}
void QuickGunnerFlashWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const QColor color(QStringLiteral("#f0a030"));
    // 整数填充确保边框严格位于机位外侧，透明中心不覆盖拍摄区域。
    painter.fillRect(QRect(0, 0, width(), 2), color);
    painter.fillRect(QRect(0, height() - 2, width(), 2), color);
    painter.fillRect(QRect(0, 2, 2, height() - 4), color);
    painter.fillRect(QRect(width() - 2, 2, 2, height() - 4), color);
}
}
