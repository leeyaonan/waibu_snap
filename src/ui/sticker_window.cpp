#include "ui/sticker_window.h"
#include "core/sticker_geometry.h"
#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
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
                              QStringLiteral("复制"), QStringLiteral("×")};
    const QString names[] = {
        QStringLiteral("stickerZoomOutButton"), QStringLiteral("stickerZoomInButton"),
        QStringLiteral("stickerResetButton"),   QStringLiteral("stickerSaveButton"),
        QStringLiteral("stickerCopyButton"),    QStringLiteral("stickerCloseButton")};
    const QString tips[] = {QStringLiteral("缩小"),      QStringLiteral("放大"),
                            QStringLiteral("恢复 100%"), QStringLiteral("保存原图"),
                            QStringLiteral("复制原图"),  QStringLiteral("关闭贴图")};
    auto* menu = new QMenu(this);
    setContextMenuPolicy(Qt::ActionsContextMenu);
    for (int index = 0; index < 6; ++index)
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
        layout->addWidget(button, index / 6, index % 6);
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
                    else
                        close();
                });
    }
    // 极小贴图容不下六个控件时，用可发现的操作菜单保留全部入口。
    moreButton_ = new QPushButton(QStringLiteral("⋯"), this);
    moreButton_->setObjectName(QStringLiteral("stickerMoreButton"));
    moreButton_->setFocusPolicy(Qt::NoFocus);
    moreButton_->setToolTip(QStringLiteral("贴图操作：缩放、100%、保存、复制、关闭；也可右键打开"));
    moreButton_->setAccessibleName(QStringLiteral("贴图操作"));
    moreButton_->hide();
    connect(moreButton_, &QPushButton::clicked, this,
            [this, menu] { menu->popup(mapToGlobal(QPoint(0, moreButton_->height()))); });
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("stickerStatus"));
    status_->setTextFormat(Qt::PlainText);
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
    editing_ = editing;
    dragging_ = false;
    controls_->setEnabled(!editing && !saveDialogOpen_);
    moreButton_->setEnabled(!editing && !saveDialogOpen_);
    setCursor(editing ? Qt::ArrowCursor : Qt::SizeAllCursor);
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
    if (closed_ || editing_ || saveDialogOpen_)
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
    if (closed_ || editing_ || saveDialogOpen_)
        return;
    const qreal next = clampedStickerScale(scale);
    const QSize nextSize = stickerWindowSize(image_.size(), next, screenDpr());
    if (nextSize.isEmpty())
        return;
    const QPointF position =
        anchoredStickerPosition(geometryPosition_, size(), nextSize, globalAnchor);
    scale_ = next;
    applyGeometry(position, nextSize);
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
    painter.setRenderHint(QPainter::SmoothPixmapTransform, scale_ != 1);
    if (scale_ == 1)
    {
        // 整数逻辑窗口可能多出不足一个逻辑像素的边缘；原像素绘制保持物理 1:1。
        painter.fillRect(rect(), Qt::black);
        painter.scale(1 / screenDpr(), 1 / screenDpr());
        painter.drawImage(QPointF(0, 0), image_);
    }
    else
        painter.drawImage(QRectF(rect()), image_, QRectF(image_.rect()));
}
void StickerWindow::positionControls()
{
    if (!controls_)
        return;
    auto* layout = static_cast<QGridLayout*>(controls_->layout());
    const auto buttons = controls_->findChildren<QPushButton*>();
    const int columns = std::clamp(width() / 44, 1, 6);
    for (int index = 0; index < buttons.size(); ++index)
        layout->addWidget(buttons[index], index / columns, index % columns);
    controls_->adjustSize();
    controls_->move(std::max(0, (width() - controls_->width()) / 2), 0);
    if (moreButton_)
    {
        const bool compact = controls_->width() > width() || controls_->height() > height();
        controls_->setVisible(hovered_ && !compact);
        moreButton_->setGeometry(0, 0, std::min(44, width()), std::min(22, height()));
        moreButton_->setVisible(hovered_ && compact);
    }
    if (status_)
    {
        status_->setFixedWidth(width());
        status_->adjustSize();
        status_->move(0, std::max(0, height() - status_->height()));
    }
}
void StickerWindow::resizeEvent(QResizeEvent*) { positionControls(); }
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
    if (event->button() != Qt::LeftButton || closed_ || editing_ || saveDialogOpen_)
        return;
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
    if (dragging_ && !editing_ && !saveDialogOpen_)
        moveDrag(event->globalPosition());
}
void StickerWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_)
    {
        moveDrag(event->globalPosition());
        dragging_ = false;
    }
}
void StickerWindow::wheelEvent(QWheelEvent* event)
{
    if (closed_ || editing_ || saveDialogOpen_)
        return;
    wheelRemainder_ += event->angleDelta().y() / 120.0;
    const int steps = int(std::trunc(wheelRemainder_));
    wheelRemainder_ -= steps;
    if (steps)
        setScale(steppedStickerScale(scale_, steps), event->globalPosition());
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
    if (closed_ || saveDialogOpen_ || editing_)
        return;
    QPointer<StickerWindow> self(this);
    const auto copy = actions_.copyImage;
    const auto result = copy(image_);
    if (!self || closed_)
        return;
    showStatus(result.success
                   ? QStringLiteral("已复制")
                   : QStringLiteral("复制失败：%1 点击「复制」重试。").arg(result.explanation),
               result.success);
}
void StickerWindow::saveImage()
{
    if (closed_ || saveDialogOpen_ || editing_)
        return;
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString suggestion = suggestedPngPath(directory);
    saveDialogOpen_ = true;
    dragging_ = false;
    controls_->setEnabled(false);
    moreButton_->setEnabled(false);
    QPointer<StickerWindow> self(this);
    const auto choose = actions_.chooseSavePath;
    QString path;
    while (true)
    {
        const QString chosen = choose(this, suggestion);
        if (!self)
            return;
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
        return;
    controls_->setEnabled(!editing_);
    moreButton_->setEnabled(!editing_);
    if (!path.isEmpty())
        exportToPath(path);
}
bool StickerWindow::exportToPath(const QString& path)
{
    if (closed_ || saveDialogOpen_ || editing_ || path.isEmpty())
        return false;
    const auto result = exportPngToPath(image_, path);
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
void StickerWindow::closeEvent(QCloseEvent* event)
{
    event->accept();
    if (closed_)
        return;
    closed_ = true;
    dragging_ = false;
    statusTimeout_.stop();
    // 4a 直接关闭；4b 在此入口接未保存确认，关闭后的图片不保留历史。
    emit closed();
}
}
