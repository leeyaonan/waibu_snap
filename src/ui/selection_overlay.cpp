#include "ui/selection_overlay.h"
#include "session/monotonic_clock.h"
#include "session/session_metrics.h"
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
constexpr qreal handleRadius = 6;
}
SelectionOverlay::SelectionOverlay(CaptureFrame frame, OverlayActions actions)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      frame_(std::move(frame)), actions_(std::move(actions))
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setGeometry(frame_.display.logicalGeometry);
    setWindowTitle(QStringLiteral("WaibuSnap 选区"));
    if (!actions_.copyImage)
        actions_.copyImage = copyImageToClipboard;
    if (!actions_.chooseSavePath)
        actions_.chooseSavePath = [](QWidget* parent, const QString& suggestion)
        {
            return QFileDialog::getSaveFileName(parent, QStringLiteral("保存截图为 PNG"),
                                                suggestion, QStringLiteral("PNG 图片 (*.png)"));
        };
    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("selectionToolbar"));
    toolbar_->setCursor(Qt::ArrowCursor);
    toolbar_->setFixedWidth(260);
    toolbar_->setStyleSheet(QStringLiteral(
        "QWidget#selectionToolbar { background: #202020; border-radius: 6px; }"
        "QPushButton { color: white; background: #404040; padding: 8px; border-radius: 4px; }"
        "QPushButton:hover { background: #705030; }"
        "QLabel { color: white; }"));
    auto* layout = new QVBoxLayout(toolbar_);
    auto* buttons = new QHBoxLayout;
    auto* copy = new QPushButton(QStringLiteral("复制"), toolbar_);
    copy->setObjectName(QStringLiteral("copyButton"));
    auto* save = new QPushButton(QStringLiteral("保存"), toolbar_);
    save->setObjectName(QStringLiteral("saveButton"));
    auto* cancel = new QPushButton(QStringLiteral("取消"), toolbar_);
    cancel->setObjectName(QStringLiteral("cancelButton"));
    buttons->addWidget(copy);
    buttons->addWidget(save);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    status_ = new QLabel(toolbar_);
    status_->setObjectName(QStringLiteral("outputStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->hide();
    layout->addWidget(status_);
    toolbar_->hide();
    connect(copy, &QPushButton::clicked, this, &SelectionOverlay::copySelection);
    connect(save, &QPushButton::clicked, this, &SelectionOverlay::saveSelection);
    connect(cancel, &QPushButton::clicked, this, [this] { complete(cancelledSessionOutcome); });
    statusTimeout_.setSingleShot(true);
    connect(&statusTimeout_, &QTimer::timeout, this,
            [this]
            {
                status_->hide();
                updateToolbar();
            });
}
void SelectionOverlay::updateToolbar()
{
    if (finished_ || selection_.isEmpty() || dragMode_ == DragMode::Create)
    {
        toolbar_->hide();
        return;
    }
    toolbar_->adjustSize();
    const QRectF region = logicalSelection();
    const int x = std::clamp(qRound(region.right()) - toolbar_->width(), 0,
                             std::max(0, width() - toolbar_->width()));
    int y = qRound(region.bottom()) + 10;
    if (y + toolbar_->height() > height())
        y = qRound(region.top()) - toolbar_->height() - 10;
    y = std::clamp(y, 0, std::max(0, height() - toolbar_->height()));
    toolbar_->move(x, y);
    toolbar_->setEnabled(dragMode_ == DragMode::None && !saveDialogOpen_);
    toolbar_->show();
}
void SelectionOverlay::showStatus(const QString& text, bool temporary)
{
    statusTimeout_.stop();
    status_->setText(text);
    status_->show();
    updateToolbar();
    if (temporary)
        statusTimeout_.start(3000);
}
void SelectionOverlay::copySelection()
{
    if (finished_ || saveDialogOpen_ || selection_.isEmpty() || dragMode_ != DragMode::None)
        return;
    const ImageOutputResult result =
        actions_.copyImage(cropFrozenSelection(frame_.pixels, selection_));
    if (result.success)
        complete(copiedSessionOutcome);
    else
        showStatus(QStringLiteral("复制失败：%1 点击「复制」重试。").arg(result.explanation),
                   false);
}
void SelectionOverlay::saveSelection()
{
    if (finished_ || saveDialogOpen_ || selection_.isEmpty() || dragMode_ != DragMode::None)
        return;
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString suggestion = suggestedPngPath(directory);
    saveDialogOpen_ = true;
    toolbar_->setEnabled(false);
    QPointer<SelectionOverlay> self(this);
    const auto chooseSavePath = actions_.chooseSavePath;
    QString path;
    while (true)
    {
        const QString chosen = chooseSavePath(this, suggestion);
        // 原生面板的嵌套事件循环可能遇到显示器变更或应用退出，不能访问已销毁会话。
        if (!self)
            return;
        if (finished_ || chosen.isEmpty())
            break;
        path = pngFilePath(chosen);
        if (path == chosen || !QFileInfo::exists(path))
            break;
        // 补后缀可能指向另一个已有文件，必须让原生面板确认最终 PNG 路径。
        suggestion = path;
        path.clear();
        showStatus(QStringLiteral("补全后缀后文件已存在，请在保存面板确认覆盖。"), false);
    }
    saveDialogOpen_ = false;
    if (finished_)
        return;
    updateToolbar();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    if (!path.isEmpty())
        exportToPath(path);
}
bool SelectionOverlay::exportToPath(const QString& path)
{
    if (finished_ || saveDialogOpen_ || selection_.isEmpty() || dragMode_ != DragMode::None ||
        path.isEmpty())
        return false;
    showStatus(QStringLiteral("正在保存 PNG…"), false);
    const ImageOutputResult result =
        exportPngToPath(cropFrozenSelection(frame_.pixels, selection_), path);
    if (!result.success)
    {
        showStatus(
            QStringLiteral("保存失败：%1 点击「保存」选择路径并重试。").arg(result.explanation),
            false);
        return false;
    }
    saved_ = true;
    showStatus(QStringLiteral("已保存：%1").arg(QFileInfo(pngFilePath(path)).fileName()), true);
    return true;
}
void SelectionOverlay::setSelection(QRect pixels)
{
    if (pixels == selection_)
        return;
    selection_ = pixels;
    saved_ = false;
    statusTimeout_.stop();
    status_->hide();
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
        setSelection(normalizedPixelSelection(press_, position, frame_.display.devicePixelRatio,
                                              frame_.pixels.size()));
    else if (dragMode_ == DragMode::Move)
        setSelection(movedPixelSelection(initialSelection_, position - press_,
                                         frame_.display.devicePixelRatio, frame_.pixels.size()));
    else if (dragMode_ == DragMode::Resize)
        setSelection(resizedPixelSelection(initialSelection_, resizeEdges_, position - press_,
                                           frame_.display.devicePixelRatio, frame_.pixels.size()));
    updateCursor(position);
    updateToolbar();
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
    if (finished_ || saveDialogOpen_ || event->button() != Qt::LeftButton)
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
        setSelection({});
    }
    updateCursor(press_);
    updateToolbar();
    update();
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (finished_ || saveDialogOpen_)
        return;
    if (dragMode_ != DragMode::None)
        dragTo(event->position());
    else
        updateCursor(event->position());
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (finished_ || saveDialogOpen_ || dragMode_ == DragMode::None ||
        event->button() != Qt::LeftButton)
        return;
    dragTo(event->position());
    dragMode_ = DragMode::None;
    updateCursor(event->position());
    updateToolbar();
    update();
}
void SelectionOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && saveDialogOpen_)
        event->accept();
    else if (event->key() == Qt::Key_Escape)
        complete(cancelledSessionOutcome);
    else
        QWidget::keyPressEvent(event);
}
void SelectionOverlay::closeEvent(QCloseEvent* event)
{
    event->accept();
    if (!finished_)
    {
        finished_ = true;
        emit finished({}, cancelledSessionOutcome);
    }
}
void SelectionOverlay::complete(int outcome)
{
    if (finished_)
        return;
    finished_ = true;
    hide();
    emit finished(outcome == copiedSessionOutcome ? selection() : QRect(), outcome);
}
}
