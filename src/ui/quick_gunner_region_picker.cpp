#include "ui/quick_gunner_region_picker.h"
#include "core/quick_gunner.h"
#include "core/selection_geometry.h"
#include "interfaces/overlay_window_behavior.h"
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
namespace waibusnap
{
QuickGunnerRegionPicker::QuickGunnerRegionPicker(CaptureFrame frame, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      frame_(std::move(frame))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setGeometry(frame_.display.logicalGeometry);
    winId();
    raiseOverlayAboveSystemChrome(windowHandle());
    // 原生窗口抬升后重新定位，避免 Cocoa 把菜单栏 / Dock 附近的机位夹回可用区。
    setGeometry(frame_.display.logicalGeometry);
    hint_ = new QLabel(this);
    hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hint_->setStyleSheet(QStringLiteral("QLabel { color: white; background: rgba(0,0,0,200); "
                                        "padding: 8px; border-radius: 4px; }"));
    updateHint();
}
void QuickGunnerRegionPicker::updateHint(bool tooSmall)
{
    hint_->setText(QStringLiteral("%1×%2 px\n%3\nEnter 确认 · Esc 取消")
                       .arg(selection_.width())
                       .arg(selection_.height())
                       .arg(tooSmall ? QStringLiteral("机位最小为 16×16 物理像素，请重新拖框。")
                                     : QStringLiteral("拖拽框选拍摄区域")));
    hint_->adjustSize();
    hint_->move(qMax(0, (width() - hint_->width()) / 2), 12);
}
void QuickGunnerRegionPicker::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.drawImage(QRectF(rect()), frame_.pixels, QRectF(frame_.pixels.rect()));
    QPainterPath shade;
    shade.addRect(QRectF(rect()));
    const qreal dpr = frame_.display.devicePixelRatio;
    const QRectF selected(selection_.x() / dpr, selection_.y() / dpr, selection_.width() / dpr,
                          selection_.height() / dpr);
    if (!selection_.isEmpty())
        shade.addRect(selected);
    painter.fillPath(shade, QColor(0, 0, 0, 140));
    if (!selection_.isEmpty())
    {
        QPen pen(Qt::white, 1);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawRect(selected);
    }
}
void QuickGunnerRegionPicker::dragTo(QPointF point)
{
    selection_ = normalizedPixelSelection(press_, point, frame_.display.devicePixelRatio,
                                          quickGunnerPixelSize(frame_.display));
    updateHint();
    update();
}
void QuickGunnerRegionPicker::mousePressEvent(QMouseEvent* event)
{
    if (finished_ || event->button() != Qt::LeftButton)
        return;
    dragging_ = true;
    press_ = event->position();
    dragTo(press_);
}
void QuickGunnerRegionPicker::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_ && !finished_)
        dragTo(event->position());
}
void QuickGunnerRegionPicker::mouseReleaseEvent(QMouseEvent* event)
{
    if (!dragging_ || finished_ || event->button() != Qt::LeftButton)
        return;
    dragTo(event->position());
    dragging_ = false;
}
void QuickGunnerRegionPicker::keyPressEvent(QKeyEvent* event)
{
    if (finished_)
        return;
    if (event->key() == Qt::Key_Escape)
        close();
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (dragging_ || !QuickGunnerRegion{frame_.display, selection_}.isValid())
        {
            updateHint(true);
            return;
        }
        finished_ = true;
        emit regionSelected(selection_);
        close();
    }
    else
        QWidget::keyPressEvent(event);
}
void QuickGunnerRegionPicker::closeEvent(QCloseEvent* event)
{
    if (!finished_)
    {
        finished_ = true;
        emit cancelled();
    }
    event->accept();
}
}
