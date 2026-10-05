#include "ui/sticker_window.h"
#include "core/sticker_geometry.h"
#include "output/annotation_renderer.h"
#include "ui/annotation_text_edit.h"
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
StickerWindow::StickerWindow(QImage image, QPoint position, bool saved, StickerActions actions)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                           Qt::WindowDoesNotAcceptFocus),
      image_(std::move(image)), actions_(std::move(actions)), geometryPosition_(position),
      saved_(saved)
{
    setObjectName(QStringLiteral("stickerWindow"));
    setWindowTitle(QStringLiteral("WaibuSnap 贴图"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setCursor(Qt::SizeAllCursor);
    // 正常钉图输入已为 DPR=1，保持隐式共享，避免再次分配整图像素。
    if (image_.devicePixelRatio() != 1)
        image_.setDevicePixelRatio(1);
    if (!actions_.copyImage)
        actions_.copyImage = copyImageToClipboard;
    if (!actions_.chooseSavePath)
        actions_.chooseSavePath = [](QWidget* parent, const QString& suggestion)
        {
            return QFileDialog::getSaveFileName(parent, QStringLiteral("保存贴图为 PNG"),
                                                suggestion, QStringLiteral("PNG 图片 (*.png)"));
        };
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
            dialog.exec();
            return dialog.clickedButton() == save ? StickerCloseDecision::Save
                   : dialog.clickedButton() == cancel || !dialog.clickedButton()
                       ? StickerCloseDecision::Cancel
                       : StickerCloseDecision::Discard;
        };
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
    draft_.reset();
    dragging_ = false;
    const QRect geometry = this->geometry();
    const QPointF precisePosition = geometryPosition_;
    const bool visible = isVisible();
    QPointer<QScreen> screen = windowHandle()->screen();
    // 切换焦点模式时重建原生窗口，避免运行中修改 NSPanel 非激活样式留下 AppKit 焦点状态。
    hide();
    destroy();
    editing_ = editing;
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                   (editing ? Qt::WindowFlags() : Qt::WindowDoesNotAcceptFocus));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(editing ? Qt::StrongFocus : Qt::NoFocus);
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
    setContextMenuPolicy(editing ? Qt::NoContextMenu : Qt::ActionsContextMenu);
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
    painter.fillRect(rect(), Qt::black);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, scale_ != 1);
    const qreal factor = scale_ / screenDpr();
    painter.scale(factor, factor);
    painter.drawImage(QPointF(0, 0), image_);
    paintAnnotations(painter, annotations());
    if (draft_)
        paintAnnotation(painter, *draft_);
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
        status_->move(0, std::max(0, bottom - status_->height()));
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
        quitInteraction_)
        return;
    if (editing_)
    {
        finishText(true);
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
        update();
        event->accept();
        return;
    }
    dragging_ = true;
    dragFraction_ = {event->position().x() / width(), event->position().y() / height()};
    event->accept();
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
    if (draft_ && !saveDialogOpen_ && !quitInteraction_)
    {
        updateAnnotation(event->position());
        update();
    }
    if (dragging_ && !editing_ && !saveDialogOpen_ && !quitInteraction_)
        moveDrag(event->globalPosition());
}
void StickerWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && draft_ && !saveDialogOpen_ && !quitInteraction_)
    {
        updateAnnotation(event->position());
        if (history_.add(std::move(*draft_)))
            markDirty();
        draft_.reset();
        updateEditToolbar();
        update();
    }
    if (event->button() == Qt::LeftButton && dragging_)
    {
        moveDrag(event->globalPosition());
        dragging_ = false;
    }
}
void StickerWindow::wheelEvent(QWheelEvent* event)
{
    if (closed_ || saveDialogOpen_ || confirmingClose_ || quitInteraction_ || draft_)
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
        update();
    }
    event->accept();
}
void StickerWindow::showStatus(const QString& text, bool temporary)
{
    statusTimeout_.stop();
    status_->setText(text);
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
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString suggestion = suggestedPngPath(directory);
    saveDialogOpen_ = true;
    dragging_ = false;
    controls_->setEnabled(false);
    moreButton_->setEnabled(false);
    updateEditToolbar();
    QPointer<StickerWindow> self(this);
    const auto choose = actions_.chooseSavePath;
    QString path;
    while (true)
    {
        const QString chosen = choose(this, suggestion);
        if (!self)
            return false;
        if (closed_ || chosen.isEmpty())
            break;
        path = pngFilePath(chosen);
        if (path == chosen || !QFileInfo::exists(path))
            break;
        suggestion = path;
        path.clear();
        showStatus(QStringLiteral("补全后缀后文件已存在，请在保存面板确认覆盖。"), false);
    }
    saveDialogOpen_ = false;
    if (closed_)
        return false;
    controls_->setEnabled(!quitInteraction_);
    moreButton_->setEnabled(!quitInteraction_);
    updateEditToolbar();
    if (path.isEmpty())
    {
        showStatus(QStringLiteral("保存已取消，贴图仍保留，可再次保存。"), true);
        return false;
    }
    return exportToPath(path);
}
bool StickerWindow::exportToPath(const QString& path)
{
    if (closed_ || saveDialogOpen_ || draft_ || path.isEmpty())
        return false;
    finishText(true);
    const auto result = exportPngToPath(renderedImage(), path);
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
        if (!saved_)
        {
            confirmingClose_ = true;
            QPointer<StickerWindow> self(this);
            const auto confirm = actions_.confirmClose;
            const auto decision = confirm(this);
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
    const QString labels[] = {QStringLiteral("矩形"), QStringLiteral("椭圆"),
                              QStringLiteral("直线"), QStringLiteral("箭头"),
                              QStringLiteral("画笔"), QStringLiteral("文本")};
    const QString names[] = {QStringLiteral("stickerEditRectangleToolButton"),
                             QStringLiteral("stickerEditEllipseToolButton"),
                             QStringLiteral("stickerEditLineToolButton"),
                             QStringLiteral("stickerEditArrowToolButton"),
                             QStringLiteral("stickerEditFreehandToolButton"),
                             QStringLiteral("stickerEditTextToolButton")};
    for (int index = 0; index < 6; ++index)
    {
        auto* button = new QPushButton(labels[index], editToolbar_);
        button->setObjectName(names[index]);
        button->setToolTip(labels[index] + QStringLiteral("：点击启用，再点取消工具"));
        button->setFocusPolicy(Qt::NoFocus);
        button->setCheckable(true);
        toolButtons_.append(button);
        tools->addWidget(button, 0, index);
        connect(button, &QPushButton::clicked, this,
                [this, index] { activateTool(static_cast<AnnotationType>(index)); });
        auto* action = editMenu_->addAction(labels[index]);
        action->setData(names[index]);
        action->setCheckable(true);
        connect(action, &QAction::triggered, button, &QPushButton::click);
    }
    auto* options = new QHBoxLayout;
    layout->addLayout(options);
    colors_ = new QComboBox(editToolbar_);
    widths_ = new QComboBox(editToolbar_);
    sizes_ = new QComboBox(editToolbar_);
    colors_->setObjectName(QStringLiteral("stickerEditColorCombo"));
    widths_->setObjectName(QStringLiteral("stickerEditWidthCombo"));
    sizes_->setObjectName(QStringLiteral("stickerEditTextSizeCombo"));
    const QString colorNames[] = {QStringLiteral("红"), QStringLiteral("黄"), QStringLiteral("蓝")};
    for (int index = 0; index < 3; ++index)
    {
        QPixmap swatch(14, 14);
        swatch.fill(annotationColors()[index]);
        colors_->addItem(QIcon(swatch), QStringLiteral("颜色：") + colorNames[index]);
        widths_->addItem(QStringLiteral("线 %1px").arg(annotationLineWidths[index]));
        sizes_->addItem(QStringLiteral("字 %1px").arg(annotationTextSizes[index]));
    }
    widths_->setCurrentIndex(1);
    sizes_->setCurrentIndex(1);
    const QString tips[] = {QStringLiteral("标注颜色"), QStringLiteral("图形线宽（物理像素）"),
                            QStringLiteral("文本字号（物理像素）")};
    const QList<QComboBox*> combos = {colors_, widths_, sizes_};
    for (int index = 0; index < combos.size(); ++index)
    {
        auto* combo = combos[index];
        combo->setFocusPolicy(Qt::NoFocus);
        combo->setToolTip(tips[index]);
        combo->setAccessibleName(tips[index]);
        options->addWidget(combo);
        auto* menu = editMenu_->addMenu(tips[index]);
        menu->menuAction()->setData(combo->objectName());
        for (int option = 0; option < combo->count(); ++option)
        {
            auto* action = menu->addAction(combo->itemText(option));
            action->setCheckable(true);
            connect(action, &QAction::triggered, this,
                    [combo, option] { combo->setCurrentIndex(option); });
        }
    }
    connect(colors_, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                finishText(true);
                style_.color = annotationColors()[index];
                updateEditToolbar();
            });
    connect(widths_, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                finishText(true);
                style_.lineWidth = annotationLineWidths[index];
                updateEditToolbar();
            });
    connect(sizes_, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                finishText(true);
                style_.textSize = annotationTextSizes[index];
                updateEditToolbar();
            });
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
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    redoShortcut_ = new QShortcut(QKeySequence::Redo, this);
    connect(undoShortcut_, &QShortcut::activated, this, &StickerWindow::undoAnnotation);
    connect(redoShortcut_, &QShortcut::activated, this, &StickerWindow::redoAnnotation);
    editMoreButton_ = new QPushButton(QStringLiteral("⋯"), this);
    editMoreButton_->setObjectName(QStringLiteral("stickerEditMoreButton"));
    editMoreButton_->setFocusPolicy(Qt::NoFocus);
    editMoreButton_->setToolTip(
        QStringLiteral("编辑工具：图形、文本、样式、撤销、重做、保存、复制、完成"));
    connect(editMoreButton_, &QPushButton::clicked, this,
            [this]
            {
                finishText(true);
                updateEditToolbar();
                editMenu_->popup(mapToGlobal(QPoint(0, editMoreButton_->height())));
            });
    editToolbar_->installEventFilter(this);
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
    widths_->setEnabled(activeTool_ != AnnotationType::Text);
    sizes_->setEnabled(activeTool_ == AnnotationType::Text);
    auto* grid = static_cast<QGridLayout*>(
        static_cast<QVBoxLayout*>(editToolbar_->layout())->itemAt(0)->layout());
    const int columns = std::clamp(width() / 44, 1, 6);
    for (int index = 0; index < toolButtons_.size(); ++index)
    {
        toolButtons_[index]->setChecked(activeTool_ == static_cast<AnnotationType>(index));
        grid->addWidget(toolButtons_[index], index / columns, index % columns);
    }
    editToolbar_->adjustSize();
    const bool compact = editToolbar_->width() > width() || editToolbar_->height() > height();
    editToolbar_->move(std::max(0, (width() - editToolbar_->width()) / 2),
                       std::max(0, height() - editToolbar_->height()));
    editToolbar_->setVisible(editing_ && !compact);
    editMoreButton_->setGeometry(0, 0, std::min(44, width()), std::min(22, height()));
    editMoreButton_->setVisible(editing_ && compact);
    for (QAction* action : editMenu_->actions())
    {
        if (auto* button = findChild<QPushButton*>(action->data().toString()))
        {
            action->setEnabled(available && button->isEnabled());
            action->setChecked(button->isChecked());
        }
        if (auto* combo = findChild<QComboBox*>(action->data().toString()))
        {
            action->setEnabled(available && combo->isEnabled());
            for (int index = 0; index < combo->count(); ++index)
                action->menu()->actions()[index]->setChecked(combo->currentIndex() == index);
        }
    }
    positionControls();
}
void StickerWindow::activateTool(AnnotationType type)
{
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_)
        return;
    finishText(true);
    activeTool_ = activeTool_ == type ? std::nullopt : std::optional<AnnotationType>(type);
    setCursor(!activeTool_                           ? Qt::ArrowCursor
              : *activeTool_ == AnnotationType::Text ? Qt::IBeamCursor
                                                     : Qt::CrossCursor);
    setFocus(Qt::OtherFocusReason);
    updateEditToolbar();
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
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_)
        return;
    finishText(true);
    if (history_.undo())
        markDirty();
    updateEditToolbar();
    update();
}
void StickerWindow::redoAnnotation()
{
    if (!editing_ || hasPendingInteraction() || quitInteraction_ || draft_)
        return;
    finishText(true);
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
    }
    textDraft_ = Annotation{AnnotationType::Text, style_, physicalPoint(point), {}, {}, {}};
    QPalette palette = textEditor_->palette();
    palette.setColor(QPalette::Text, style_.color);
    palette.setColor(QPalette::Base, QColor(25, 25, 25));
    textEditor_->setPalette(palette);
    textEditor_->clear();
    positionTextEditor();
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(Qt::MouseFocusReason);
}
void StickerWindow::positionTextEditor()
{
    if (!textEditor_)
        return;
    const qreal factor = scale_ / screenDpr();
    QFont editorFont = annotationFont(textDraft_.style);
    editorFont.setPointSizeF(textDraft_.style.textSize * factor * 72.0 / logicalDpiY());
    textEditor_->setFont(editorFont);
    const QPoint point = (textDraft_.first * factor).toPoint();
    textEditor_->setGeometry(point.x(), point.y(), std::max(1, std::min(360, width() - point.x())),
                             std::max(1, std::min(140, height() - point.y())));
}
void StickerWindow::finishText(bool commit, bool restoreFocus)
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
    if (restoreFocus && editing_)
        setFocus(Qt::OtherFocusReason);
    updateEditToolbar();
    update();
}
bool StickerWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editToolbar_ &&
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
            finishText(true, false);
    }
    return QWidget::eventFilter(watched, event);
}
void StickerWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !editing_ && !hasPendingInteraction() &&
        !quitInteraction_)
    {
        setEditing(true);
        event->accept();
    }
}
void StickerWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && editing_ && !hasPendingInteraction() && !quitInteraction_)
    {
        if (draft_)
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
