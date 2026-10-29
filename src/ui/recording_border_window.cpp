#include "ui/recording_border_window.h"
#include <QPainter>
namespace waibusnap
{
QString recordingDuration(qint64 ms)
{
    const auto seconds = qMax(qint64(0), ms) / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
RecordingBorderWindow::RecordingBorderWindow()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("recordingBorder"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::NoFocus);
}
void RecordingBorderWindow::present(QRect frame, qint64 recordedMs)
{
    text_ = QStringLiteral("已暂停 · %1").arg(recordingDuration(recordedMs));
    setGeometry(frame);
    if (!isVisible())
    {
        winId();
        raiseOverlayAboveSystemChrome(windowHandle());
        setGeometry(frame);
        observation_ = observeOverlayWindow(windowHandle(), this,
                                            [this](const QString&)
                                            {
                                                if (isVisible())
                                                    reassertOverlayWindow(windowHandle(), false);
                                            });
        show();
    }
    update();
}
void RecordingBorderWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setPen(QPen(QColor(0, 0, 0, 100), 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.setPen(QPen(QColor(255, 59, 48), 2));
    p.drawRect(rect().adjusted(2, 2, -3, -3));
    const QRect capsule(7, 7, fontMetrics().horizontalAdvance(text_) + 20, 28);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 20, 215));
    p.drawRoundedRect(capsule, 8, 8);
    p.setPen(Qt::white);
    p.drawText(capsule, Qt::AlignCenter, text_);
}
}
