#include "ui/selection_overlay.h"
#include "core/selection_assistance.h"
#include "core/window_snapping.h"
#include "interfaces/overlay_window_behavior.h"
#include "output/annotation_renderer.h"
#include "session/monotonic_clock.h"
#include "session/session_metrics.h"
#include "ui/annotation_text_edit.h"
#include "ui/ocr_dialog.h"
#include "ui/text_recognition_task.h"
#include "ui/toolbar_icons.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFocusEvent>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <algorithm>
#include <cmath>
#include <limits>
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
    if (!actions_.setClipboardText)
        actions_.setClipboardText = [](const QString& text)
        { QApplication::clipboard()->setText(text); };
    recognitionProgress_.setSingleShot(true);
    connect(&recognitionProgress_, &QTimer::timeout, this,
            [this]
            {
                if (textMode_ && recognizingText_ && !finished_)
                    showStatus(QStringLiteral("正在识别文字…"), false);
            });
    statusTimeout_.setSingleShot(true);
    connect(&statusTimeout_, &QTimer::timeout, this,
            [this]
            {
                if (status_)
                    status_->hide();
                updateToolbar();
            });
    magnifierTimeout_.setSingleShot(true);
    magnifierTimeout_.setTimerType(Qt::PreciseTimer);
    connect(&magnifierTimeout_, &QTimer::timeout, this, &SelectionOverlay::hideMagnifier);
}
void SelectionOverlay::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!overlayLevelRaised_)
    {
        raiseOverlayAboveSystemChrome(windowHandle());
        overlayLevelRaised_ = true;
    }
}
void SelectionOverlay::ensureToolbar()
{
    if (toolbar_)
        return;
    style_.fontFamily = font().family();
    // 选区成立后才创建工具和快捷键，未选区首帧不初始化标注控件 / 文本排版。
    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("selectionToolbar"));
    optionsBar_ = new QWidget(this);
    optionsBar_->setObjectName(QStringLiteral("annotationOptionsBar"));
    const QString buttonsStyle = QStringLiteral(
        "QPushButton { background: transparent; padding: 0; border: none; border-radius: 6px; }"
        "QPushButton:hover { background: #3a3a3a; }"
        "QPushButton:checked { background: #a65316; }"
        "QPushButton:disabled { background: transparent; }");
    toolbar_->setStyleSheet(
        QStringLiteral("QWidget#selectionToolbar { background: #202020; border: 1px solid "
                       "#3a3a3a; border-radius: 10px; }") +
        buttonsStyle);
    optionsBar_->setStyleSheet(
        QStringLiteral("QWidget#annotationOptionsBar { background: #202020; border: 1px solid "
                       "#3a3a3a; border-radius: 8px; }") +
        buttonsStyle);
    const auto makeButton =
        [](QWidget* parent, const QString& object, const QString& tip, ToolbarIcon icon)
    {
        auto* button = new QPushButton(parent);
        button->setText(QString());
        button->setObjectName(object);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setIcon(toolbarIcon(icon));
        button->setIconSize(QSize(18, 18));
        button->setFixedSize(28, 28);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };
    const auto makeSeparator = [](QWidget* parent)
    {
        auto* separator = new QLabel(parent);
        separator->setStyleSheet(QStringLiteral("background: #4a4a4a; border: none;"));
        separator->setFixedSize(1, 22);
        return separator;
    };
    for (int index = 0; index < 3; ++index)
        separators_.append(makeSeparator(toolbar_));
    optionsSeparator_ = makeSeparator(optionsBar_);
    const QString names[] = {QStringLiteral("矩形"), QStringLiteral("椭圆"),
                             QStringLiteral("直线"), QStringLiteral("箭头"),
                             QStringLiteral("画笔"), QStringLiteral("文本"),
                             QStringLiteral("遮盖"), QStringLiteral("马赛克")};
    const QString objects[] = {
        QStringLiteral("rectangleToolButton"), QStringLiteral("ellipseToolButton"),
        QStringLiteral("lineToolButton"),      QStringLiteral("arrowToolButton"),
        QStringLiteral("freehandToolButton"),  QStringLiteral("textToolButton"),
        QStringLiteral("coverToolButton"),     QStringLiteral("mosaicToolButton")};
    for (int index = 0; index < 8; ++index)
    {
        QString tip = names[index] + QStringLiteral("：点击启用，再次点击恢复选区调整");
        if (index == static_cast<int>(AnnotationType::Mosaic))
            tip = QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖");
        auto* button = makeButton(toolbar_, objects[index], tip, static_cast<ToolbarIcon>(index));
        button->setCheckable(true);
        toolButtons_.append(button);
        connect(button, &QPushButton::clicked, this,
                [this, index] { activateTool(static_cast<AnnotationType>(index)); });
    }
    ocrButton_ = makeButton(toolbar_, QStringLiteral("ocrToolButton"),
                            QStringLiteral("取字（文字识别）"), ToolbarIcon::Ocr);
    ocrButton_->setCheckable(true);
    connect(ocrButton_, &QPushButton::clicked, this, &SelectionOverlay::toggleTextMode);
    const QString actionObjects[] = {QStringLiteral("undoAnnotationButton"),
                                     QStringLiteral("redoAnnotationButton"),
                                     QStringLiteral("copyButton"),
                                     QStringLiteral("saveButton"),
                                     QStringLiteral("pinButton"),
                                     QStringLiteral("cancelButton")};
    const QString actionTips[] = {
        QStringLiteral("撤销标注（%1）")
            .arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)),
        QStringLiteral("重做标注（%1）")
            .arg(QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText)),
        QStringLiteral("复制（%1）")
            .arg(QKeySequence(QKeySequence::Copy).toString(QKeySequence::NativeText)),
        QStringLiteral("保存（快速保存）"),
        QStringLiteral("钉到屏幕"),
        QStringLiteral("取消（Esc）")};
    for (int index = 0; index < 6; ++index)
        actionButtons_.append(makeButton(toolbar_, actionObjects[index], actionTips[index],
                                         static_cast<ToolbarIcon>(12 + index)));
    undoButton_ = actionButtons_[0];
    redoButton_ = actionButtons_[1];
    extractAllTextButton_ = makeButton(toolbar_, QStringLiteral("extractAllTextButton"),
                                       QStringLiteral("提取全文"), ToolbarIcon::ExtractAllText);
    extractAllTextButton_->hide();
    connect(extractAllTextButton_, &QPushButton::clicked, this, &SelectionOverlay::openOcrDialog);
    copyShortcut_ = new QShortcut(QKeySequence::Copy, this);
    connect(copyShortcut_, &QShortcut::activated, this, &SelectionOverlay::copyShortcut);
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    redoShortcut_ = new QShortcut(QKeySequence::Redo, this);
    connect(undoShortcut_, &QShortcut::activated, this, &SelectionOverlay::undoAnnotation);
    connect(redoShortcut_, &QShortcut::activated, this, &SelectionOverlay::redoAnnotation);
    connect(undoButton_, &QPushButton::clicked, this, &SelectionOverlay::undoAnnotation);
    connect(redoButton_, &QPushButton::clicked, this, &SelectionOverlay::redoAnnotation);
    connect(actionButtons_[2], &QPushButton::clicked, this, &SelectionOverlay::copySelection);
    connect(actionButtons_[3], &QPushButton::clicked, this, &SelectionOverlay::saveSelection);
    connect(actionButtons_[4], &QPushButton::clicked, this, &SelectionOverlay::pinSelection);
    connect(actionButtons_[5], &QPushButton::clicked, this,
            [this] { complete(cancelledSessionOutcome); });
    const QString colorNames[] = {QStringLiteral("红"), QStringLiteral("黄"), QStringLiteral("蓝")};
    const QString levels[] = {QStringLiteral("细"), QStringLiteral("中"), QStringLiteral("粗")};
    const QString sizeNames[] = {QStringLiteral("小"), QStringLiteral("中"), QStringLiteral("大")};
    for (int index = 0; index < 3; ++index)
    {
        auto* color = makeButton(optionsBar_, QStringLiteral("annotationColor%1Button").arg(index),
                                 colorNames[index], ToolbarIcon::Color);
        color->setIcon(toolbarIcon(ToolbarIcon::Color, annotationColors()[index]));
        auto* width = makeButton(
            optionsBar_, QStringLiteral("annotationWidth%1Button").arg(index),
            QStringLiteral("%1 %2 像素").arg(levels[index]).arg(annotationLineWidths[index]),
            ToolbarIcon::LineWidth);
        width->setIcon(toolbarIcon(ToolbarIcon::LineWidth, {}, annotationLineWidths[index] / 2.0));
        auto* size = makeButton(
            optionsBar_, QStringLiteral("annotationTextSize%1Button").arg(index),
            QStringLiteral("%1 %2 像素").arg(sizeNames[index]).arg(annotationTextSizes[index]),
            ToolbarIcon::TextSize);
        const qreal scales[] = {0.72, 0.86, 1.0};
        size->setIcon(toolbarIcon(ToolbarIcon::TextSize, {}, scales[index]));
        for (auto* button : {color, width, size})
        {
            button->setCheckable(true);
            button->setFocusPolicy(Qt::StrongFocus);
        }
        colorButtons_.append(color);
        widthButtons_.append(width);
        textSizeButtons_.append(size);
        connect(color, &QPushButton::clicked, this,
                [this, index]
                {
                    finishText(true, false);
                    style_.color = annotationColors()[index];
                    updateToolbar();
                });
        connect(width, &QPushButton::clicked, this,
                [this, index]
                {
                    style_.lineWidth = annotationLineWidths[index];
                    updateToolbar();
                });
        connect(size, &QPushButton::clicked, this,
                [this, index]
                {
                    finishText(true, false);
                    style_.textSize = annotationTextSizes[index];
                    updateToolbar();
                });
    }
    optionsBar_->hide();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("outputStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setIndent(0);
    status_->setStyleSheet(QStringLiteral(
        "background: #202020; color: #e8e8e8; border-radius: 8px; padding: 0 10px;"));
    status_->hide();
    for (auto* panel : {toolbar_, optionsBar_, static_cast<QWidget*>(status_)})
    {
        panel->setCursor(Qt::ArrowCursor);
        panel->setMouseTracking(true);
        panel->installEventFilter(this);
        for (QWidget* widget : panel->findChildren<QWidget*>())
        {
            widget->setMouseTracking(true);
            widget->installEventFilter(this);
        }
    }
}
QRect SelectionOverlay::availableRect() const
{
    const QRect available = frame_.display.availableLogicalGeometry.isEmpty()
                                ? frame_.display.logicalGeometry
                                : frame_.display.availableLogicalGeometry;
    const QRect local = available.translated(-frame_.display.logicalGeometry.topLeft()) & rect();
    return local.isEmpty() ? rect() : local;
}
void SelectionOverlay::layoutToolbar(int availableWidth)
{
    for (auto* separator : separators_)
        separator->hide();
    QVector<QWidget*> tools;
    for (auto* tool : toolButtons_)
        tools.append(tool);
    tools.append(ocrButton_);
    QVector<QWidget*> actions;
    if (textMode_)
        actions << extractAllTextButton_ << separators_[1];
    actions << undoButton_ << redoButton_ << separators_[2];
    for (int index = 2; index < actionButtons_.size(); ++index)
        actions.append(actionButtons_[index]);
    const auto rowWidth = [](const QVector<QWidget*>& row)
    {
        int result = 16 - 4;
        for (auto* widget : row)
            result += (qobject_cast<QLabel*>(widget) ? 17 : 28) + 4;
        return result;
    };
    const auto placeRow = [](const QVector<QWidget*>& row, int y)
    {
        int x = 8;
        for (auto* widget : row)
        {
            const bool separator = qobject_cast<QLabel*>(widget);
            widget->move(x + (separator ? 8 : 0), y + (separator ? 3 : 0));
            widget->show();
            x += (separator ? 17 : 28) + 4;
        }
    };
    QVector<QWidget*> single = tools;
    single << separators_[0];
    single += actions;
    if (rowWidth(single) <= availableWidth)
    {
        toolbar_->setFixedSize(rowWidth(single), 44);
        placeRow(single, 8);
    }
    else if (availableWidth > 320 && rowWidth(tools) <= availableWidth)
    {
        toolbar_->setFixedSize(std::max(rowWidth(tools), rowWidth(actions)), 76);
        placeRow(tools, 8);
        placeRow(actions, 40);
    }
    else
    {
        // 320 逻辑像素使用三列工具网格，操作组仍紧凑成行。
        toolbar_->setFixedSize(std::max(108, rowWidth(actions)), 140);
        for (int index = 0; index < tools.size(); ++index)
        {
            tools[index]->move(8 + index % 3 * 32, 8 + index / 3 * 32);
            tools[index]->show();
        }
        placeRow(actions, 104);
    }
}
void SelectionOverlay::updateOptionsBar()
{
    const bool colors = !textMode_ && activeTool_ && activeTool_ != AnnotationType::Mosaic;
    const bool widths =
        colors && activeTool_ != AnnotationType::Text && activeTool_ != AnnotationType::Cover;
    const bool sizes = colors && activeTool_ == AnnotationType::Text;
    for (int index = 0; index < 3; ++index)
    {
        colorButtons_[index]->setVisible(colors);
        widthButtons_[index]->setVisible(widths);
        textSizeButtons_[index]->setVisible(sizes);
        colorButtons_[index]->setChecked(style_.color == annotationColors()[index]);
        widthButtons_[index]->setChecked(style_.lineWidth == annotationLineWidths[index]);
        textSizeButtons_[index]->setChecked(style_.textSize == annotationTextSizes[index]);
        colorButtons_[index]->move(4 + index * 32, 4);
        widthButtons_[index]->move(121 + index * 32, 4);
        textSizeButtons_[index]->move(121 + index * 32, 4);
    }
    optionsSeparator_->setVisible(widths || sizes);
    optionsSeparator_->move(108, 7);
    optionsBar_->setFixedSize(widths || sizes ? 217 : 100, 36);
    optionsBar_->setVisible(colors);
}
void SelectionOverlay::positionStatus()
{
    if (status_->isHidden())
        return;
    const QRect bounds = availableRect();
    const int maximumWidth = std::min(toolbar_->width(), bounds.width());
    const int naturalWidth = status_->fontMetrics().horizontalAdvance(status_->text()) + 20;
    const int pillWidth = std::min(maximumWidth, naturalWidth);
    status_->setFixedWidth(pillWidth);
    // 由 QLabel 按实际样式计算换行高度，避免字体度量与控件排版差异裁掉末行。
    const int pillHeight = std::max(26, status_->heightForWidth(pillWidth) + 8);
    status_->setFixedHeight(pillHeight);
    const int x = std::clamp(toolbar_->geometry().right() + 1 - pillWidth, bounds.left(),
                             std::max(bounds.left(), bounds.right() + 1 - pillWidth));
    const int clusterBottom =
        optionsBar_->isHidden() ? toolbar_->geometry().bottom() : optionsBar_->geometry().bottom();
    int y = clusterBottom + 11;
    if (y + pillHeight > bounds.bottom() + 1)
        y = toolbar_->y() - pillHeight - 10;
    y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() + 1 - pillHeight));
    status_->move(x, y);
}
void SelectionOverlay::updateToolbar()
{
    if (finished_ || selection_.isEmpty() || dragMode_ == DragMode::Create)
    {
        if (toolbar_)
        {
            toolbar_->hide();
            optionsBar_->hide();
            status_->hide();
        }
        if (undoShortcut_)
        {
            undoShortcut_->setEnabled(false);
            redoShortcut_->setEnabled(false);
        }
        return;
    }
    ensureToolbar();
    const bool editingText = textEditor_ && textEditor_->isVisible();
    undoButton_->setEnabled(!textMode_ && history_.canUndo());
    redoButton_->setEnabled(!textMode_ && history_.canRedo());
    undoShortcut_->setEnabled(!textMode_ && !editingText && !saveDialogOpen_ &&
                              dragMode_ == DragMode::None);
    redoShortcut_->setEnabled(!textMode_ && !editingText && !saveDialogOpen_ &&
                              dragMode_ == DragMode::None);
    copyShortcut_->setEnabled(!editingText && !saveDialogOpen_ && !ocrDialog_);
    ocrButton_->setChecked(textMode_);
    extractAllTextButton_->setVisible(textMode_);
    const QRect bounds = availableRect();
    layoutToolbar(bounds.width());
    updateOptionsBar();
    const int clusterHeight = toolbar_->height() + (optionsBar_->isHidden() ? 0 : 42);
    const QRectF region = logicalSelection();
    const int x = std::clamp(qRound(region.right()) - toolbar_->width(), bounds.left(),
                             std::max(bounds.left(), bounds.right() + 1 - toolbar_->width()));
    int y = qRound(region.bottom()) + 10;
    if (y + clusterHeight > bounds.bottom() + 1)
    {
        y = qRound(region.top()) - clusterHeight - 10;
        if (y < bounds.top())
            y = bounds.bottom() + 1 - clusterHeight - 10;
    }
    y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() + 1 - clusterHeight));
    toolbar_->move(x, y);
    optionsBar_->move(x, y + toolbar_->height() + 6);
    const bool enabled = dragMode_ == DragMode::None && !saveDialogOpen_;
    toolbar_->setEnabled(enabled);
    optionsBar_->setEnabled(enabled);
    toolbar_->show();
    positionStatus();
}
void SelectionOverlay::showStatus(const QString& text, bool temporary)
{
    ensureToolbar();
    statusTimeout_.stop();
    status_->setText(text);
    status_->setToolTip(text);
    status_->show();
    updateToolbar();
    if (temporary)
        statusTimeout_.start(3000);
}
void SelectionOverlay::copySelection()
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
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
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
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
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return;
    finishText(true);
    hideMagnifier();
    saveDialogOpen_ = true;
    toolbar_->setEnabled(false);
    optionsBar_->setEnabled(false);
    QPointer<SelectionOverlay> self(this);
    const ImageSaveActions actions = actions_;
    const auto target = chooseImageSaveTarget(
        this, actions, [self] { return self && !self->finished_; },
        [self](const QString& text)
        {
            if (self)
                self->showStatus(text, false);
        });
    if (!self)
        return;
    saveDialogOpen_ = false;
    if (finished_)
        return;
    updateToolbar();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    if (!target.path.isEmpty())
        finishSave(saveImageToTarget(
            renderAnnotatedSelection(frame_.pixels, annotations(), selection_), target));
}
bool SelectionOverlay::exportToPath(const QString& path)
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None || path.isEmpty())
        return false;
    finishText(true);
    return finishSave(exportImageToPath(
        renderAnnotatedSelection(frame_.pixels, annotations(), selection_), path));
}
bool SelectionOverlay::finishSave(const ImageFileResult& output)
{
    if (!output.result.success)
    {
        showStatus(QStringLiteral("保存失败：%1 点击「保存」选择路径并重试。")
                       .arg(output.result.explanation),
                   false);
        return false;
    }
    saved_ = true;
    showStatus(QStringLiteral("已保存：%1").arg(output.path), true);
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
    if (textMode_)
    {
        setCursor(textAt(position, false).valid() ? Qt::IBeamCursor : Qt::ArrowCursor);
        return;
    }
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
    if (dragMode_ != DragMode::Annotate)
        showMouseMagnifier(position);
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
        paintAnnotations(painter, annotations(), frame_.pixels, &renderCache_);
        if (draft_)
            paintAnnotation(painter, *draft_, frame_.pixels);
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
            if (textMode_)
            {
                QPainterPath highlight;
                highlight.setFillRule(Qt::WindingFill);
                for (const auto& box : ocrHighlightBoxes())
                    highlight.addRect(QRectF((box.x() + selection_.x()) / scale,
                                             (box.y() + selection_.y()) / scale,
                                             box.width() / scale, box.height() / scale));
                QColor color = palette().color(QPalette::Highlight);
                color.setAlpha(77);
                painter.save();
                painter.setClipRect(region);
                painter.setBrush(color);
                painter.setPen(QPen(palette().color(QPalette::Highlight), 1 / scale));
                painter.drawPath(highlight.simplified());
                painter.restore();
            }
            if (!textMode_ && dragMode_ != DragMode::Create)
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
        const QString hint = textMode_ ? QStringLiteral("拖选文字 · 双击选词 · Esc 退出取字")
                             : activeTool_
                                 ? QStringLiteral("拖拽标注 · 再点工具调整选区 · Esc 取消")
                                 : QStringLiteral("单击吸附 · 拖拽框选 · Esc 取消");
        const QString text = selectionSizeText() + QStringLiteral("   ") + hint;
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
        paintMagnifier(painter);
    }
    if (!painted_)
    {
        painted_ = true;
        emit firstPaintCompleted(monotonicNs());
    }
}
void SelectionOverlay::mousePressEvent(QMouseEvent* event)
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || event->button() != Qt::LeftButton)
        return;
    hideMagnifier();
    if (textMode_)
    {
        setFocus(Qt::MouseFocusReason);
        ocrAnchor_ = textAt(event->position(), false);
        selectingText_ = ocrAnchor_.valid();
        ocrSelection_ = text_layout::selectionRange(ocrResult_.lines, ocrAnchor_, ocrAnchor_);
        update();
        return;
    }
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
    showMouseMagnifier(press_);
    updateToolbar();
    update();
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (ocrDialog_ || finished_ || saveDialogOpen_)
        return;
    if (textMode_)
    {
        if (selectingText_)
            selectOcrTo(event->position());
        updateCursor(event->position());
        return;
    }
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
    if (ocrDialog_)
        return;
    if (textMode_ && !finished_ && !saveDialogOpen_ && event->button() == Qt::LeftButton)
    {
        if (selectingText_)
            selectOcrTo(event->position());
        selectingText_ = false;
        return;
    }
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
    hideMagnifier();
    updateHover(event->position());
    updateCursor(event->position());
    updateToolbar();
    update();
}
void SelectionOverlay::keyPressEvent(QKeyEvent* event)
{
    if (ocrDialog_)
    {
        if (event->key() == Qt::Key_Escape)
            ocrDialog_->reject();
        event->accept();
    }
    else if (finished_ || saveDialogOpen_)
        event->accept();
    else if (textEditor_ && textEditor_->isVisible())
    {
        // 焦点层即使有事件泄漏到覆盖层，也不能取消会话或撤销已提交标注。
        if (event->key() == Qt::Key_Escape && !textEditor_->isComposing())
            finishText(false);
        event->accept();
    }
    else if (event->key() == Qt::Key_Escape)
    {
        if (ocrDialog_)
            ocrDialog_->reject();
        else if (textMode_)
            exitTextMode();
        else
            complete(cancelledSessionOutcome);
    }
    else if (event->matches(QKeySequence::Copy))
        copyShortcut();
    else if (textMode_)
        event->accept();
    else if (event->matches(QKeySequence::Undo))
        undoAnnotation();
    else if (event->matches(QKeySequence::Redo))
        redoAnnotation();
    else if (nudgeSelection(event))
        event->accept();
    else
        QWidget::keyPressEvent(event);
}
QString SelectionOverlay::selectionSizeText() const
{
    return QStringLiteral("%1 × %2 像素").arg(selection_.width()).arg(selection_.height());
}
bool SelectionOverlay::nudgeSelection(QKeyEvent* event)
{
    if (textMode_ || !hasFocus() || activeTool_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return false;
    const auto modifiers = event->modifiers();
    if (modifiers & ~(Qt::ShiftModifier | Qt::AltModifier) ||
        modifiers == (Qt::ShiftModifier | Qt::AltModifier))
        return false;
    SelectionEdge edge;
    switch (event->key())
    {
    case Qt::Key_Left:
        edge = SelectionEdge::Left;
        break;
    case Qt::Key_Up:
        edge = SelectionEdge::Top;
        break;
    case Qt::Key_Right:
        edge = SelectionEdge::Right;
        break;
    case Qt::Key_Down:
        edge = SelectionEdge::Bottom;
        break;
    default:
        return false;
    }
    const auto mode = modifiers == Qt::ShiftModifier ? SelectionNudge::Expand
                      : modifiers == Qt::AltModifier ? SelectionNudge::Shrink
                                                     : SelectionNudge::Move;
    setSelection(nudgedPixelSelection(selection_, edge, mode, frame_.pixels.size()));
    updateToolbar();
    showMagnifier(selectionNudgeAnchor(selection_, edge, mode), true);
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
    return true;
}
void SelectionOverlay::showMagnifier(QPoint anchor, bool keyboard)
{
    if (textMode_ || finished_ || saveDialogOpen_ || activeTool_)
        return;
    const QRect sampleRect = magnifierSamplingRect(anchor, frame_.pixels.size());
    if (sampleRect.isEmpty())
        return;
    if (!magnifierVisible_ || anchor != magnifierAnchor_)
    {
        magnifierAnchor_ = anchor;
        magnifierSampleRect_ = sampleRect;
        magnifierSample_ = frame_.pixels.copy(sampleRect);
        magnifierSample_.setDevicePixelRatio(1);
    }
    magnifierVisible_ = true;
    if (keyboard)
        magnifierTimeout_.start(700);
    else
        magnifierTimeout_.stop();
    update();
}
void SelectionOverlay::showMouseMagnifier(QPointF position)
{
    const QPointF source = physicalPoint(position);
    // 沿用物理坐标路径，最近像素取整后夹在实际像素索引内；不累计逻辑步长。
    showMagnifier({std::clamp(qRound(source.x()), 0, frame_.pixels.width() - 1),
                   std::clamp(qRound(source.y()), 0, frame_.pixels.height() - 1)},
                  false);
}
void SelectionOverlay::hideMagnifier()
{
    magnifierTimeout_.stop();
    magnifierVisible_ = false;
    magnifierAnchor_ = {};
    magnifierSampleRect_ = {};
    magnifierSample_ = {};
    update();
}
QString SelectionOverlay::magnifierPositionText() const
{
    return magnifierVisible_
               ? QStringLiteral("%1, %2").arg(magnifierAnchor_.x()).arg(magnifierAnchor_.y())
               : QString();
}
QRect SelectionOverlay::magnifierRect() const
{
    if (!magnifierVisible_)
        return {};
    const QSize panel(magnifierSample_.width() * magnifierCellSize + 8,
                      magnifierSample_.height() * magnifierCellSize + 32);
    const QPointF anchor = QPointF(magnifierAnchor_) / frame_.display.devicePixelRatio;
    QRect obstacle;
    for (auto* widget : {toolbar_, optionsBar_, static_cast<QWidget*>(status_)})
        if (widget && widget->isVisible())
            obstacle = obstacle.united(widget->geometry());
    return placedMagnifier(anchor, panel, availableRect(), obstacle);
}
void SelectionOverlay::paintMagnifier(QPainter& painter)
{
    if (!magnifierVisible_)
        return;
    const QRect panel = magnifierRect();
    const qreal dpr = devicePixelRatioF();
    painter.save();
    // 在目标设备像素坐标中整数对齐；每源像素在 1 / 1.5 / 2 倍为 8 / 12 / 16 像素。
    painter.scale(1 / dpr, 1 / dpr);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const QPoint origin(qRound(panel.x() * dpr), qRound(panel.y() * dpr));
    const int cell = qRound(magnifierCellSize * dpr);
    const int padding = qRound(4 * dpr);
    const QRect grid(origin + QPoint(padding, padding), magnifierSample_.size() * cell);
    const QRect background(origin, QSize(grid.width() + padding * 2, qRound(panel.height() * dpr)));
    painter.fillRect(background, QColor(20, 20, 20));
    painter.drawImage(grid, magnifierSample_, magnifierSample_.rect());
    const QPoint offset = magnifierAnchor_ - magnifierSampleRect_.topLeft();
    const QRectF marker(grid.topLeft() + offset * cell, QSize(cell, cell));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::black, 3 * dpr));
    painter.drawRect(marker.adjusted(1.5 * dpr, 1.5 * dpr, -1.5 * dpr, -1.5 * dpr));
    painter.setPen(QPen(Qt::white, dpr));
    painter.drawRect(marker.adjusted(1.5 * dpr, 1.5 * dpr, -1.5 * dpr, -1.5 * dpr));
    painter.setPen(Qt::white);
    QFont textFont = painter.font();
    textFont.setPixelSize(qRound(12 * dpr));
    painter.setFont(textFont);
    painter.drawText(QRect(grid.x(), grid.y() + grid.height(), grid.width(), qRound(24 * dpr)),
                     Qt::AlignCenter, magnifierPositionText());
    painter.restore();
}
void SelectionOverlay::closeEvent(QCloseEvent* event)
{
    event->accept();
    if (!finished_)
    {
        hoveredWindowPixels_ = {};
        finished_ = true;
        exitTextMode();
        hideMagnifier();
        discardAnnotations();
        emit finished({}, cancelledSessionOutcome);
    }
}
void SelectionOverlay::complete(int outcome)
{
    if (finished_)
        return;
    finished_ = true;
    exitTextMode();
    hideMagnifier();
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
    if (ocrDialog_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    exitTextMode();
    finishText(true);
    activeTool_ = activeTool_ == type ? std::nullopt : std::optional<AnnotationType>(type);
    hideMagnifier();
    for (int index = 0; index < toolButtons_.size(); ++index)
        toolButtons_[index]->setChecked(activeTool_ == static_cast<AnnotationType>(index));
    if (activeTool_ == AnnotationType::Mosaic && !mosaicNoticeShown_)
    {
        mosaicNoticeShown_ = true;
        showStatus(QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖"), true);
    }
    setFocus(Qt::OtherFocusReason);
    updateToolbar();
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
    if (textMode_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    finishText(true);
    if (history_.undo())
        markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::redoAnnotation()
{
    if (textMode_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
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
    if ((watched == toolbar_ || watched == optionsBar_ || qobject_cast<QLabel*>(watched)) &&
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
void SelectionOverlay::toggleTextMode()
{
    if (finished_ || saveDialogOpen_ || ocrDialog_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return;
    if (textMode_)
        exitTextMode();
    else
    {
        finishText(true);
        activeTool_.reset();
        for (auto* button : toolButtons_)
            button->setChecked(false);
        textMode_ = true;
        hideMagnifier();
        startOcrRecognition();
    }
    setFocus(Qt::OtherFocusReason);
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::exitTextMode()
{
    if (!textMode_)
        return;
    ++recognitionToken_;
    recognitionProgress_.stop();
    recognizingText_ = false;
    selectingText_ = false;
    textMode_ = false;
    ocrResult_ = {};
    ocrAnchor_ = {};
    ocrSelection_.clear();
    if (ocrDialog_)
        ocrDialog_->reject();
    statusTimeout_.stop();
    if (status_)
        status_->hide();
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::startOcrRecognition()
{
    if (!textMode_ || finished_)
        return;
    if (!actions_.recognizer)
        actions_.recognizer = createTextRecognizer();
    const quint64 token = ++recognitionToken_;
    recognizingText_ = true;
    ocrResult_ = {};
    ocrSelection_.clear();
    selectingText_ = false;
    statusTimeout_.stop();
    status_->hide();
    if (ocrDialog_)
        ocrDialog_->setRecognizing();
    auto* task = new TextRecognitionTask(
        actions_.recognizer, renderAnnotatedSelection(frame_.pixels, annotations(), selection_),
        token, this);
    connect(
        task, &TextRecognitionTask::completed, this,
        [this](quint64 completedToken, TextRecognitionResult result)
        {
            if (!textMode_ || finished_ || completedToken != recognitionToken_)
                return;
            recognitionProgress_.stop();
            recognizingText_ = false;
            ocrResult_ = std::move(result);
            if (ocrDialog_)
                ocrDialog_->setResult(ocrResult_);
            if (!ocrResult_.ok)
                showStatus(QStringLiteral("识别失败：%1").arg(ocrResult_.explanation), false);
            else if (ocrResult_.lines.isEmpty())
                showStatus(QStringLiteral("未识别到文字"), false);
            else
            {
                status_->hide();
                updateToolbar();
            }
            updateCursor(mapFromGlobal(QCursor::pos()));
            update();
        },
        Qt::QueuedConnection);
    connect(task, &QThread::finished, task, &QObject::deleteLater);
    recognitionProgress_.start(300);
    task->start();
    update();
}
void SelectionOverlay::openOcrDialog()
{
    if (!textMode_ || finished_ || saveDialogOpen_ || ocrDialog_)
        return;
    auto* dialog = new OcrDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    ocrDialog_ = dialog;
    if (!recognizingText_)
        dialog->setResult(ocrResult_);
    connect(dialog, &OcrDialog::retryRequested, this, &SelectionOverlay::startOcrRecognition);
    connect(dialog, &QDialog::finished, this,
            [this]
            {
                ocrDialog_ = nullptr;
                if (!finished_)
                {
                    updateToolbar();
                    activateWindow();
                    setFocus(Qt::OtherFocusReason);
                }
            });
    updateToolbar();
    dialog->open();
}
text_layout::TokenPosition SelectionOverlay::textAt(QPointF position, bool nearest) const
{
    if (!textMode_ || recognizingText_ || !ocrResult_.ok)
        return {};
    const QPointF point = physicalPoint(position) - QPointF(selection_.topLeft());
    int lineIndex = -1;
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i < ocrResult_.lines.size(); ++i)
    {
        const auto& line = ocrResult_.lines[i];
        if (!nearest)
        {
            const auto tokens = text_layout::selectableTokens(line);
            for (int j = 0; j < tokens.size(); ++j)
                if (tokens[j].box.contains(point))
                    return {i, j};
        }
        else
        {
            const qreal distance = std::abs(line.box.center().y() - point.y());
            if (distance < best && !line.text.isEmpty())
            {
                lineIndex = i;
                best = distance;
            }
        }
    }
    return lineIndex < 0
               ? text_layout::TokenPosition{}
               : text_layout::TokenPosition{
                     lineIndex, text_layout::nearestToken(ocrResult_.lines[lineIndex], point.x())};
}
void SelectionOverlay::selectOcrTo(QPointF position)
{
    ocrSelection_ =
        text_layout::selectionRange(ocrResult_.lines, ocrAnchor_, textAt(position, true));
    update();
}
void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!textMode_)
    {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (ocrDialog_ || finished_ || saveDialogOpen_ || event->button() != Qt::LeftButton)
        return;
    ocrAnchor_ = textAt(event->position(), false);
    selectingText_ = false;
    ocrSelection_ = text_layout::selectionRange(ocrResult_.lines, ocrAnchor_, ocrAnchor_);
    update();
}
QString SelectionOverlay::selectedOcrText() const
{
    return text_layout::selectionText(ocrResult_.lines, ocrSelection_);
}
QVector<QRectF> SelectionOverlay::ocrHighlightBoxes() const
{
    QVector<QRectF> boxes;
    for (const auto& range : ocrSelection_)
    {
        const auto tokens = text_layout::selectableTokens(ocrResult_.lines[range.line]);
        for (int i = range.firstToken; i <= range.lastToken; ++i)
            boxes.append(tokens[i].box);
    }
    return boxes;
}
void SelectionOverlay::copyShortcut()
{
    if (finished_ || saveDialogOpen_ || ocrDialog_)
        return;
    if (!textMode_)
    {
        copySelection();
        return;
    }
    const QString text = selectedOcrText();
    if (!text.isEmpty())
    {
        actions_.setClipboardText(text);
        showStatus(QStringLiteral("已复制 %1 个字符").arg(text.size()), true);
        statusTimeout_.start(2000);
    }
}

}
