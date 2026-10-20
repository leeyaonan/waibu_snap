#include "ui/scroll_result_window.h"
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
class ScrollResultView final : public QWidget
{
    Q_OBJECT
  public:
    explicit ScrollResultView(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("scrollResultView"));
        setMouseTracking(true);
    }
    void setVertical(bool vertical) { vertical_ = vertical; }
    void setImage(const QImage& image)
    {
        image_ = image;
        resetCrop();
        setZoom(scale_);
    }
    void setZoom(qreal scale)
    {
        // 总览可低于 10%；交互缩放的 10% 下限由结果窗口约束。
        scale_ = std::clamp(scale, qreal(0.001), qreal(4));
        resize(std::max(1, qRound(image_.width() * scale_)),
               std::max(1, qRound(image_.height() * scale_)));
        update();
    }
    qreal zoom() const { return scale_; }
    void setCropping(bool enabled)
    {
        cropping_ = enabled;
        dragging_ = 0;
        resetCrop();
        setCursor(Qt::ArrowCursor);
        update();
    }
    void resetCrop()
    {
        cropStart_ = 0;
        cropEnd_ = axisLength();
        emit cropChanged();
        update();
    }
    QRect cropRect() const
    {
        return vertical() ? QRect(0, cropStart_, image_.width(), cropEnd_ - cropStart_)
                          : QRect(cropStart_, 0, cropEnd_ - cropStart_, image_.height());
    }
    bool cropping() const { return cropping_; }
  signals:
    void zoomRequested(qreal scale);
    void overviewRequested();
    void cropChanged();

  protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, scale_ < 1);
        // 只提交可见源矩形，不生成一张随缩放倍数增长的预览位图。
        const QRectF target = event->rect();
        const QRectF source(target.x() / scale_, target.y() / scale_, target.width() / scale_,
                            target.height() / scale_);
        painter.drawImage(target, image_, source);
        if (!cropping_)
            return;
        painter.setBrush(QColor(0, 0, 0, 130));
        painter.setPen(Qt::NoPen);
        if (vertical())
        {
            painter.drawRect(QRectF(0, 0, width(), cropStart_ * scale_));
            painter.drawRect(QRectF(0, cropEnd_ * scale_, width(), height() - cropEnd_ * scale_));
        }
        else
        {
            painter.drawRect(QRectF(0, 0, cropStart_ * scale_, height()));
            painter.drawRect(QRectF(cropEnd_ * scale_, 0, width() - cropEnd_ * scale_, height()));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#f0a030")), 3));
        for (int position : {cropStart_, cropEnd_})
            if (vertical())
                painter.drawLine(QPointF(0, position * scale_),
                                 QPointF(width(), position * scale_));
            else
                painter.drawLine(QPointF(position * scale_, 0),
                                 QPointF(position * scale_, height()));
    }
    void wheelEvent(QWheelEvent* event) override
    {
        // Qt 的 ControlModifier 在 macOS 上对应 Command。
        if (event->modifiers().testFlag(Qt::ControlModifier))
        {
            emit zoomRequested(scale_ * std::pow(1.15, event->angleDelta().y() / 120.0));
            event->accept();
        }
        else
            event->ignore();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (!cropping_ && event->button() == Qt::LeftButton)
            emit overviewRequested();
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (!cropping_ || event->button() != Qt::LeftButton)
            return;
        const qreal position = vertical() ? event->position().y() : event->position().x();
        if (std::abs(position - cropStart_ * scale_) <= 12)
            dragging_ = 1;
        else if (std::abs(position - cropEnd_ * scale_) <= 12)
            dragging_ = 2;
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!cropping_)
            return;
        const qreal position = vertical() ? event->position().y() : event->position().x();
        if (dragging_)
        {
            const int pixel = std::clamp(qRound(position / scale_), 0, axisLength());
            if (dragging_ == 1)
                cropStart_ = std::min(pixel, cropEnd_ - 1);
            else
                cropEnd_ = std::max(pixel, cropStart_ + 1);
            emit cropChanged();
            update();
        }
        setCursor(dragging_ || std::abs(position - cropStart_ * scale_) <= 12 ||
                          std::abs(position - cropEnd_ * scale_) <= 12
                      ? vertical() ? Qt::SizeVerCursor : Qt::SizeHorCursor
                      : Qt::ArrowCursor);
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = 0; }

  private:
    bool vertical() const { return vertical_; }
    int axisLength() const { return vertical() ? image_.height() : image_.width(); }
    QImage image_;
    qreal scale_ = 1;
    int cropStart_ = 0;
    int cropEnd_ = 0;
    int dragging_ = 0;
    bool cropping_ = false;
    bool vertical_ = true;
};
ScrollResultWindow::ScrollResultWindow(QImage image, ScrollStitchStats stats,
                                       ScrollResultActions actions)
    : image_(std::move(image)), actions_(std::move(actions))
{
    image_.setDevicePixelRatio(1);
    setObjectName(QStringLiteral("scrollResultWindow"));
    setWindowTitle(QStringLiteral("长图结果"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 700);
    if (!actions_.copyImage)
        actions_.copyImage = copyImageToClipboard;
    auto* layout = new QVBoxLayout(this);
    auto* warning = new QWidget(this);
    warning->setObjectName(QStringLiteral("scrollResultWarningBar"));
    auto* warningLayout = new QHBoxLayout(warning);
    auto* message = new QLabel(warning);
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    warningLayout->addWidget(message, 1);
    auto* retry = new QPushButton(QStringLiteral("重试"), warning);
    retry->setObjectName(QStringLiteral("scrollResultRetryButton"));
    warningLayout->addWidget(retry);
    const QString limitText =
        stats.memoryLimited
            ? QStringLiteral("已达内存预算上限，结果已保留；缩窄选区可采集更长内容。")
            : QStringLiteral("已达长度上限，结果已保留。");
    if (stats.warnings)
    {
        message->setText(
            QStringLiteral(
                "⚠ 本次采集有 %1 处可能不完整（重叠范围不足、尾部未验证或采集中断），建议重试。")
                .arg(stats.warnings));
        warning->setStyleSheet(QStringLiteral("background: #4a3a12; color: #f0c674;"));
    }
    else if (stats.limitReached)
    {
        message->setText(limitText);
        warning->setStyleSheet(QStringLiteral("background: #1f3a24; color: #9fd8a8;"));
    }
    if (stats.warnings && stats.limitReached)
    {
        auto* limit = new QLabel(limitText, this);
        limit->setObjectName(QStringLiteral("scrollResultLimitBar"));
        limit->setStyleSheet(QStringLiteral("background: #1f3a24; color: #9fd8a8; padding: 6px;"));
        layout->addWidget(limit);
    }
    layout->addWidget(warning);
    warning->setVisible(stats.warnings || stats.limitReached);
    retry->setVisible(stats.warnings > 0);
    connect(retry, &QPushButton::clicked, this,
            [this]
            {
                if (saving_)
                    return;
                emit retryRequested();
            });
    scrollArea_ = new QScrollArea(this);
    view_ = new ScrollResultView(scrollArea_);
    view_->setVertical(stats.motionDetected ? stats.axis == ScrollAxis::Vertical
                                            : image_.height() >= image_.width());
    view_->setImage(image_);
    scrollArea_->setWidget(view_);
    scrollArea_->setAlignment(Qt::AlignHCenter);
    layout->addWidget(scrollArea_, 1);
    auto* information = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("scrollResultStatusLabel"));
    information->addWidget(status_, 1);
    layout->addLayout(information);
    const auto button = [&](QHBoxLayout* row, const QString& name, const QString& text)
    {
        auto* result = new QPushButton(text, this);
        result->setObjectName(name);
        row->addWidget(result);
        return result;
    };
    connect(
        button(information, QStringLiteral("scrollResultFitButton"), QStringLiteral("适配窗口")),
        &QPushButton::clicked, this, [this] { fit(); });
    connect(
        button(information, QStringLiteral("scrollResultZoom100Button"), QStringLiteral("100%")),
        &QPushButton::clicked, this, [this] { setZoom(1); });
    notice_ = new QLabel(this);
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->hide();
    layout->addWidget(notice_);
    outputBar_ = new QWidget(this);
    auto* outputs = new QHBoxLayout(outputBar_);
    outputs->setContentsMargins(0, 0, 0, 0);
    outputs->addStretch();
    auto* saveButton =
        button(outputs, QStringLiteral("scrollResultSaveButton"), QStringLiteral("保存"));
    saveButton->setStyleSheet(
        QStringLiteral("background: #a65316; color: white; padding: 6px 16px;"));
    connect(saveButton, &QPushButton::clicked, this, &ScrollResultWindow::save);
    connect(button(outputs, QStringLiteral("scrollResultCopyButton"), QStringLiteral("复制")),
            &QPushButton::clicked, this,
            [this]
            {
                if (!saving_)
                    report(actions_.copyImage(image_));
            });
    auto* pin = button(outputs, QStringLiteral("scrollResultPinButton"), QStringLiteral("钉图"));
    pin->setEnabled(bool(actions_.pinImage));
    connect(pin, &QPushButton::clicked, this,
            [this]
            {
                if (!saving_ && actions_.pinImage)
                    report(actions_.pinImage(image_));
            });
    connect(button(outputs, QStringLiteral("scrollResultCropButton"), QStringLiteral("裁剪")),
            &QPushButton::clicked, this,
            [this]
            {
                if (!saving_)
                    setCropping(true);
            });
    connect(button(outputs, QStringLiteral("scrollResultCloseButton"), QStringLiteral("关闭")),
            &QPushButton::clicked, this, &QWidget::close);
    layout->addWidget(outputBar_);
    cropBar_ = new QWidget(this);
    auto* crops = new QHBoxLayout(cropBar_);
    crops->setContentsMargins(0, 0, 0, 0);
    crops->addStretch();
    connect(button(crops, QStringLiteral("scrollResultCropApplyButton"), QStringLiteral("应用")),
            &QPushButton::clicked, this, &ScrollResultWindow::applyCrop);
    connect(button(crops, QStringLiteral("scrollResultCropResetButton"), QStringLiteral("重置")),
            &QPushButton::clicked, view_, &ScrollResultView::resetCrop);
    connect(button(crops, QStringLiteral("scrollResultCropExitButton"), QStringLiteral("退出裁剪")),
            &QPushButton::clicked, this, [this] { setCropping(false); });
    layout->addWidget(cropBar_);
    cropBar_->hide();
    connect(view_, &ScrollResultView::cropChanged, this, &ScrollResultWindow::updateStatus);
    connect(view_, &ScrollResultView::zoomRequested, this, &ScrollResultWindow::setZoom);
    connect(view_, &ScrollResultView::overviewRequested, this,
            [this]
            {
                if (qFuzzyCompare(zoom(), qreal(1)))
                    fit();
                else
                    setZoom(1);
            });
    updateStatus();
    QTimer::singleShot(0, this,
                       [this]
                       {
                           if (!initialFit_)
                           {
                               initialFit_ = true;
                               fit(true);
                           }
                       });
}
qreal ScrollResultWindow::zoom() const { return view_->zoom(); }
void ScrollResultWindow::setZoom(qreal scale)
{
    initialFit_ = true;
    view_->setZoom(std::clamp(scale, qreal(0.1), qreal(4)));
    updateStatus();
}
void ScrollResultWindow::fit(bool widthOnly)
{
    if (image_.isNull())
        return;
    const QSize viewport = scrollArea_->viewport()->size() - QSize(16, 16);
    qreal scale = std::min(qreal(1), qreal(viewport.width()) / image_.width());
    if (!widthOnly)
        scale = std::min(scale, qreal(viewport.height()) / image_.height());
    initialFit_ = true;
    view_->setZoom(scale);
    updateStatus();
}
void ScrollResultWindow::updateStatus()
{
    const QSize size = view_->cropping() ? view_->cropRect().size() : image_.size();
    status_->setText((view_->cropping() ? QStringLiteral("裁剪后 ") : QString()) +
                     QStringLiteral("%1 × %2 像素 · %3%")
                         .arg(size.width())
                         .arg(size.height())
                         .arg(qRound(zoom() * 100)));
}
void ScrollResultWindow::setCropping(bool enabled)
{
    view_->setCropping(enabled);
    outputBar_->setVisible(!enabled);
    cropBar_->setVisible(enabled);
    updateStatus();
}
void ScrollResultWindow::applyCrop()
{
    if (!view_->cropping())
        return;
    const QRect rectangle = view_->cropRect();
    if (rectangle != image_.rect())
    {
        image_ = image_.copy(rectangle);
        output_ = false;
        view_->setImage(image_);
        if (actions_.historyUpdated)
            actions_.historyUpdated(image_);
    }
    setCropping(false);
}
void ScrollResultWindow::report(ImageOutputResult result)
{
    if (result.success)
        output_ = true;
    notice_->setText(result.explanation.isEmpty() ? result.success
                                                        ? QStringLiteral("输出成功。")
                                                        : QStringLiteral("输出失败，请重试。")
                                                  : result.explanation);
    notice_->show();
}
void ScrollResultWindow::save()
{
    if (saving_)
        return;
    saving_ = true;
    QPointer<ScrollResultWindow> self(this);
    const auto target = chooseImageSaveTarget(
        this, actions_, [self] { return self && !self->closing_; },
        [self](const QString& text)
        {
            if (self)
            {
                self->notice_->setText(text);
                self->notice_->show();
            }
        });
    if (!self)
        return;
    if (!target.path.isEmpty() || !target.quickDirectory.isEmpty())
    {
        const auto result = saveImageToTarget(image_, target);
        report({result.result.success, result.result.success
                                           ? QStringLiteral("已保存：%1").arg(result.path)
                                           : result.result.explanation});
    }
    saving_ = false;
}
void ScrollResultWindow::closeWithoutConfirmation()
{
    closing_ = true;
    close();
}
void ScrollResultWindow::closeEvent(QCloseEvent* event)
{
    if (saving_ && !closing_)
    {
        event->ignore();
        return;
    }
    if (!closing_ && !output_)
    {
        bool discard = false;
        if (actions_.confirmDiscard)
            discard = actions_.confirmDiscard(this);
        else
        {
            QMessageBox dialog(QMessageBox::Question, QStringLiteral("长图结果"),
                               QStringLiteral("放弃这张长图？"), QMessageBox::NoButton, nullptr);
            QPointer<ScrollResultWindow> self(this);
            auto* abandon =
                dialog.addButton(QStringLiteral("放弃并关闭"), QMessageBox::DestructiveRole);
            auto* continueViewing =
                dialog.addButton(QStringLiteral("继续查看"), QMessageBox::RejectRole);
            dialog.setDefaultButton(continueViewing);
            dialog.exec();
            if (!self)
                return;
            discard = dialog.clickedButton() == abandon;
        }
        if (!discard)
        {
            event->ignore();
            return;
        }
    }
    closing_ = true;
    event->accept();
}
void ScrollResultWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        if (view_->cropping())
            setCropping(false);
        else
            close();
    }
    else
        QWidget::keyPressEvent(event);
}
}
#include "scroll_result_window.moc"
