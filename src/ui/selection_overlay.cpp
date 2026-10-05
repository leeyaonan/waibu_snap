#include "ui/selection_overlay.h"
#include "core/window_snapping.h"
#include "output/annotation_renderer.h"
#include "session/monotonic_clock.h"
#include "session/session_metrics.h"
#include "ui/annotation_text_edit.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
namespace
{
constexpr qreal handleRadius = 6;
constexpr qreal windowClickDistance = 3;
}
SelectionOverlay::SelectionOverlay(CaptureFrame frame, OverlayActions actions,
                                   QVector<QRect> windows, QString snappingError)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      frame_(std::move(frame)), actions_(std::move(actions)), windows_(std::move(windows))
{
    if (!snappingError.isEmpty() || windows_.isEmpty())
    {
        windows_.clear();
        snappingNotice_ = QStringLiteral("窗口吸附不可用，请手动框选");
    }
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
    statusTimeout_.setSingleShot(true);
    connect(&statusTimeout_, &QTimer::timeout, this,
            [this]
            {
                if (status_)
                    status_->hide();
                updateToolbar();
            });
}
void SelectionOverlay::ensureToolbar()
{
    if (toolbar_)
        return;
    style_.fontFamily = font().family();
    // 选区成立后才创建工具和快捷键，未选区首帧不初始化标注控件 / 文本排版。
    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("selectionToolbar"));
    toolbar_->setCursor(Qt::ArrowCursor);
    toolbar_->setFixedWidth(std::min(560, width()));
    toolbar_->setStyleSheet(QStringLiteral(
        "QWidget#selectionToolbar { background: #202020; border-radius: 6px; }"
        "QPushButton { color: white; background: #404040; padding: 6px; border-radius: 4px; }"
        "QPushButton:hover { background: #705030; }"
        "QPushButton:checked { background: #a65316; }"
        "QPushButton:disabled { color: #888888; background: #303030; }"
        "QComboBox { color: white; background: #404040; padding: 3px; }"
        "QComboBox QAbstractItemView { color: white; background: #303030; }"
        "QLabel { color: white; }"));
    auto* layout = new QVBoxLayout(toolbar_);
    auto* tools = new QGridLayout;
    const QString names[] = {QStringLiteral("矩形"), QStringLiteral("椭圆"),
                             QStringLiteral("直线"), QStringLiteral("箭头"),
                             QStringLiteral("画笔"), QStringLiteral("文本")};
    const QString objects[] = {
        QStringLiteral("rectangleToolButton"), QStringLiteral("ellipseToolButton"),
        QStringLiteral("lineToolButton"),      QStringLiteral("arrowToolButton"),
        QStringLiteral("freehandToolButton"),  QStringLiteral("textToolButton")};
    const int columns = width() >= 480 ? 6 : 3;
    for (int index = 0; index < 6; ++index)
    {
        auto* button = new QPushButton(names[index], toolbar_);
        button->setObjectName(objects[index]);
        button->setToolTip(names[index] + QStringLiteral("：点击启用，再次点击恢复选区调整"));
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        toolButtons_.append(button);
        tools->addWidget(button, index / columns, index % columns);
        connect(button, &QPushButton::clicked, this,
                [this, index] { activateTool(static_cast<AnnotationType>(index)); });
    }
    layout->addLayout(tools);
    auto* options = new QHBoxLayout;
    auto* colors = new QComboBox(toolbar_);
    colors->setObjectName(QStringLiteral("annotationColorCombo"));
    colors->setAccessibleName(QStringLiteral("标注颜色"));
    colors->setToolTip(QStringLiteral("标注颜色"));
    const QString colorNames[] = {QStringLiteral("红"), QStringLiteral("黄"), QStringLiteral("蓝")};
    for (int index = 0; index < 3; ++index)
    {
        QPixmap swatch(14, 14);
        swatch.fill(annotationColors()[index]);
        colors->addItem(QIcon(swatch), QStringLiteral("颜色：") + colorNames[index]);
    }
    auto* widths = new QComboBox(toolbar_);
    widths->setObjectName(QStringLiteral("annotationWidthCombo"));
    widths->setAccessibleName(QStringLiteral("图形线宽（物理像素）"));
    widths->setToolTip(QStringLiteral("图形线宽（物理像素）"));
    const QString levels[] = {QStringLiteral("细"), QStringLiteral("中"), QStringLiteral("粗")};
    for (int index = 0; index < 3; ++index)
        widths->addItem(
            QStringLiteral("%1 %2px").arg(levels[index]).arg(annotationLineWidths[index]));
    widths->setCurrentIndex(1);
    auto* sizes = new QComboBox(toolbar_);
    sizes->setObjectName(QStringLiteral("annotationTextSizeCombo"));
    sizes->setAccessibleName(QStringLiteral("文本字号（物理像素）"));
    sizes->setToolTip(QStringLiteral("文本字号（物理像素）"));
    for (int size : annotationTextSizes)
        sizes->addItem(QStringLiteral("字 %1px").arg(size));
    sizes->setCurrentIndex(1);
    sizes->setEnabled(false);
    options->addWidget(colors);
    options->addWidget(widths);
    options->addWidget(sizes);
    layout->addLayout(options);
    connect(colors, &QComboBox::currentIndexChanged, this,
            [this](int index) { style_.color = annotationColors()[index]; });
    connect(widths, &QComboBox::currentIndexChanged, this,
            [this](int index) { style_.lineWidth = annotationLineWidths[index]; });
    connect(sizes, &QComboBox::currentIndexChanged, this,
            [this](int index) { style_.textSize = annotationTextSizes[index]; });
    const auto makeButton = [this](const QString& name, const QString& object)
    {
        auto* button = new QPushButton(name, toolbar_);
        button->setObjectName(object);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };
    auto* buttons = new QHBoxLayout;
    undoButton_ = makeButton(QStringLiteral("撤销"), QStringLiteral("undoAnnotationButton"));
    redoButton_ = makeButton(QStringLiteral("重做"), QStringLiteral("redoAnnotationButton"));
    auto* copy = makeButton(QStringLiteral("复制"), QStringLiteral("copyButton"));
    auto* save = makeButton(QStringLiteral("保存"), QStringLiteral("saveButton"));
    auto* pin = makeButton(QStringLiteral("钉到屏幕"), QStringLiteral("pinButton"));
    auto* cancel = makeButton(QStringLiteral("取消"), QStringLiteral("cancelButton"));
    for (auto* button : {undoButton_, redoButton_, copy, save, pin, cancel})
        buttons->addWidget(button);
    layout->addLayout(buttons);
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    redoShortcut_ = new QShortcut(QKeySequence::Redo, this);
    undoButton_->setToolTip(
        QStringLiteral("撤销标注（%1）")
            .arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)));
    redoButton_->setToolTip(
        QStringLiteral("重做标注（%1）")
            .arg(QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText)));
    connect(undoShortcut_, &QShortcut::activated, this, &SelectionOverlay::undoAnnotation);
    connect(redoShortcut_, &QShortcut::activated, this, &SelectionOverlay::redoAnnotation);
    connect(undoButton_, &QPushButton::clicked, this, &SelectionOverlay::undoAnnotation);
    connect(redoButton_, &QPushButton::clicked, this, &SelectionOverlay::redoAnnotation);
    status_ = new QLabel(toolbar_);
    status_->setObjectName(QStringLiteral("outputStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->hide();
    layout->addWidget(status_);
    connect(copy, &QPushButton::clicked, this, &SelectionOverlay::copySelection);
    connect(save, &QPushButton::clicked, this, &SelectionOverlay::saveSelection);
    connect(pin, &QPushButton::clicked, this, &SelectionOverlay::pinSelection);
    connect(cancel, &QPushButton::clicked, this, [this] { complete(cancelledSessionOutcome); });
    for (QWidget* widget : toolbar_->findChildren<QWidget*>())
    {
        widget->setMouseTracking(true);
        widget->installEventFilter(this);
    }
    toolbar_->setMouseTracking(true);
    toolbar_->installEventFilter(this);
}
void SelectionOverlay::updateToolbar()
{
    if (finished_ || selection_.isEmpty() || dragMode_ == DragMode::Create)
    {
        if (toolbar_)
            toolbar_->hide();
        if (undoShortcut_)
        {
            undoShortcut_->setEnabled(false);
            redoShortcut_->setEnabled(false);
        }
        return;
    }
    ensureToolbar();
    const bool editingText = textEditor_ && textEditor_->isVisible();
    undoButton_->setEnabled(history_.canUndo());
    redoButton_->setEnabled(history_.canRedo());
    undoShortcut_->setEnabled(!editingText && !saveDialogOpen_ && dragMode_ == DragMode::None);
    redoShortcut_->setEnabled(!editingText && !saveDialogOpen_ && dragMode_ == DragMode::None);
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
    ensureToolbar();
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
    finishText(true);
    const ImageOutputResult result =
        actions_.copyImage(renderAnnotatedSelection(frame_.pixels, annotations(), selection_));
    if (result.success)
        complete(copiedSessionOutcome);
    else
        showStatus(QStringLiteral("复制失败：%1 点击「复制」重试。").arg(result.explanation),
                   false);
}
void SelectionOverlay::pinSelection()
{
    if (finished_ || saveDialogOpen_ || selection_.isEmpty() || dragMode_ != DragMode::None)
        return;
    finishText(true);
    const ImageOutputResult result =
        actions_.pinImage
            ? actions_.pinImage(renderAnnotatedSelection(frame_.pixels, annotations(), selection_))
            : ImageOutputResult{false, QStringLiteral("贴图管理器不可用。")};
    if (result.success)
        complete(pinnedSessionOutcome);
    else
        showStatus(QStringLiteral("钉图失败：%1 点击「钉到屏幕」重试。").arg(result.explanation),
                   false);
}
QPoint SelectionOverlay::selectionGlobalPosition() const
{
    return frame_.display.logicalGeometry.topLeft() + logicalSelection().topLeft().toPoint();
}
void SelectionOverlay::saveSelection()
{
    if (finished_ || saveDialogOpen_ || selection_.isEmpty() || dragMode_ != DragMode::None)
        return;
    finishText(true);
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
    finishText(true);
    showStatus(QStringLiteral("正在保存 PNG…"), false);
    const ImageOutputResult result =
        exportPngToPath(renderAnnotatedSelection(frame_.pixels, annotations(), selection_), path);
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
    if (!pixels.isEmpty())
        hoveredWindowPixels_ = {};
    markDirty();
}
void SelectionOverlay::markDirty()
{
    saved_ = false;
    statusTimeout_.stop();
    if (status_)
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
    if (activeTool_ && !selection_.isEmpty())
    {
        setCursor(*activeTool_ == AnnotationType::Text ? Qt::IBeamCursor : Qt::CrossCursor);
        return;
    }
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
    maximumPressDistance_ = std::max(
        maximumPressDistance_, std::hypot(position.x() - press_.x(), position.y() - press_.y()));
    if (dragMode_ == DragMode::Annotate)
        updateAnnotation(position);
    else if (dragMode_ == DragMode::Create)
    {
        if (pressedWindow_ < 0 || maximumPressDistance_ > windowClickDistance)
            setSelection(normalizedPixelSelection(press_, position, frame_.display.devicePixelRatio,
                                                  frame_.pixels.size()));
    }
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
void SelectionOverlay::updateHover(QPointF position)
{
    const int index =
        selection_.isEmpty() && dragMode_ == DragMode::None && !finished_ && !saveDialogOpen_
            ? windowAt(windows_, position)
            : -1;
    const QRect pixels =
        index >= 0 ? windowPixelSelection(windows_[index], frame_.display.devicePixelRatio,
                                          frame_.pixels.size())
                   : QRect();
    if (pixels != hoveredWindowPixels_)
    {
        hoveredWindowPixels_ = pixels;
        update();
    }
}
void SelectionOverlay::leaveEvent(QEvent*)
{
    hoveredWindowPixels_ = {};
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
        painter.save();
        painter.scale(1 / scale, 1 / scale);
        paintAnnotations(painter, annotations());
        if (draft_)
            paintAnnotation(painter, *draft_);
        painter.restore();
        const QRectF region = logicalSelection();
        QPainterPath shade;
        shade.addRect(rect());
        if (!selection_.isEmpty())
            shade.addRect(region);
        painter.fillPath(shade, QColor(0, 0, 0, 75));
        if (!hoveredWindowPixels_.isEmpty())
        {
            const QRectF hover(hoveredWindowPixels_.x() / scale, hoveredWindowPixels_.y() / scale,
                               hoveredWindowPixels_.width() / scale,
                               hoveredWindowPixels_.height() / scale);
            painter.fillRect(hover, QColor(60, 170, 255, 55));
            painter.setPen(QPen(QColor(90, 200, 255), 2, Qt::DashLine));
            painter.drawRect(hover);
        }
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
        const QString hint = activeTool_ ? QStringLiteral("拖拽标注 · 再点工具调整选区 · Esc 取消")
                                         : QStringLiteral("单击吸附 · 拖拽框选 · Esc 取消");
        const QString text = QStringLiteral("%1 × %2 像素   %3")
                                 .arg(selection_.width())
                                 .arg(selection_.height())
                                 .arg(hint);
        const QRect box(16, 16, std::max(0, std::min(width() - 32, 580)), 36);
        painter.fillRect(box, QColor(20, 20, 20, 220));
        painter.setPen(Qt::white);
        painter.drawText(box.adjusted(12, 0, -12, 0), Qt::AlignVCenter, text);
        if (!snappingNotice_.isEmpty())
        {
            const QRect notice = box.translated(0, 40);
            painter.fillRect(notice, QColor(20, 20, 20, 220));
            painter.drawText(notice.adjusted(12, 0, -12, 0), Qt::AlignVCenter, snappingNotice_);
        }
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
    finishText(true);
    setFocus(Qt::MouseFocusReason);
    press_ = event->position();
    if (activeTool_ && !selection_.isEmpty())
    {
        if (*activeTool_ == AnnotationType::Text)
            startText(press_);
        else
        {
            draft_ = Annotation{*activeTool_,          style_, physicalPoint(press_),
                                physicalPoint(press_), {},     {}};
            if (*activeTool_ == AnnotationType::Freehand)
                appendFreehandPoint(*draft_, draft_->first);
            dragMode_ = DragMode::Annotate;
            updateToolbar();
            update();
        }
        return;
    }
    initialSelection_ = selection_;
    maximumPressDistance_ = 0;
    pressedWindow_ = selection_.isEmpty() ? windowAt(windows_, press_) : -1;
    hoveredWindowPixels_ = {};
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
    {
        updateHover(event->position());
        updateCursor(event->position());
    }
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (finished_ || saveDialogOpen_ || dragMode_ == DragMode::None ||
        event->button() != Qt::LeftButton)
        return;
    dragTo(event->position());
    if (dragMode_ == DragMode::Create && pressedWindow_ >= 0 &&
        maximumPressDistance_ <= windowClickDistance)
        setSelection(windowPixelSelection(windows_[pressedWindow_], frame_.display.devicePixelRatio,
                                          frame_.pixels.size()));
    if (dragMode_ == DragMode::Annotate && draft_)
    {
        if (history_.add(std::move(*draft_)))
            markDirty();
        draft_.reset();
    }
    dragMode_ = DragMode::None;
    updateHover(event->position());
    updateCursor(event->position());
    updateToolbar();
    update();
}
void SelectionOverlay::keyPressEvent(QKeyEvent* event)
{
    if (finished_ || saveDialogOpen_)
        event->accept();
    else if (textEditor_ && textEditor_->isVisible())
    {
        // 焦点层即使有事件泄漏到覆盖层，也不能取消会话或撤销已提交标注。
        if (event->key() == Qt::Key_Escape && !textEditor_->isComposing())
            finishText(false);
        event->accept();
    }
    else if (event->key() == Qt::Key_Escape)
        complete(cancelledSessionOutcome);
    else if (event->matches(QKeySequence::Undo))
        undoAnnotation();
    else if (event->matches(QKeySequence::Redo))
        redoAnnotation();
    else
        QWidget::keyPressEvent(event);
}
void SelectionOverlay::closeEvent(QCloseEvent* event)
{
    event->accept();
    if (!finished_)
    {
        hoveredWindowPixels_ = {};
        finished_ = true;
        discardAnnotations();
        emit finished({}, cancelledSessionOutcome);
    }
}
void SelectionOverlay::complete(int outcome)
{
    if (finished_)
        return;
    finished_ = true;
    hoveredWindowPixels_ = {};
    discardAnnotations();
    hide();
    emit finished(outcome == copiedSessionOutcome || outcome == pinnedSessionOutcome ? selection()
                                                                                     : QRect(),
                  outcome);
}
QPointF SelectionOverlay::physicalPoint(QPointF position) const
{
    const qreal scale = frame_.display.devicePixelRatio;
    return {std::clamp(position.x() * scale, qreal(0), qreal(frame_.pixels.width())),
            std::clamp(position.y() * scale, qreal(0), qreal(frame_.pixels.height()))};
}
void SelectionOverlay::activateTool(AnnotationType type)
{
    if (finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    finishText(true);
    activeTool_ = activeTool_ == type ? std::nullopt : std::optional<AnnotationType>(type);
    for (int index = 0; index < toolButtons_.size(); ++index)
        toolButtons_[index]->setChecked(activeTool_ == static_cast<AnnotationType>(index));
    toolbar_->findChild<QComboBox*>(QStringLiteral("annotationTextSizeCombo"))
        ->setEnabled(activeTool_ == AnnotationType::Text);
    toolbar_->findChild<QComboBox*>(QStringLiteral("annotationWidthCombo"))
        ->setEnabled(activeTool_ != AnnotationType::Text);
    setFocus(Qt::OtherFocusReason);
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::updateAnnotation(QPointF position)
{
    if (!draft_)
        return;
    const QPointF point = physicalPoint(position);
    if (draft_->type == AnnotationType::Freehand)
        appendFreehandPoint(*draft_, point);
    else
        draft_->last = point;
}
void SelectionOverlay::undoAnnotation()
{
    if (finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    finishText(true);
    if (history_.undo())
        markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::redoAnnotation()
{
    if (finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    finishText(true);
    if (history_.redo())
        markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::startText(QPointF position)
{
    if (!textEditor_)
    {
        textEditor_ = new AnnotationTextEdit(this);
        textEditor_->installEventFilter(this);
        connect(textEditor_, &AnnotationTextEdit::commitRequested, this,
                [this] { finishText(true); });
        connect(textEditor_, &AnnotationTextEdit::cancelRequested, this,
                [this] { finishText(false); });
    }
    textDraft_ = Annotation{AnnotationType::Text, style_, physicalPoint(position), {}, {}, {}};
    QFont editorFont = annotationFont(style_);
    // 编辑器在逻辑坐标中排版，支持非整数 DPR；提交后的文本按物理像素统一绘制。
    editorFont.setPointSizeF(style_.textSize * 72.0 /
                             (logicalDpiY() * frame_.display.devicePixelRatio));
    textEditor_->setFont(editorFont);
    QPalette palette = textEditor_->palette();
    palette.setColor(QPalette::Text, style_.color);
    palette.setColor(QPalette::Base, QColor(25, 25, 25));
    textEditor_->setPalette(palette);
    const QPoint point = position.toPoint();
    textEditor_->setGeometry(point.x(), point.y(), std::max(1, std::min(360, width() - point.x())),
                             std::max(1, std::min(140, height() - point.y())));
    textEditor_->clear();
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(Qt::MouseFocusReason);
    updateToolbar();
}
void SelectionOverlay::finishText(bool commit, bool restoreFocus)
{
    if (!textEditor_ || !textEditor_->isVisible())
        return;
    if (commit)
        QGuiApplication::inputMethod()->commit();
    else
        QGuiApplication::inputMethod()->reset();
    textEditor_->clearPreedit();
    textDraft_.text = textEditor_->toPlainText();
    textEditor_->hide();
    textEditor_->clear();
    if (commit && history_.add(std::move(textDraft_)))
        markDirty();
    textDraft_ = {};
    if (restoreFocus)
        setFocus(Qt::OtherFocusReason);
    updateToolbar();
    update();
}
bool SelectionOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == toolbar_ || qobject_cast<QLabel*>(watched)) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease))
    {
        // 空白 / 提示标签提交当前编辑并消化手势；按钮在 clicked 中提交，避免按下时移动工具栏。
        if (event->type() == QEvent::MouseButtonPress)
            finishText(true, false);
        event->accept();
        return true;
    }
    if (watched != textEditor_ && event->type() == QEvent::MouseMove)
    {
        // 子工具栏保持箭头光标，同时同步其下方画布的模式状态。
        const auto* mouse = static_cast<QMouseEvent*>(event);
        updateCursor(mapFromGlobal(mouse->globalPosition().toPoint()));
    }
    if (watched == textEditor_ && event->type() == QEvent::FocusOut)
    {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        // 候选窗口 / 系统面板的临时焦点变化不提交；点击其他控件才提交。
        if (reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason)
            finishText(true, false);
    }
    return QWidget::eventFilter(watched, event);
}
void SelectionOverlay::discardAnnotations()
{
    finishText(false);
    history_.clear();
    draft_.reset();
    activeTool_.reset();
    updateToolbar();
}
}
