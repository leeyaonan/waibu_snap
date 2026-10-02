#include "ui/selection_overlay.h"
#include "session/monotonic_clock.h"
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
constexpr qreal handleRadius = 6;
}
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
QRect SelectionOverlay::selection() const { return selection_; }
QRectF SelectionOverlay::logicalSelection() const
{
    const qreal scale = frame_.display.devicePixelRatio;
    return {selection_.x() / scale, selection_.y() / scale, selection_.width() / scale,
            selection_.height() / scale};
}
SelectionEdges SelectionOverlay::edgesAt(QPointF position) const
{
    if (selection_.isEmpty())
        return {};
    const QRectF region = logicalSelection();
    if (!region.adjusted(-handleRadius, -handleRadius, handleRadius, handleRadius)
             .contains(position))
        return {};
    SelectionEdges edges;
    const qreal leftDistance = std::abs(position.x() - region.left());
    const qreal rightDistance = std::abs(position.x() - region.right());
    const qreal topDistance = std::abs(position.y() - region.top());
    const qreal bottomDistance = std::abs(position.y() - region.bottom());
    if (std::min(leftDistance, rightDistance) <= handleRadius)
        edges |= leftDistance <= rightDistance ? SelectionEdge::Left : SelectionEdge::Right;
    if (std::min(topDistance, bottomDistance) <= handleRadius)
        edges |= topDistance <= bottomDistance ? SelectionEdge::Top : SelectionEdge::Bottom;
    return edges;
}
void SelectionOverlay::updateCursor(QPointF position)
{
    const SelectionEdges edges = dragMode_ == DragMode::Resize ? resizeEdges_ : edgesAt(position);
    const bool horizontal =
        edges.testFlag(SelectionEdge::Left) || edges.testFlag(SelectionEdge::Right);
    const bool vertical =
        edges.testFlag(SelectionEdge::Top) || edges.testFlag(SelectionEdge::Bottom);
    if (dragMode_ == DragMode::Create)
        setCursor(Qt::CrossCursor);
    else if (dragMode_ == DragMode::Move)
        setCursor(Qt::SizeAllCursor);
    else if (horizontal && vertical)
        setCursor(edges.testFlag(SelectionEdge::Left) == edges.testFlag(SelectionEdge::Top)
                      ? Qt::SizeFDiagCursor
                      : Qt::SizeBDiagCursor);
    else if (horizontal)
        setCursor(Qt::SizeHorCursor);
    else if (vertical)
        setCursor(Qt::SizeVerCursor);
    else
        setCursor(!selection_.isEmpty() && logicalSelection().contains(position) ? Qt::SizeAllCursor
                                                                                 : Qt::CrossCursor);
}
void SelectionOverlay::dragTo(QPointF position)
{
    if (dragMode_ == DragMode::Create)
        selection_ = normalizedPixelSelection(press_, position, frame_.display.devicePixelRatio,
                                              frame_.pixels.size());
    else if (dragMode_ == DragMode::Move)
        selection_ = movedPixelSelection(initialSelection_, position - press_,
                                         frame_.display.devicePixelRatio, frame_.pixels.size());
    else if (dragMode_ == DragMode::Resize)
        selection_ = resizedPixelSelection(initialSelection_, resizeEdges_, position - press_,
                                           frame_.display.devicePixelRatio, frame_.pixels.size());
    updateCursor(position);
    update();
}
void SelectionOverlay::paintEvent(QPaintEvent*)
{
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // QImage 带目标屏 DPR，以原像素绘制，不生成缩放副本。
        painter.drawImage(QPointF(0, 0), frame_.pixels);
        const qreal scale = frame_.display.devicePixelRatio;
        const QRectF region = logicalSelection();
        QPainterPath shade;
        shade.addRect(rect());
        if (!selection_.isEmpty())
            shade.addRect(region);
        painter.fillPath(shade, QColor(0, 0, 0, 75));
        painter.setPen(QPen(Qt::white, 1.0 / scale));
        if (!selection_.isEmpty())
        {
            painter.drawRect(region);
            if (dragMode_ != DragMode::Create)
            {
                painter.setBrush(QColor(245, 130, 35));
                const qreal x[] = {region.left(), region.center().x(), region.right()};
                const qreal y[] = {region.top(), region.center().y(), region.bottom()};
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        if (row != 1 || column != 1)
                            painter.drawRect(QRectF(x[column] - 3, y[row] - 3, 6, 6));
            }
        }
        const QString text =
            QStringLiteral("%1 × %2 像素   拖拽框选 · 内部移动 · 边角调整 · Esc 取消")
                .arg(selection_.width())
                .arg(selection_.height());
        const QRect box(16, 16, std::max(0, std::min(width() - 32, 580)), 36);
        painter.fillRect(box, QColor(20, 20, 20, 220));
        painter.setPen(Qt::white);
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
    if (finished_ || event->button() != Qt::LeftButton)
        return;
    press_ = event->position();
    initialSelection_ = selection_;
    resizeEdges_ = edgesAt(press_);
    if (resizeEdges_)
        dragMode_ = DragMode::Resize;
    else if (!selection_.isEmpty() && logicalSelection().contains(press_))
        dragMode_ = DragMode::Move;
    else
    {
        dragMode_ = DragMode::Create;
        selection_ = {};
    }
    updateCursor(press_);
    update();
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (dragMode_ != DragMode::None)
        dragTo(event->position());
    else
        updateCursor(event->position());
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragMode_ == DragMode::None || event->button() != Qt::LeftButton)
        return;
    dragTo(event->position());
    dragMode_ = DragMode::None;
    updateCursor(event->position());
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
