#include "ui/sticker_window.h"
#include "core/shape_mask.h"
#include "core/sticker_geometry.h"
#include "output/annotation_renderer.h"
#include "ui/annotation_color_palette.h"
#include "ui/annotation_text_edit.h"
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDebug>
#include <QFocusEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
StickerWindow::StickerWindow(QImage image, QPoint position, bool saved, StickerActions actions,
                             qreal initialScale)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      image_(std::move(image)), actions_(std::move(actions)), geometryPosition_(position),
      saved_(saved)
{
    setObjectName(QStringLiteral("stickerWindow"));
    setWindowTitle(QStringLiteral("WaibuSnap 贴图"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setAttribute(Qt::WA_DeleteOnClose);
    transparent_ = hasTransparentPixels(image_);
    if (transparent_)
        setAttribute(Qt::WA_TranslucentBackground);
    else
        setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setCursor(Qt::SizeAllCursor);
    // 正常钉图输入已为 DPR=1，保持隐式共享，避免再次分配整图像素。
    if (image_.devicePixelRatio() != 1)
        image_.setDevicePixelRatio(1);
    if (!actions_.copyImage)
        actions_.copyImage = copyImageToClipboard;
    if (!actions_.confirmClose)
        actions_.confirmClose = [](QWidget* parent)
        {
            QMessageBox dialog(QMessageBox::Question, QStringLiteral("关闭未保存贴图"),
                               QStringLiteral("贴图尚未保存，请选择保存或放弃。"),
                               QMessageBox::NoButton, parent);
            auto* cancel = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
            auto* save = dialog.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
            dialog.addButton(QStringLiteral("放弃"), QMessageBox::DestructiveRole);
            dialog.setDefaultButton(cancel);
            dialog.setEscapeButton(cancel);
            auto* doNotRemind = new QCheckBox(QStringLiteral("不再提醒"), &dialog);
            doNotRemind->setObjectName(QStringLiteral("stickerDoNotRemindCheck"));
            dialog.setCheckBox(doNotRemind);
            dialog.exec();
            const auto decision = dialog.clickedButton() == save ? StickerCloseDecision::Save
                                  : dialog.clickedButton() == cancel || !dialog.clickedButton()
                                      ? StickerCloseDecision::Cancel
                                      : StickerCloseDecision::Discard;
            return StickerCloseConfirmation{decision, doNotRemind->isChecked()};
        };
    if (!actions_.confirmDoubleClickClose)
        actions_.confirmDoubleClickClose = [](QWidget* parent)
        {
            QMessageBox dialog(QMessageBox::Question, QStringLiteral("双击关闭贴图"),
                               QStringLiteral("开启后，双击非编辑状态的贴图即可关闭。\n"
                                              "未保存贴图仍会按关闭提醒处理。\n"
                                              "可在「设置 → 贴图」中随时修改。"),
                               QMessageBox::NoButton, parent);
            auto* enable = dialog.addButton(QStringLiteral("开启并关闭"), QMessageBox::AcceptRole);
            auto* cancel = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
            dialog.setDefaultButton(cancel);
            dialog.setEscapeButton(cancel);
            dialog.exec();
            return dialog.clickedButton() == enable;
        };
    scale_ =
        std::isfinite(initialScale) && initialScale > 0 && initialScale <= 1 ? initialScale : 1;
    winId();
    if (QScreen* target = QGuiApplication::screenAt(position))
        windowHandle()->setScreen(target);
    resize(stickerWindowSize(image_.size(), scale_, screenDpr()));
    move(position);
    controls_ = new QWidget(this);
    controls_->setObjectName(QStringLiteral("stickerControls"));
    controls_->setCursor(Qt::ArrowCursor);
    controls_->setStyleSheet(QStringLiteral(
        "QWidget#stickerControls { background: #202020; border-radius: 4px; }"
        "QPushButton { color: white; background: #404040; padding: 2px; border-radius: 3px; }"
        "QPushButton:hover { background: #705030; }"
        "QPushButton:disabled { color: #888888; }"));
    auto* layout = new QGridLayout(controls_);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    const QString labels[] = {QStringLiteral("－"),   QStringLiteral("＋"),
                              QStringLiteral("100%"), QStringLiteral("保存"),
                              QStringLiteral("复制"), QStringLiteral("编辑"),
                              QStringLiteral("×")};
    const QString names[] = {
        QStringLiteral("stickerZoomOutButton"), QStringLiteral("stickerZoomInButton"),
        QStringLiteral("stickerResetButton"),   QStringLiteral("stickerSaveButton"),
        QStringLiteral("stickerCopyButton"),    QStringLiteral("stickerEditButton"),
        QStringLiteral("stickerCloseButton")};
    const QString tips[] = {QStringLiteral("缩小"),      QStringLiteral("放大"),
                            QStringLiteral("恢复 100%"), QStringLiteral("保存原图"),
                            QStringLiteral("复制原图"),  QStringLiteral("编辑"),
                            QStringLiteral("关闭贴图")};
    auto* menu = new QMenu(this);
    setContextMenuPolicy(Qt::ActionsContextMenu);
    for (int index = 0; index < 7; ++index)
    {
        auto* button = new QPushButton(labels[index], controls_);
        button->setObjectName(names[index]);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tips[index]);
        button->setAccessibleName(tips[index]);
        if (index == 2)
            scaleButton_ = button;
        auto* action = new QAction(tips[index], this);
        addAction(action);
        menu->addAction(action);
        connect(action, &QAction::triggered, button, &QPushButton::click);
        // 窄图自动分行；控制条覆盖显示层，不改变图片窗口尺寸和输出。
        layout->addWidget(button, index / 7, index % 7);
        connect(button, &QPushButton::clicked, this,
                [this, index]
                {
                    if (index < 3)
                        setCenteredScale(
                            index == 2 ? 1 : steppedStickerScale(scale_, index == 0 ? -1 : 1));
                    else if (index == 3)
                        saveImage();
                    else if (index == 4)
                        copyImage();
                    else if (index == 5)
                        setEditing(true);
                    else
                        close();
                });
    }
    // 极小贴图容不下七个控件时，用可发现的操作菜单保留全部入口。
    moreButton_ = new QPushButton(QStringLiteral("⋯"), this);
    moreButton_->setObjectName(QStringLiteral("stickerMoreButton"));
    moreButton_->setFocusPolicy(Qt::NoFocus);
    moreButton_->setToolTip(
        QStringLiteral("贴图操作：缩放、100%、保存、复制、编辑、关闭；也可右键打开"));
    moreButton_->setAccessibleName(QStringLiteral("贴图操作"));
    moreButton_->hide();
    connect(moreButton_, &QPushButton::clicked, this,
            [this, menu] { menu->popup(mapToGlobal(QPoint(0, moreButton_->height()))); });
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("stickerStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setAttribute(Qt::WA_TransparentForMouseEvents);
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: white; background: #202020; padding: 3px;"));
    status_->hide();
    statusTimeout_.setSingleShot(true);
    connect(&statusTimeout_, &QTimer::timeout, status_, &QWidget::hide);
    updateScaleLabel();
    positionControls();
    controls_->hide();
    connect(windowHandle(), &QWindow::screenChanged, this, &StickerWindow::refreshScreenGeometry);
}
qreal StickerWindow::screenDpr() const
{
    return windowHandle() && windowHandle()->screen() ? windowHandle()->screen()->devicePixelRatio()
                                                      : 1;
}
void StickerWindow::setEditing(bool editing)
{
    if (closed_ || hasPendingInteraction() || quitInteraction_ || editing_ == editing)
        return;
    if (!editing)
        finishText(true, false);
    clearAnnotationSelection();
    selectMode_ = false;
    widthWheel_.reset();
    wheelRemainder_ = 0;
    draft_.reset();
    lastAnnotationSelected_ = false;
    dragging_ = false;
    const QRect geometry = this->geometry();
    const QPointF precisePosition = geometryPosition_;
    const bool visible = isVisible();
    QPointer<QScreen> screen = windowHandle()->screen();
    // 切换焦点模式时重建原生窗口，避免运行中修改 NSPanel 非激活样式留下 AppKit 焦点状态。
    hide();
    destroy();
    editing_ = editing;
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(editing ? Qt::StrongFocus : Qt::ClickFocus);
    winId();
    if (screen)
        windowHandle()->setScreen(screen);
    const bool configured = configureStickerWindowBehavior(windowHandle(), editing);
    connect(windowHandle(), &QWindow::screenChanged, this, &StickerWindow::refreshScreenGeometry,
            Qt::UniqueConnection);
    applyGeometry(precisePosition, geometry.size());
    if (visible)
    {
        show();
        // Cocoa 在显示时可能按菜单栏边界调整窗口，再恢复精确位置。
        applyGeometry(precisePosition, geometry.size());
    }
    setAttribute(Qt::WA_ShowWithoutActivating, !editing);
    setContextMenuPolicy(editing ? Qt::DefaultContextMenu : Qt::ActionsContextMenu);
    setCursor(editing ? Qt::ArrowCursor : Qt::SizeAllCursor);
    if (editing)
    {
        ensureEditToolbar();
        focusSession_ = activateStickerEditing(windowHandle());
        activateWindow();
        setFocus(Qt::OtherFocusReason);
    }
    else
        focusSession_.reset();
    if (!configured)
        showStatus(QStringLiteral("无法配置贴图焦点，请关闭后重新钉图。"), false);
    positionControls();
    updateEditToolbar();
    update();
}
void StickerWindow::applyGeometry(QPointF position, QSize size)
{
    // 保存亚像素位置，连续按钮缩放不能累计顶层窗口整数取整误差。
    geometryPosition_ = position;
    setGeometry(QRect(position.toPoint(), size));
}
void StickerWindow::updateScaleLabel()
{
    if (!scaleButton_)
        return;
    const int percent = qRound(scale_ * 100);
    scaleButton_->setText(QStringLiteral("%1%").arg(percent));
    const QString tip = percent == 100 ? QStringLiteral("当前缩放 100%")
                                       : QStringLiteral("当前缩放 %1%，点击恢复 100%").arg(percent);
    scaleButton_->setToolTip(tip);
    scaleButton_->setAccessibleName(tip);
}
void StickerWindow::moveEvent(QMoveEvent* event)
{
    if (event->pos() != geometryPosition_.toPoint())
        geometryPosition_ = event->pos();
}
void StickerWindow::setCenteredScale(qreal scale)
{
    if (closed_ || editing_ || hasPendingInteraction() || quitInteraction_)
        return;
    const qreal next = clampedStickerScale(scale);
    const QSize nextSize = stickerWindowSize(image_.size(), next, screenDpr());
    if (nextSize.isEmpty())
        return;
    // 中心取整一次，此后保留亚像素左上角；跨过全局坐标零点也不积累误差。
    const QPointF center = (geometryPosition_ + QPointF(width() / 2.0, height() / 2.0)).toPoint();
    scale_ = next;
    applyGeometry(center - QPointF(nextSize.width() / 2.0, nextSize.height() / 2.0), nextSize);
    updateScaleLabel();
    positionControls();
    update();
}
void StickerWindow::setScale(qreal scale, QPointF globalAnchor)
{
    if (closed_ || editing_ || hasPendingInteraction() || quitInteraction_)
        return;
    const qreal next = clampedStickerScale(scale);
    const QSize nextSize = stickerWindowSize(image_.size(), next, screenDpr());
    if (nextSize.isEmpty())
        return;
    const QPointF position =
        anchoredStickerPosition(geometryPosition_, size(), nextSize, globalAnchor);
    scale_ = next;
    applyGeometry(position, nextSize);
    positionTextEditor();
    updateScaleLabel();
    positionControls();
    update();
}
void StickerWindow::refreshScreenGeometry()
{
    if (closed_ || dragging_)
        return;
    const QSize nextSize = stickerWindowSize(image_.size(), scale_, screenDpr());
    if (nextSize.isEmpty())
        return;
    const QPointF center = geometryPosition_ + QPointF(width() / 2.0, height() / 2.0);
    applyGeometry(anchoredStickerPosition(geometryPosition_, size(), nextSize, center), nextSize);
    update();
}
bool StickerWindow::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange && controls_)
        refreshScreenGeometry();
    return handled;
}
void StickerWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    if (!transparent_)
        painter.fillRect(rect(), Qt::black);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, scale_ != 1);
    const qreal factor = scale_ / screenDpr();
    painter.save();
    painter.scale(factor, factor);
    painter.drawImage(QPointF(0, 0), image_);
    paintAnnotations(painter, annotations(), image_, &renderCache_);
    if (draft_)
        paintAnnotation(painter, *draft_, image_);
    painter.restore();
    paintAnnotationSelection(painter);
}
QImage StickerWindow::renderedImage() const
{
    // 无标注直接返回隐式共享的基图；标注只在输出时合成，不缓存缩放副本。
    return annotations().isEmpty() ? image_
                                   : renderAnnotatedSelection(image_, annotations(), image_.rect());
}
void StickerWindow::positionControls()
{
    if (!controls_)
        return;
    auto* layout = static_cast<QGridLayout*>(controls_->layout());
    const auto buttons = controls_->findChildren<QPushButton*>();
    const int columns = std::clamp(width() / 44, 1, 7);
    for (int index = 0; index < buttons.size(); ++index)
        layout->addWidget(buttons[index], index / columns, index % columns);
    controls_->adjustSize();
    controls_->move(std::max(0, (width() - controls_->width()) / 2), 0);
    if (moreButton_)
    {
        const bool compact = controls_->width() > width() || controls_->height() > height();
        controls_->setVisible(hovered_ && !compact && !editing_);
        moreButton_->setGeometry(0, 0, std::min(44, width()), std::min(22, height()));
        moreButton_->setVisible(hovered_ && compact && !editing_);
    }
    if (status_)
    {
        status_->setFixedWidth(width());
        status_->adjustSize();
        const int bottom =
            editing_ && editToolbar_ && editToolbar_->isVisible() ? editToolbar_->y() : height();
        const int top = toolBar_ && toolBar_->isVisible() ? toolBar_->y() : bottom;
        status_->move(0, std::max(0, top - status_->height()));
    }
}
void StickerWindow::resizeEvent(QResizeEvent*)
{
    positionControls();
    updateEditToolbar();
    positionTextEditor();
}
void StickerWindow::enterEvent(QEnterEvent*)
{
    hovered_ = true;
    positionControls();
}
void StickerWindow::leaveEvent(QEvent*)
{
    hovered_ = false;
    if (!saveDialogOpen_)
    {
        controls_->hide();
        moreButton_->hide();
    }
}
void StickerWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || closed_ || saveDialogOpen_ || confirmingClose_ ||
        confirmingDoubleClickClose_ || quitInteraction_)
        return;
    if (editing_)
    {
        const bool textWasEditing = textEditor_ && textEditor_->isVisible();
        const qsizetype textCountBefore = annotations().size();
        finishText(true);
        const bool textCommitted = textWasEditing && annotations().size() > textCountBefore;
        lastAnnotationSelected_ = false;
        if (textCommitted)
        {
            // 点画布外提交文本时保留当前文本框选中，显示选框供直接拖动 / 旋转。
            updateEditToolbar();
            update();
            event->accept();
            return;
        }
        if (selectMode_)
        {
            beginAnnotationGesture(event->position(), event->modifiers());
            event->accept();
            return;
        }
        if (activeTool_ && selectedAnnotation_ >= 0)
        {
            if (rotationHandleHit(event->position()))
            {
                startAnnotationGesture(AnnotationGesture::Rotate, selectedAnnotation_,
                                       event->position());
                event->accept();
                return;
            }
            if (annotationHitTest(annotations()[selectedAnnotation_],
                                  physicalPoint(event->position()), 4))
            {
                pendingSelectedGesture_ = true;
                pendingSelectedPress_ = event->position();
                event->accept();
                return;
            }
            clearAnnotationSelection();
        }
        if (!activeTool_)
            return;
        if (activeTool_ == AnnotationType::Text)
            startText(event->position());
        else
            draft_ = Annotation{*activeTool_,
                                style_,
                                physicalPoint(event->position()),
                                physicalPoint(event->position()),
                                {},
                                {}};
        updateEditToolbar();
        update();
        event->accept();
        return;
    }
    dragging_ = true;
    clickCandidate_ = true;
    pressGlobal_ = event->globalPosition();
    dragFraction_ = {event->position().x() / width(), event->position().y() / height()};
    event->accept();
}
void StickerWindow::focusForKeyboard()
{
    if (!windowHandle())
        return;
    if (!makeStickerWindowKeyForKeyboard(windowHandle()))
        windowHandle()->requestActivate();
    setFocus(Qt::MouseFocusReason);
}
void StickerWindow::moveDrag(QPointF globalPosition)
{
    if (QScreen* target = QGuiApplication::screenAt(globalPosition.toPoint()))
        windowHandle()->setScreen(target);
    const QSize nextSize = stickerWindowSize(image_.size(), scale_, screenDpr());
    if (nextSize.isEmpty())
        return;
    const QPointF offset(dragFraction_.x() * nextSize.width(),
                         dragFraction_.y() * nextSize.height());
    applyGeometry(globalPosition - offset, nextSize);
}
void StickerWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (pendingSelectedGesture_)
    {
        if (std::hypot(event->position().x() - pendingSelectedPress_.x(),
                       event->position().y() - pendingSelectedPress_.y()) < 2)
            return;
        pendingSelectedGesture_ = false;
        if (!beginSelectedAnnotationGesture(pendingSelectedPress_))
            return;
    }
    if (annotationGesture_ != AnnotationGesture::None)
    {
        updateAnnotationGesture(event->position(), event->modifiers());
        return;
    }
    if (selectMode_)
    {
        if (rotationHandleHit(event->position()))
            setCursor(Qt::CrossCursor);
        else if (annotationAt(event->position()) >= 0)
            setCursor(Qt::SizeAllCursor);
        else
            setCursor(Qt::ArrowCursor);
    }
    else if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations().size())
    {
        if (rotationHandleHit(event->position()))
            setCursor(Qt::CrossCursor);
        else if (annotationHitTest(annotations()[selectedAnnotation_],
                                   physicalPoint(event->position()), 4))
            setCursor(Qt::SizeAllCursor);
        else if (activeTool_)
            setCursor(*activeTool_ == AnnotationType::Text ? Qt::IBeamCursor : Qt::CrossCursor);
    }
    if (draft_ && !saveDialogOpen_ && !quitInteraction_)
    {
        updateAnnotation(event->position());
        update();
    }
    if (dragging_ && !editing_ && !saveDialogOpen_ && !quitInteraction_)
    {
        if ((event->globalPosition() - pressGlobal_).manhattanLength() > 2)
            clickCandidate_ = false;
        moveDrag(event->globalPosition());
    }
}
void StickerWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && pendingSelectedGesture_)
    {
        pendingSelectedGesture_ = false;
        if (activeTool_ == AnnotationType::Text)
        {
            selectedAnnotation_ = -1;
            startText(pendingSelectedPress_);
        }
        else
        {
            clearAnnotationSelection();
            updateEditToolbar();
            setCursor(Qt::ArrowCursor);
            update();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && annotationGesture_ != AnnotationGesture::None)
    {
        finishAnnotationGesture();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && draft_ && !saveDialogOpen_ && !quitInteraction_)
    {
        updateAnnotation(event->position());
        const bool widthTool = annotationUsesLineWidth(draft_->type);
        if (history_.add(std::move(*draft_)))
        {
            markDirty();
            lastAnnotationSelected_ = widthTool;
            selectedAnnotation_ = int(annotations().size()) - 1;
        }
        else
            lastAnnotationSelected_ = false;
        draft_.reset();
        updateEditToolbar();
        update();
    }
    if (event->button() == Qt::LeftButton && dragging_)
    {
        moveDrag(event->globalPosition());
        dragging_ = false;
        const bool clicked = clickCandidate_;
        clickCandidate_ = false;
        if (clicked)
            focusForKeyboard();
    }
}
void StickerWindow::wheelEvent(QWheelEvent* event)
{
    if (closed_ || saveDialogOpen_ || confirmingClose_ || confirmingDoubleClickClose_ ||
        quitInteraction_)
        return;
    for (QWidget* panel : {controls_, static_cast<QWidget*>(moreButton_), editToolbar_, toolBar_,
                           static_cast<QWidget*>(editMoreButton_)})
        if (panel && panel->isVisible() && panel->geometry().contains(event->position().toPoint()))
        {
            event->accept();
            return;
        }
    if (editing_ && activeTool_ && annotationUsesLineWidth(*activeTool_))
    {
        if (!rect().contains(event->position().toPoint()))
            return;
        wheelRemainder_ = 0;
        if (event->phase() == Qt::ScrollBegin)
            widthWheel_.reset();
        const int width =
            widthWheel_.adjust(style_.lineWidth, event->angleDelta(), event->pixelDelta());
        style_.lineWidth = width;
        if (draft_)
            draft_->style.lineWidth = width;
        else if (lastAnnotationSelected_ && history_.setLastLineWidth(width))
            markDirty();
        if (event->phase() == Qt::ScrollEnd)
            widthWheel_.reset();
        updateEditToolbar();
        if (event->angleDelta().y() || event->pixelDelta().y())
            showStatus(QStringLiteral("线宽 %1px").arg(style_.lineWidth), true);
        update();
        event->accept();
        return;
    }
    if (editing_ && activeTool_ && *activeTool_ == AnnotationType::Text)
    {
        if (!rect().contains(event->position().toPoint()))
            return;
        wheelRemainder_ = 0;
        adjustTextSize(event->angleDelta(), event->pixelDelta(), event->phase());
        if (event->angleDelta().y() || event->pixelDelta().y())
            showStatus(QStringLiteral("字号 %1px").arg(style_.textSize), true);
        updateEditToolbar();
        update();
        event->accept();
        return;
    }
    widthWheel_.reset();
    if (draft_)
        return;
    wheelRemainder_ += event->angleDelta().y() / 120.0;
    const int steps = int(std::trunc(wheelRemainder_));
    wheelRemainder_ -= steps;
    if (steps)
    {
        const qreal next = steppedStickerScale(scale_, steps);
        const QSize nextSize = stickerWindowSize(image_.size(), next, screenDpr());
        const QPointF position =
            anchoredStickerPosition(geometryPosition_, size(), nextSize, event->globalPosition());
        scale_ = next;
        applyGeometry(position, nextSize);
        positionTextEditor();
        updateScaleLabel();
        positionControls();
        update();
    }
    event->accept();
}
void StickerWindow::showStatus(const QString& text, bool temporary)
{
    statusTimeout_.stop();
    status_->setText(text);
    status_->setToolTip(text);
    setToolTip(text);
    status_->show();
    positionControls();
    if (temporary)
        statusTimeout_.start(3000);
}
void StickerWindow::copyImage()
{
    if (closed_ || hasPendingInteraction() || quitInteraction_ || draft_)
        return;
    finishText(true);
    QPointer<StickerWindow> self(this);
    const auto copy = actions_.copyImage;
    const auto result = copy(renderedImage());
    if (!self || closed_)
        return;
    showStatus(result.success
                   ? QStringLiteral("已复制")
                   : QStringLiteral("复制失败：%1 点击「复制」重试。").arg(result.explanation),
               result.success);
}
bool StickerWindow::saveImage()
{
    if (closed_ || saveDialogOpen_ || draft_)
        return false;
    finishText(true);
    saveDialogOpen_ = true;
    dragging_ = false;
    controls_->setEnabled(false);
    moreButton_->setEnabled(false);
    updateEditToolbar();
    QPointer<StickerWindow> self(this);
    const ImageSaveActions actions = actions_;
    const auto target = chooseImageSaveTarget(
        this, actions, [self] { return self && !self->closed_; },
        [self](const QString& text)
        {
            if (self)
                self->showStatus(text, false);
        });
    if (!self)
        return false;
    saveDialogOpen_ = false;
    if (closed_)
        return false;
    controls_->setEnabled(!quitInteraction_);
    moreButton_->setEnabled(!quitInteraction_);
    updateEditToolbar();
    if (target.path.isEmpty())
    {
        showStatus(QStringLiteral("保存已取消，贴图仍保留，可再次保存。"), true);
        return false;
    }
    return finishSave(saveImageToTarget(renderedImage(), target));
}
bool StickerWindow::exportToPath(const QString& path)
{
    if (closed_ || saveDialogOpen_ || draft_ || path.isEmpty())
        return false;
    finishText(true);
    return finishSave(exportImageToPath(renderedImage(), path));
}
bool StickerWindow::finishSave(const ImageFileResult& output)
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
void StickerWindow::forceClose()
{
    forceClosing_ = true;
    close();
}
void StickerWindow::closeEvent(QCloseEvent* event)
{
    if (closed_)
    {
        event->accept();
        return;
    }
    if (!forceClosing_)
    {
        if (hasPendingInteraction() || quitInteraction_)
        {
            event->ignore();
            return;
        }
        finishText(true);
        if (!saved_ && (!actions_.loadCloseReminder || actions_.loadCloseReminder()))
        {
            confirmingClose_ = true;
            QPointer<StickerWindow> self(this);
            const auto confirm = actions_.confirmClose;
            const auto confirmation = confirm(this);
            const auto decision = confirmation.decision;
            if (!self)
                return;
            if (closed_)
                return;
            if (decision == StickerCloseDecision::Cancel ||
                (decision == StickerCloseDecision::Save && !saveImage()))
            {
                if (!self)
                    return;
                confirmingClose_ = false;
                updateEditToolbar();
                event->ignore();
                return;
            }
            if (!self)
                return;
            if (decision == StickerCloseDecision::Discard && confirmation.doNotRemind &&
                actions_.saveCloseReminder)
            {
                const QString error = actions_.saveCloseReminder(false);
                if (!error.isEmpty())
                    qWarning().noquote() << error;
            }
            if (!self)
                return;
            confirmingClose_ = false;
        }
    }
    closed_ = true;
    dragging_ = false;
    statusTimeout_.stop();
    focusSession_.reset();
    event->accept();
    emit closed();
}
void StickerWindow::ensureEditToolbar()
{
    if (editToolbar_)
        return;
    style_.fontFamily = font().family();
    editToolbar_ = new QWidget(this);
    editToolbar_->setObjectName(QStringLiteral("stickerEditToolbar"));
    editToolbar_->setCursor(Qt::ArrowCursor);
    editToolbar_->setStyleSheet(
        QStringLiteral("QWidget#stickerEditToolbar { background: #202020; border-radius: 4px; }"
                       "QPushButton, QComboBox { color: white; background: #404040; padding: 3px; }"
                       "QPushButton:checked { background: #a65316; }"
                       "QPushButton:disabled { color: #888888; }"
                       "QComboBox QAbstractItemView { color: white; background: #303030; }"));
    auto* layout = new QVBoxLayout(editToolbar_);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    auto* tools = new QGridLayout;
    tools->setSpacing(2);
    layout->addLayout(tools);
    editMenu_ = new QMenu(this);
    editMenu_->setObjectName(QStringLiteral("stickerEditMenu"));
    // 二级变体与颜色 / 线宽 / 字号同处参数行；内层只做透明布局容器，
    // 背景仍由 editToolbar_ 提供，避免主栏上方再浮一条独立黑条。
    toolBar_ = new QWidget(editToolbar_);
    toolBar_->setObjectName(QStringLiteral("stickerEditToolBar"));
    toolBar_->setCursor(Qt::ArrowCursor);
    toolBar_->setStyleSheet(
        QStringLiteral("QWidget#stickerEditToolBar { background: transparent; border: none; }"
                       "QPushButton { background: transparent; border: none; border-radius: 6px; }"
                       "QPushButton:hover { background: #3a3a3a; }"
                       "QPushButton:checked { background: #a65316; }"));
    toolBar_->setFixedSize(68, 36);
    toolBar_->hide();
    const auto stickerName = [](QString name)
    {
        name[0] = name[0].toUpper();
        return QStringLiteral("stickerEdit") + name;
    };
    selectButton_ = new QPushButton(QStringLiteral("选择"), editToolbar_);
    selectButton_->setObjectName(QStringLiteral("stickerEditSelectToolButton"));
    selectButton_->setToolTip(QStringLiteral("选择标注：点击选中，拖动移动，拖动上方圆点旋转"));
    selectButton_->setAccessibleName(selectButton_->toolTip());
    selectButton_->setIcon(toolbarIcon(ToolbarIcon::Select));
    selectButton_->setFocusPolicy(Qt::NoFocus);
    selectButton_->setCheckable(true);
    connect(selectButton_, &QPushButton::clicked, this, &StickerWindow::toggleSelectMode);
    for (int slot = 0; slot < int(toolTypes_.size()); ++slot)
    {
        const auto type = toolTypes_[slot];
        auto* button = new QPushButton(annotationToolName(type), editToolbar_);
        button->setObjectName(stickerName(annotationGroupObject(slot)));
        button->setToolTip(annotationToolTip(type));
        button->setAccessibleName(button->toolTip());
        button->setIcon(toolbarIcon(static_cast<ToolbarIcon>(type)));
        button->setFocusPolicy(Qt::NoFocus);
        button->setCheckable(true);
        toolButtons_.append(button);
        tools->addWidget(button, 0, slot);
        connect(button, &QPushButton::clicked, this,
                [this, slot] { activateTool(toolTypes_[slot]); });
        auto* action = editMenu_->addAction(button->icon(), button->text());
        action->setData(button->objectName());
        action->setCheckable(true);
        if (slot == 0 || slot == 1 || slot == 4)
        {
            auto* menu = new QMenu(editMenu_);
            action->setMenu(menu);
        }
        else
            connect(action, &QAction::triggered, button, &QPushButton::click);
    }
    for (auto type : {AnnotationType::Rectangle, AnnotationType::Ellipse, AnnotationType::Line,
                      AnnotationType::Arrow, AnnotationType::Cover, AnnotationType::Mosaic})
    {
        auto* button = new QPushButton(toolBar_);
        button->setObjectName(stickerName(annotationToolObject(type)));
        button->setProperty("annotationType", static_cast<int>(type));
        button->setToolTip(type == AnnotationType::Mosaic
                               ? annotationToolTip(type)
                               : QStringLiteral("使用%1").arg(annotationToolName(type)));
        button->setAccessibleName(button->toolTip());
        button->setIcon(toolbarIcon(static_cast<ToolbarIcon>(type)));
        button->setIconSize(QSize(18, 18));
        button->setFixedSize(28, 28);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCheckable(true);
        subtoolButtons_.append(button);
        connect(button, &QPushButton::clicked, this, [this, type] { activateTool(type, false); });
        auto* menu = editMenu_->actions()[annotationToolSlot(type)]->menu();
        auto* action = menu->addAction(button->icon(), annotationToolName(type));
        action->setData(button->objectName());
        action->setToolTip(button->toolTip());
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, type] { activateTool(type, false); });
    }
    auto* options = new QHBoxLayout;
    layout->addLayout(options);
    options->addWidget(toolBar_);
    colors_ = new AnnotationColorPalette(editToolbar_, QStringLiteral("stickerEditColor"),
                                         [this](int index)
                                         {
                                             finishText(true);
                                             style_.color = annotationColors()[index];
                                             updateEditToolbar();
                                         });
    options->addWidget(colors_);
    auto* colorMenu = editMenu_->addMenu(QStringLiteral("标注颜色"));
    colorMenu->menuAction()->setData(colors_->objectName());
    for (auto* button : colors_->findChildren<QPushButton*>())
    {
        auto* action = colorMenu->addAction(button->icon(), button->toolTip());
        action->setData(button->objectName());
        action->setCheckable(true);
        connect(action, &QAction::triggered, button, &QPushButton::click);
    }
    widthLabel_ = new QLabel(editToolbar_);
    widthLabel_->setObjectName(QStringLiteral("stickerEditWidthLabel"));
    widthLabel_->setStyleSheet(QStringLiteral("color: white;"));
    options->addWidget(widthLabel_);
    const auto makeFormatButton =
        [this](const QString& text, const QString& object, const QString& tip, int style)
    {
        auto* button = new QPushButton(text, editToolbar_);
        button->setObjectName(object);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFixedSize(28, 28);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        QFont font = button->font();
        font.setBold(style == 0);
        font.setItalic(style == 1);
        font.setUnderline(style == 2);
        button->setFont(font);
        return button;
    };
    const auto shortcutText = [](Qt::Key key)
    { return QKeySequence(Qt::CTRL | key).toString(QKeySequence::NativeText); };
    boldButton_ = makeFormatButton(QStringLiteral("B"), QStringLiteral("stickerEditTextBoldButton"),
                                   QStringLiteral("加粗（%1）").arg(shortcutText(Qt::Key_B)), 0);
    italicButton_ =
        makeFormatButton(QStringLiteral("I"), QStringLiteral("stickerEditTextItalicButton"),
                         QStringLiteral("斜体（%1）").arg(shortcutText(Qt::Key_I)), 1);
    underlineButton_ =
        makeFormatButton(QStringLiteral("U"), QStringLiteral("stickerEditTextUnderlineButton"),
                         QStringLiteral("下划线（%1）").arg(shortcutText(Qt::Key_U)), 2);
    options->addWidget(boldButton_);
    options->addWidget(italicButton_);
    options->addWidget(underlineButton_);
    textSizeLabel_ = new QLabel(editToolbar_);
    textSizeLabel_->setObjectName(QStringLiteral("stickerEditTextSizeLabel"));
    textSizeLabel_->setStyleSheet(QStringLiteral("color: white;"));
    textSizeLabel_->setAlignment(Qt::AlignCenter);
    options->addWidget(textSizeLabel_);
    sizes_ = new QComboBox(editToolbar_);
    sizes_->setObjectName(QStringLiteral("stickerEditTextSizeCombo"));
    for (const int size : annotationTextSizes)
        sizes_->addItem(QStringLiteral("字 %1px").arg(size));
    sizes_->setCurrentIndex(1);
    sizes_->setFocusPolicy(Qt::NoFocus);
    sizes_->setToolTip(QStringLiteral("文本字号（物理像素）"));
    sizes_->setAccessibleName(sizes_->toolTip());
    options->addWidget(sizes_);
    auto* sizeMenu = editMenu_->addMenu(sizes_->toolTip());
    sizeMenu->menuAction()->setData(sizes_->objectName());
    for (int index = 0; index < sizes_->count(); ++index)
    {
        auto* action = sizeMenu->addAction(sizes_->itemText(index));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this,
                [this, index] { setTextSize(annotationTextSizes[index]); });
    }
    formatMenu_ = editMenu_->addMenu(QStringLiteral("文字格式"));
    for (auto* button : {boldButton_, italicButton_, underlineButton_})
    {
        auto* action = formatMenu_->addAction(button->text());
        action->setData(button->objectName());
        action->setCheckable(true);
        connect(action, &QAction::triggered, button, &QPushButton::click);
    }
    connect(sizes_, &QComboBox::currentIndexChanged, this,
            [this](int index) { setTextSize(annotationTextSizes[index]); });
    connect(boldButton_, &QPushButton::clicked, this,
            [this] { toggleTextStyle(TextStyleToggle::Bold); });
    connect(italicButton_, &QPushButton::clicked, this,
            [this] { toggleTextStyle(TextStyleToggle::Italic); });
    connect(underlineButton_, &QPushButton::clicked, this,
            [this] { toggleTextStyle(TextStyleToggle::Underline); });
    auto* outputs = new QHBoxLayout;
    layout->addLayout(outputs);
    const auto makeButton = [&](const QString& label, const QString& name)
    {
        auto* button = new QPushButton(label, editToolbar_);
        button->setObjectName(name);
        button->setFocusPolicy(Qt::NoFocus);
        outputs->addWidget(button);
        auto* action = editMenu_->addAction(label);
        action->setData(name);
        connect(action, &QAction::triggered, button, &QPushButton::click);
        return button;
    };
    undoButton_ =
        makeButton(QStringLiteral("撤销"), QStringLiteral("stickerEditUndoAnnotationButton"));
    redoButton_ =
        makeButton(QStringLiteral("重做"), QStringLiteral("stickerEditRedoAnnotationButton"));
    auto* save = makeButton(QStringLiteral("保存"), QStringLiteral("stickerEditSaveButton"));
    auto* copy = makeButton(QStringLiteral("复制"), QStringLiteral("stickerEditCopyButton"));
    auto* done = makeButton(QStringLiteral("完成"), QStringLiteral("stickerEditDoneButton"));
    connect(undoButton_, &QPushButton::clicked, this, &StickerWindow::undoAnnotation);
    connect(redoButton_, &QPushButton::clicked, this, &StickerWindow::redoAnnotation);
    connect(save, &QPushButton::clicked, this, &StickerWindow::saveImage);
    connect(copy, &QPushButton::clicked, this, &StickerWindow::copyImage);
    connect(done, &QPushButton::clicked, this, [this] { setEditing(false); });
    auto* selectAction = editMenu_->addAction(selectButton_->icon(), selectButton_->text());
    selectAction->setData(selectButton_->objectName());
    selectAction->setCheckable(true);
    selectAction->setToolTip(selectButton_->toolTip());
    connect(selectAction, &QAction::triggered, selectButton_, &QPushButton::click);
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    redoShortcut_ = new QShortcut(QKeySequence::Redo, this);
    connect(undoShortcut_, &QShortcut::activated, this, &StickerWindow::undoAnnotation);
    connect(redoShortcut_, &QShortcut::activated, this, &StickerWindow::redoAnnotation);
    editMoreButton_ = new QPushButton(QStringLiteral("⋯"), this);
    editMoreButton_->setObjectName(QStringLiteral("stickerEditMoreButton"));
    editMoreButton_->setFocusPolicy(Qt::NoFocus);
    editMoreButton_->setToolTip(QStringLiteral(
        "编辑工具：图形、文本、遮盖、马赛克、文字格式、样式、撤销、重做、保存、复制、完成"));
    connect(editMoreButton_, &QPushButton::clicked, this,
            [this]
            {
                finishText(true);
                updateEditToolbar();
                editMenu_->popup(mapToGlobal(QPoint(0, editMoreButton_->height())));
            });
    for (auto* panel : {editToolbar_, toolBar_})
    {
        panel->installEventFilter(this);
        for (auto* child : panel->findChildren<QWidget*>())
            child->installEventFilter(this);
    }
}
void StickerWindow::updateEditToolbar()
{
    if (!editToolbar_)
        return;
    const bool available =
        editing_ && !saveDialogOpen_ && !confirmingClose_ && !quitInteraction_ && !draft_;
    editToolbar_->setEnabled(available);
    editMoreButton_->setEnabled(available);
    undoButton_->setEnabled(history_.canUndo());
    redoButton_->setEnabled(history_.canRedo());
    // AnnotationTextEdit 的 ShortcutOverride 保证输入期间优先使用文本自身的历史。
    undoShortcut_->setEnabled(available);
    redoShortcut_->setEnabled(available);
    widthLabel_->setVisible(!selectMode_ && activeTool_ && annotationUsesLineWidth(*activeTool_));
    widthLabel_->setText(QStringLiteral("线宽 %1px").arg(style_.lineWidth));
    widthLabel_->setToolTip(QStringLiteral("画布上滚轮调整线宽（1–24px，每格 1px）"));
    widthLabel_->setAccessibleName(widthLabel_->text() + QStringLiteral("；") +
                                   widthLabel_->toolTip());
    colors_->setColor(style_.color);
    colors_->setEnabled(!selectMode_ && activeTool_ != AnnotationType::Mosaic);
    const bool textTool = !selectMode_ && activeTool_ && *activeTool_ == AnnotationType::Text;
    sizes_->setEnabled(textTool);
    sizes_->setVisible(textTool);
    textSizeLabel_->setVisible(textTool);
    textSizeLabel_->setText(QStringLiteral("字号 %1px").arg(style_.textSize));
    textSizeLabel_->setToolTip(QStringLiteral("画布上滚轮调整字号（9–96px，每格 1px）"));
    textSizeLabel_->setAccessibleName(textSizeLabel_->text() + QStringLiteral("；") +
                                      textSizeLabel_->toolTip());
    for (auto* button : {boldButton_, italicButton_, underlineButton_})
        button->setVisible(textTool);
    boldButton_->setChecked(style_.bold);
    italicButton_->setChecked(style_.italic);
    underlineButton_->setChecked(style_.underline);
    auto* grid = static_cast<QGridLayout*>(
        static_cast<QVBoxLayout*>(editToolbar_->layout())->itemAt(0)->layout());
    const int columns = std::clamp(width() / 44, 1, 5);
    selectButton_->setEnabled(available);
    selectButton_->setChecked(selectMode_);
    grid->addWidget(selectButton_, 0, 0);
    for (int index = 0; index < toolButtons_.size(); ++index)
    {
        const auto type = toolTypes_[index];
        auto* button = toolButtons_[index];
        button->setChecked(!selectMode_ && activeTool_ == type);
        if (button->toolTip() != annotationToolTip(type))
        {
            button->setText(annotationToolName(type));
            button->setToolTip(annotationToolTip(type));
            button->setAccessibleName(button->toolTip());
            button->setIcon(toolbarIcon(static_cast<ToolbarIcon>(type)));
        }
        grid->addWidget(toolButtons_[index], (index + 1) / columns, (index + 1) % columns);
    }
    const int activeSlot = activeTool_ ? annotationToolSlot(*activeTool_) : -1;
    const bool grouped = activeSlot == 0 || activeSlot == 1 || activeSlot == 4;
    int x = 4;
    for (auto* button : subtoolButtons_)
    {
        const auto type = static_cast<AnnotationType>(button->property("annotationType").toInt());
        const bool visible = grouped && annotationToolSlot(type) == activeSlot;
        button->setVisible(visible);
        button->setChecked(activeTool_ == type);
        if (visible)
        {
            button->move(x, 4);
            x += 32;
        }
    }
    toolBar_->setEnabled(available);
    toolBar_->setVisible(available && grouped);
    editToolbar_->adjustSize();
    const bool compact = editToolbar_->width() > width() || editToolbar_->height() > height();
    editToolbar_->move(std::max(0, (width() - editToolbar_->width()) / 2),
                       std::max(0, height() - editToolbar_->height()));
    editToolbar_->setVisible(editing_ && !compact);
    editMoreButton_->setGeometry(0, 0, std::min(44, width()), std::min(22, height()));
    editMoreButton_->setVisible(editing_ && compact);
    const auto syncMenu = [&](const auto& self, QMenu* menu) -> void
    {
        for (QAction* action : menu->actions())
        {
            if (action->data().toString().isEmpty())
                continue;
            if (auto* button = findChild<QPushButton*>(action->data().toString()))
            {
                action->setEnabled(available && button->isEnabled());
                action->setChecked(button->isChecked());
                action->setIcon(button->icon());
                action->setToolTip(button->toolTip());
                if (colors_->isAncestorOf(button))
                    action->setText(button->toolTip());
                else if (!button->text().isEmpty())
                    action->setText(button->text());
            }
            if (action->data().toString() == colors_->objectName())
                action->setEnabled(available && colors_->isEnabled());
            if (auto* combo = findChild<QComboBox*>(action->data().toString()))
            {
                action->setEnabled(available && combo->isEnabled());
                for (int index = 0; index < combo->count(); ++index)
                    action->menu()->actions()[index]->setChecked(combo->currentIndex() == index);
            }
            if (action->menu())
                self(self, action->menu());
        }
    };
    syncMenu(syncMenu, editMenu_);
    if (formatMenu_)
        formatMenu_->menuAction()->setEnabled(available && textTool);
    positionControls();
}
void StickerWindow::activateTool(AnnotationType type, bool toggle)
{
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_)
        return;
    finishText(true);
    clearAnnotationSelection();
    selectMode_ = false;
    lastAnnotationSelected_ = false;
    widthWheel_.reset();
    wheelRemainder_ = 0;
    toolTypes_[annotationToolSlot(type)] = type;
    activeTool_ =
        toggle && activeTool_ == type ? std::nullopt : std::optional<AnnotationType>(type);
    setCursor(!activeTool_                           ? Qt::ArrowCursor
              : *activeTool_ == AnnotationType::Text ? Qt::IBeamCursor
                                                     : Qt::CrossCursor);
    setFocus(Qt::OtherFocusReason);
    updateEditToolbar();
    if (activeTool_ == AnnotationType::Mosaic && !mosaicNoticeShown_)
    {
        mosaicNoticeShown_ = true;
        showStatus(QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖"), true);
    }
}
void StickerWindow::toggleSelectMode()
{
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_)
        return;
    finishText(true);
    activeTool_.reset();
    lastAnnotationSelected_ = false;
    widthWheel_.reset();
    wheelRemainder_ = 0;
    selectMode_ = !selectMode_;
    if (!selectMode_)
        clearAnnotationSelection();
    else
        showStatus(QStringLiteral("选择标注：拖动移动，拖动上方圆点旋转"), true);
    setFocus(Qt::OtherFocusReason);
    updateEditToolbar();
    setCursor(Qt::ArrowCursor);
    update();
}
qreal StickerWindow::annotationLogicalFactor() const { return scale_ / screenDpr(); }
int StickerWindow::annotationAt(QPointF point) const
{
    const qreal factor = annotationLogicalFactor();
    if (factor <= 0)
        return -1;
    const QPointF physical = physicalPoint(point);
    const qreal tolerance = 4;
    const auto& items = annotations();
    for (qsizetype index = items.size() - 1; index >= 0; --index)
        if (annotationHitTest(items[index], physical, tolerance))
            return int(index);
    return -1;
}
bool StickerWindow::rotationHandleHit(QPointF point) const
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations().size())
        return false;
    const qreal factor = annotationLogicalFactor();
    if (factor <= 0)
        return false;
    const auto& annotation = annotations()[selectedAnnotation_];
    const QPointF physical = physicalPoint(point);
    const QPointF handle = annotationRotationHandle(annotation, 24 / factor);
    return std::hypot(physical.x() - handle.x(), physical.y() - handle.y()) <= 10 / factor;
}
bool StickerWindow::beginAnnotationGesture(QPointF point, Qt::KeyboardModifiers modifiers)
{
    if (!selectMode_ || !editing_)
        return false;
    Q_UNUSED(modifiers);
    if (rotationHandleHit(point))
    {
        startAnnotationGesture(AnnotationGesture::Rotate, selectedAnnotation_, point);
        return true;
    }
    const int hit = annotationAt(point);
    if (hit < 0)
    {
        clearAnnotationSelection();
        updateEditToolbar();
        setCursor(Qt::ArrowCursor);
        update();
        return true;
    }
    startAnnotationGesture(AnnotationGesture::Move, hit, point);
    return true;
}
bool StickerWindow::beginSelectedAnnotationGesture(QPointF point)
{
    if (!editing_ || !activeTool_ || selectedAnnotation_ < 0 ||
        selectedAnnotation_ >= annotations().size())
        return false;
    if (rotationHandleHit(point))
    {
        startAnnotationGesture(AnnotationGesture::Rotate, selectedAnnotation_, point);
        return true;
    }
    const auto& annotation = annotations()[selectedAnnotation_];
    if (!annotationHitTest(annotation, physicalPoint(point), 4))
        return false;
    startAnnotationGesture(AnnotationGesture::Move, selectedAnnotation_, point);
    return true;
}
void StickerWindow::startAnnotationGesture(AnnotationGesture gesture, int index, QPointF point)
{
    if (index < 0 || index >= annotations().size())
        return;
    selectedAnnotation_ = index;
    const auto& annotation = annotations()[index];
    annotationBeforeEdit_ = annotation;
    annotationPressPhysical_ = physicalPoint(point);
    annotationStartCenter_ = annotationCenter(annotation);
    annotationStartAngle_ = std::atan2(annotationPressPhysical_.y() - annotationStartCenter_.y(),
                                       annotationPressPhysical_.x() - annotationStartCenter_.x()) *
                            180.0 / 3.14159265358979323846;
    annotationGesture_ = gesture;
    history_.beginEdit();
    updateEditToolbar();
    update();
}
void StickerWindow::updateAnnotationGesture(QPointF point, Qt::KeyboardModifiers modifiers)
{
    if (annotationGesture_ == AnnotationGesture::None || selectedAnnotation_ < 0 ||
        selectedAnnotation_ >= annotations().size())
        return;
    const QPointF physical = physicalPoint(point);
    Annotation next = annotationBeforeEdit_;
    if (annotationGesture_ == AnnotationGesture::Move)
        translateAnnotation(next, physical - annotationPressPhysical_);
    else
    {
        const qreal angle = std::atan2(physical.y() - annotationStartCenter_.y(),
                                       physical.x() - annotationStartCenter_.x()) *
                            180.0 / 3.14159265358979323846;
        qreal rotation = annotationBeforeEdit_.rotation + angle - annotationStartAngle_;
        if (modifiers.testFlag(Qt::ShiftModifier))
            rotation = snapAnnotationRotation(rotation);
        setAnnotationRotation(next, rotation);
    }
    if (history_.updateAnnotation(selectedAnnotation_, std::move(next)))
        update();
}
void StickerWindow::finishAnnotationGesture()
{
    if (annotationGesture_ == AnnotationGesture::None)
        return;
    if (history_.commitEdit())
        markDirty();
    annotationGesture_ = AnnotationGesture::None;
    annotationBeforeEdit_ = {};
    updateEditToolbar();
    update();
}
void StickerWindow::clearAnnotationSelection()
{
    if (annotationGesture_ != AnnotationGesture::None)
        history_.cancelEdit();
    annotationGesture_ = AnnotationGesture::None;
    pendingSelectedGesture_ = false;
    selectedAnnotation_ = -1;
    annotationBeforeEdit_ = {};
}
void StickerWindow::paintAnnotationSelection(QPainter& painter) const
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations().size() || !editing_)
        return;
    const qreal factor = annotationLogicalFactor();
    if (factor <= 0)
        return;
    const auto& annotation = annotations()[selectedAnnotation_];
    QPolygonF corners;
    for (const QPointF point : annotationCorners(annotation))
        corners << point * factor;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(245, 130, 35), 1.4, Qt::DashLine);
    pen.setDashPattern({4, 3});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(corners);
    const QRectF base = annotationBaseBounds(annotation);
    const QPointF top =
        annotationFromBase(annotation, QPointF(base.center().x(), base.top())) * factor;
    const QPointF handle = annotationRotationHandle(annotation, 24 / factor) * factor;
    painter.drawLine(top, handle);
    painter.setPen(QPen(QColor(245, 130, 35), 1.5));
    painter.setBrush(QColor(245, 130, 35));
    painter.drawEllipse(handle, 4, 4);
    painter.restore();
}
void StickerWindow::markDirty()
{
    saved_ = false;
    statusTimeout_.stop();
    status_->hide();
}
QPointF StickerWindow::physicalPoint(QPointF point) const
{
    const qreal factor = screenDpr() / scale_;
    return {std::clamp(point.x() * factor, qreal(0), qreal(image_.width())),
            std::clamp(point.y() * factor, qreal(0), qreal(image_.height()))};
}
void StickerWindow::updateAnnotation(QPointF point)
{
    if (!draft_)
        return;
    if (draft_->type == AnnotationType::Freehand)
    {
        // 空点击不产生画笔标注；有位移才把起点与后续点加入草稿。
        const QPointF physical = physicalPoint(point);
        if (draft_->points.isEmpty() && physical != draft_->first)
            appendFreehandPoint(*draft_, draft_->first);
        if (!draft_->points.isEmpty())
            appendFreehandPoint(*draft_, physical);
    }
    else
        draft_->last = physicalPoint(point);
}
void StickerWindow::undoAnnotation()
{
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_ ||
        annotationGesture_ != AnnotationGesture::None)
        return;
    finishText(true);
    clearAnnotationSelection();
    lastAnnotationSelected_ = false;
    if (history_.undo())
        markDirty();
    updateEditToolbar();
    update();
}
void StickerWindow::redoAnnotation()
{
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_ ||
        annotationGesture_ != AnnotationGesture::None)
        return;
    finishText(true);
    clearAnnotationSelection();
    lastAnnotationSelected_ = false;
    if (history_.redo())
        markDirty();
    updateEditToolbar();
    update();
}
void StickerWindow::startText(QPointF point)
{
    if (!textEditor_)
    {
        textEditor_ = new AnnotationTextEdit(this);
        textEditor_->setObjectName(QStringLiteral("stickerEditTextEditor"));
        textEditor_->installEventFilter(this);
        connect(textEditor_, &AnnotationTextEdit::commitRequested, this,
                [this] { finishText(true); });
        connect(textEditor_, &AnnotationTextEdit::cancelRequested, this,
                [this] { finishText(false); });
        connect(textEditor_, &AnnotationTextEdit::formatToggleRequested, this,
                [this](int format) { toggleTextStyle(static_cast<TextStyleToggle>(format)); });
    }
    lastAnnotationSelected_ = false;
    textFocusCommitPending_ = false;
    textDraft_ = Annotation{AnnotationType::Text, style_, physicalPoint(point), {}, {}, {}};
    QPalette palette = textEditor_->palette();
    palette.setColor(QPalette::Text, style_.color);
    QColor placeholder = style_.color;
    placeholder.setAlpha(160);
    palette.setColor(QPalette::PlaceholderText, placeholder);
    textEditor_->setPalette(palette);
    textEditor_->clear();
    positionTextEditor();
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(Qt::MouseFocusReason);
}
void StickerWindow::positionTextEditor()
{
    if (!textEditor_ || textDraft_.type != AnnotationType::Text)
        return;
    const qreal factor = scale_ / screenDpr();
    QFont editorFont = annotationFont(textDraft_.style);
    editorFont.setPointSizeF(textDraft_.style.textSize * factor * 72.0 / logicalDpiY());
    textEditor_->setAnnotationFont(editorFont);
    const QPoint point = (textDraft_.first * factor).toPoint();
    textEditor_->setEditingArea(point, rect());
    textDraft_.first = physicalPoint(textEditor_->pos());
}
void StickerWindow::finishText(bool commit, bool restoreFocus)
{
    textFocusCommitPending_ = false;
    if (!textEditor_ || !textEditor_->isVisible())
        return;
    if (commit)
        QGuiApplication::inputMethod()->commit();
    else
        QGuiApplication::inputMethod()->reset();
    textEditor_->clearPreedit();
    textDraft_.text = textEditor_->toPlainText();
    textDraft_.style = style_;
    textEditor_->hide();
    textEditor_->clear();
    bool added = false;
    if (commit)
    {
        added = history_.add(std::move(textDraft_));
        if (added)
        {
            markDirty();
            selectedAnnotation_ = int(annotations().size()) - 1;
            if (restoreFocus)
                showStatus(QStringLiteral("文本框已选中：拖动移动，拖圆点旋转"), true);
        }
    }
    lastAnnotationSelected_ = added;
    textDraft_ = {};
    if (restoreFocus && editing_)
        setFocus(Qt::OtherFocusReason);
    updateEditToolbar();
    update();
}
void StickerWindow::applyTextEditorFont()
{
    if (!textEditor_ || !textEditor_->isVisible())
        return;
    const qreal factor = scale_ / screenDpr();
    QFont editorFont = annotationFont(style_);
    editorFont.setPointSizeF(style_.textSize * factor * 72.0 / logicalDpiY());
    textEditor_->setAnnotationFont(editorFont);
}
void StickerWindow::setTextSize(int size)
{
    textFocusCommitPending_ = false;
    style_.textSize = std::clamp(size, annotationMinimumTextSize, annotationMaximumTextSize);
    if (sizes_)
    {
        int preset = -1;
        for (int index = 0; index < sizes_->count(); ++index)
            if (annotationTextSizes[index] == style_.textSize)
                preset = index;
        const QSignalBlocker blocker(sizes_);
        sizes_->setCurrentIndex(preset);
    }
    if (textEditor_ && textEditor_->isVisible())
    {
        textDraft_.style = style_;
        applyTextEditorFont();
        if (!textEditor_->hasFocus())
            textEditor_->setFocus(Qt::OtherFocusReason);
        if (!textEditor_->hasFocus())
            QTimer::singleShot(0, this,
                               [this]
                               {
                                   if (textEditor_ && textEditor_->isVisible() &&
                                       !textEditor_->hasFocus())
                                       textEditor_->setFocus(Qt::OtherFocusReason);
                               });
    }
    else if (lastAnnotationSelected_ && history_.setLastTextStyle(style_.textSize, style_.bold,
                                                                  style_.italic, style_.underline))
        markDirty();
    updateEditToolbar();
    update();
}
void StickerWindow::toggleTextStyle(TextStyleToggle toggle)
{
    if (!editing_ || !activeTool_ || *activeTool_ != AnnotationType::Text)
        return;
    textFocusCommitPending_ = false;
    bool* value = toggle == TextStyleToggle::Bold     ? &style_.bold
                  : toggle == TextStyleToggle::Italic ? &style_.italic
                                                      : &style_.underline;
    *value = !*value;
    if (textEditor_ && textEditor_->isVisible())
    {
        textDraft_.style = style_;
        applyTextEditorFont();
        if (!textEditor_->hasFocus())
            textEditor_->setFocus(Qt::OtherFocusReason);
        if (!textEditor_->hasFocus())
            QTimer::singleShot(0, this,
                               [this]
                               {
                                   if (textEditor_ && textEditor_->isVisible() &&
                                       !textEditor_->hasFocus())
                                       textEditor_->setFocus(Qt::OtherFocusReason);
                               });
    }
    else if (lastAnnotationSelected_ && history_.setLastTextStyle(style_.textSize, style_.bold,
                                                                  style_.italic, style_.underline))
        markDirty();
    updateEditToolbar();
    update();
}
void StickerWindow::adjustTextSize(QPoint angleDelta, QPoint pixelDelta, Qt::ScrollPhase phase)
{
    if (!editing_ || !activeTool_ || *activeTool_ != AnnotationType::Text)
        return;
    if (phase == Qt::ScrollBegin)
        widthWheel_.reset();
    setTextSize(widthWheel_.adjust(style_.textSize, angleDelta, pixelDelta,
                                   annotationMinimumTextSize, annotationMaximumTextSize));
    if (phase == Qt::ScrollEnd)
        widthWheel_.reset();
}
bool StickerWindow::eventFilter(QObject* watched, QEvent* event)
{
    // 格式按钮按下即应用；不抢编辑框焦点，避免 FocusOut 提前提交文本。
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
    {
        if (watched == boldButton_)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                toggleTextStyle(TextStyleToggle::Bold);
            }
            event->accept();
            return true;
        }
        if (watched == italicButton_)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                toggleTextStyle(TextStyleToggle::Italic);
            }
            event->accept();
            return true;
        }
        if (watched == underlineButton_)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                toggleTextStyle(TextStyleToggle::Underline);
            }
            event->accept();
            return true;
        }
    }
    if (watched == textEditor_ && event->type() == QEvent::Wheel)
    {
        auto* wheel = static_cast<QWheelEvent*>(event);
        adjustTextSize(wheel->angleDelta(), wheel->pixelDelta(), wheel->phase());
        event->accept();
        return true;
    }
    if (watched != textEditor_ && event->type() == QEvent::Wheel)
    {
        event->accept();
        return true;
    }
    // NoFocus 选项即使选择同一档也属于点击别处，不能只依赖索引变化或 FocusOut。
    if ((colors_->isAncestorOf(qobject_cast<QWidget*>(watched)) || watched == sizes_) &&
        event->type() == QEvent::MouseButtonPress)
        finishText(true, false);
    if ((watched == editToolbar_ || watched == toolBar_) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease))
    {
        if (event->type() == QEvent::MouseButtonPress)
            finishText(true, false);
        event->accept();
        return true;
    }
    if (watched == textEditor_ && event->type() == QEvent::FocusOut)
    {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        if (reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason)
        {
            const auto over = [](QWidget* control)
            {
                return control && control->isVisible() &&
                       control->rect().contains(control->mapFromGlobal(QCursor::pos()));
            };
            bool textControl = over(boldButton_) || over(italicButton_) || over(underlineButton_);
            textControl |= over(sizes_);
            if (!textControl)
            {
                textFocusCommitPending_ = true;
                QTimer::singleShot(0, this,
                                   [this]
                                   {
                                       if (textFocusCommitPending_)
                                       {
                                           textFocusCommitPending_ = false;
                                           finishText(true, false);
                                       }
                                   });
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
void StickerWindow::contextMenuEvent(QContextMenuEvent* event)
{
    if (editing_ && !hasPendingInteraction() && !quitInteraction_ && !draft_)
    {
        finishText(true);
        updateEditToolbar();
        editMenu_->popup(event->globalPos());
        event->accept();
    }
}
void StickerWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || editing_ || hasPendingInteraction() ||
        quitInteraction_)
        return;
    std::optional<bool> preference = false;
    if (actions_.loadDoubleClickClose)
        preference = actions_.loadDoubleClickClose();
    if (preference.value_or(false))
    {
        close();
        event->accept();
        return;
    }
    if (!preference.has_value() && actions_.confirmDoubleClickClose)
    {
        confirmingDoubleClickClose_ = true;
        const bool enable = actions_.confirmDoubleClickClose(this);
        confirmingDoubleClickClose_ = false;
        if (enable)
        {
            if (actions_.saveDoubleClickClose)
            {
                const QString error = actions_.saveDoubleClickClose(true);
                if (!error.isEmpty())
                {
                    showStatus(error, false);
                    event->accept();
                    return;
                }
            }
            close();
            event->accept();
            return;
        }
        if (actions_.saveDoubleClickClose)
        {
            const QString error = actions_.saveDoubleClickClose(false);
            if (!error.isEmpty())
                qWarning().noquote() << error;
        }
    }
    setEditing(true);
    event->accept();
}
void StickerWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !editing_ && !hasPendingInteraction() &&
        !quitInteraction_ && !event->isAutoRepeat())
    {
        setEditing(true);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && editing_ && !hasPendingInteraction() && !quitInteraction_)
    {
        if (selectMode_ && selectedAnnotation_ >= 0)
        {
            clearAnnotationSelection();
            updateEditToolbar();
            setCursor(Qt::ArrowCursor);
            update();
        }
        else if (draft_)
        {
            draft_.reset();
            updateEditToolbar();
            update();
        }
        else if (textEditor_ && textEditor_->isVisible())
        {
            if (textEditor_->isComposing())
            {
                QGuiApplication::inputMethod()->reset();
                textEditor_->clearPreedit();
            }
            else
                finishText(false);
        }
        else
            setEditing(false);
        event->accept();
    }
    else
        QWidget::keyPressEvent(event);
}
void StickerWindow::setQuitInteraction(bool busy)
{
    quitInteraction_ = busy;
    dragging_ = false;
    draft_.reset();
    controls_->setEnabled(!busy && !saveDialogOpen_);
    moreButton_->setEnabled(!busy && !saveDialogOpen_);
    for (QAction* action : actions())
        action->setEnabled(!busy && !saveDialogOpen_);
    updateEditToolbar();
}

}
