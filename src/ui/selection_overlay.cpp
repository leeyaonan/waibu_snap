#include "ui/selection_overlay.h"
#include "core/selection_geometry.h"
#include "session/monotonic_clock.h"
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
namespace waibusnap
{
SelectionOverlay::SelectionOverlay(CaptureFrame frame)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      frame_(std::move(frame))
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setGeometry(frame_.display.logicalGeometry);
    setWindowTitle(QStringLiteral("WaibuSnap 选区"));
}
QRect SelectionOverlay::selection() const
{
    return normalizedPixelSelection(anchor_, cursor_, frame_.display.devicePixelRatio,
                                    frame_.pixels.size());
}
void SelectionOverlay::paintEvent(QPaintEvent*)
{
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // QImage 带目标屏 DPR，以原像素绘制，不生成缩放副本。
        painter.drawImage(QPointF(0, 0), frame_.pixels);
        const QRect pixels = selection();
        const qreal scale = frame_.display.devicePixelRatio;
        const QRectF region(pixels.x() / scale, pixels.y() / scale, pixels.width() / scale,
                            pixels.height() / scale);
        QPainterPath shade;
        shade.addRect(rect());
        if (!pixels.isEmpty())
            shade.addRect(region);
        painter.fillPath(shade, QColor(0, 0, 0, 75));
        painter.setPen(QPen(Qt::white, 1.0 / scale));
        if (!pixels.isEmpty())
            painter.drawRect(region);
        const QString text = QStringLiteral("%1 × %2 像素   拖拽选区 · 松开确认 · Esc 取消")
                                 .arg(pixels.width())
                                 .arg(pixels.height());
        const QRect box(16, 16, std::min(width() - 32, 520), 36);
        painter.fillRect(box, QColor(20, 20, 20, 220));
        painter.drawText(box.adjusted(12, 0, -12, 0), Qt::AlignVCenter, text);
    }
    if (!painted_)
    {
        painted_ = true;
        emit firstPaintCompleted(monotonicNs());
    }
}
void SelectionOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    anchor_ = cursor_ = event->position();
    dragging_ = true;
    update();
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!dragging_)
        return;
    cursor_ = event->position();
    update();
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (!dragging_ || event->button() != Qt::LeftButton)
        return;
    cursor_ = event->position();
    dragging_ = false;
    if (!selection().isEmpty())
        complete(true);
    else
        update();
}
void SelectionOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
        complete(false);
    else
        QWidget::keyPressEvent(event);
}
void SelectionOverlay::closeEvent(QCloseEvent* event)
{
    event->accept();
    if (!finished_)
    {
        finished_ = true;
        emit finished({}, false);
    }
}
void SelectionOverlay::complete(bool confirmed)
{
    if (finished_)
        return;
    finished_ = true;
    hide();
    emit finished(confirmed ? selection() : QRect(), confirmed);
}
}
