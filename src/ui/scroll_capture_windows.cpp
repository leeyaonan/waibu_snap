#include "ui/scroll_capture_windows.h"
#include "interfaces/overlay_window_behavior.h"
#include "interfaces/scroll_injector.h"
#include "interfaces/sticker_window_behavior.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <algorithm>
namespace waibusnap
{
QRect placeScrollBar(QRect region, QSize size, QRect available)
{
    size.setWidth(std::min(size.width(), available.width()));
    const int x = std::clamp(region.right() + 1 - size.width(), available.left(),
                             std::max(available.left(), available.right() + 1 - size.width()));
    int y = region.bottom() + 9;
    if (y + size.height() > available.bottom() + 1)
        y = region.top() - size.height() - 8;
    if (y < available.top())
        y = std::min(region.bottom() + 1, available.bottom() + 1) - size.height() - 8;
    y = std::clamp(y, available.top(),
                   std::max(available.top(), available.bottom() + 1 - size.height()));
    return {QPoint(x, y), size};
}
ScrollBorderWindow::ScrollBorderWindow(QRect region)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("scrollBorderWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(region.adjusted(-2, -2, 2, 2));
    winId();
    configureStickerWindowBehavior(windowHandle());
    // 先抬升再重设位置，避免 Cocoa 将菜单栏附近的外框约束到可用区内。
    raiseOverlayAboveSystemChrome(windowHandle());
    setGeometry(region.adjusted(-2, -2, 2, 2));
}
void ScrollBorderWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setPen(QPen(QColor(QStringLiteral("#f0a030")), 2));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));
}
ScrollControlBar::ScrollControlBar(QRect region, QRect available)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      region_(region), available_(available)
{
    setObjectName(QStringLiteral("scrollControlBar"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("QWidget#scrollControlBar { background: #202020; border: 1px "
                                 "solid #3a3a3a; border-radius: 8px; }"
                                 "QLabel { color: #f2f2f2; border: none; font-size: 12px; }"
                                 "QPushButton { color: #f2f2f2; background: transparent; border: "
                                 "none; border-radius: 6px; padding: 0 6px; }"
                                 "QPushButton:hover { background: #3a3a3a; }"
                                 "QPushButton:checked { background: #a65316; color: white; }"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("scrollStatusLabel"));
    status_->setTextFormat(Qt::PlainText);
    status_->setMinimumWidth(0);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(status_, 1);
    const auto button = [&](const QString& name, const QString& text)
    {
        auto* result = new QPushButton(text, this);
        result->setObjectName(name);
        result->setFixedHeight(28);
        result->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(result);
        return result;
    };
    autoButton_ = button(QStringLiteral("scrollAutoToggle"), QStringLiteral("自动滚动"));
    autoButton_->setCheckable(true);
    speedButton_ = button(QStringLiteral("scrollSpeedButton"), QStringLiteral("速度：中"));
    directionButton_ =
        button(QStringLiteral("scrollDirectionButton"), QStringLiteral("方向：自动"));
    auto* finish = button(QStringLiteral("scrollFinishButton"), QStringLiteral("完成"));
    auto* cancel = button(QStringLiteral("scrollCancelButton"), QStringLiteral("取消"));
    connect(autoButton_, &QPushButton::clicked, this, &ScrollControlBar::automaticRequested);
    connect(speedButton_, &QPushButton::clicked, this, &ScrollControlBar::speedRequested);
    connect(directionButton_, &QPushButton::clicked, this, &ScrollControlBar::directionRequested);
    connect(finish, &QPushButton::clicked, this, &ScrollControlBar::finishRequested);
    connect(cancel, &QPushButton::clicked, this, &ScrollControlBar::cancelRequested);
    noticeTimeout_.setSingleShot(true);
    connect(&noticeTimeout_, &QTimer::timeout, this, [this] { setNotice({}); });
    statistics_ = QStringLiteral("已拼接 0 px · 0 帧");
    setAutomatic(false);
    winId();
    configureStickerWindowBehavior(windowHandle());
}
void ScrollControlBar::place()
{
    const QString speeds[] = {QStringLiteral("慢"), QStringLiteral("中"), QStringLiteral("快")};
    const QString directions[] = {QStringLiteral("自动"), QStringLiteral("竖向"),
                                  QStringLiteral("横向")};
    const bool compact = available_.width() < 480;
    autoButton_->setText(compact ? QStringLiteral("自动") : QStringLiteral("自动滚动"));
    autoButton_->setToolTip(QStringLiteral("自动滚动"));
    speedButton_->setToolTip(QStringLiteral("速度：%1").arg(speeds[speed_]));
    directionButton_->setToolTip(QStringLiteral("方向：%1").arg(directions[direction_]));
    speedButton_->setText(compact ? speeds[speed_] : speedButton_->toolTip());
    directionButton_->setText(compact ? directions[direction_] : directionButton_->toolTip());
    for (auto* control : findChildren<QPushButton*>())
        control->setFixedWidth(control->fontMetrics().horizontalAdvance(control->text()) + 16);
    // 状态文字用省略显示，完整提示保留 tooltip，窄可用区不推走按钮。
    const int desired = autoButton_->isChecked() ? 680 : 480;
    setGeometry(placeScrollBar(region_, QSize(desired, 40), available_));
    layout()->activate();
    refreshStatus();
    emit placementChanged(geometry());
}
void ScrollControlBar::refreshStatus()
{
    const QString text = notice_.isEmpty() ? statistics_ : notice_;
    status_->setToolTip(text);
    status_->setText(status_->fontMetrics().elidedText(text, Qt::ElideRight, status_->width()));
}
void ScrollControlBar::setStatistics(ScrollStitchStats stats, qint64 limit)
{
    statistics_ =
        stats.lengthPx >= limit * 9 / 10
            ? QStringLiteral("已拼接 %1 px（接近上限）").arg(stats.lengthPx)
            : QStringLiteral("已拼接 %1 px · %2 帧").arg(stats.lengthPx).arg(stats.frames);
    refreshStatus();
}
void ScrollControlBar::setNotice(const QString& text)
{
    if (notice_ == text)
        return;
    notice_ = text;
    noticeTimeout_.stop();
    if (text == QStringLiteral("画面重叠不足或内容变化，部分帧未拼接。") ||
        text.startsWith(QStringLiteral("检测到另一方向")))
        noticeTimeout_.start(2500);
    refreshStatus();
}
void ScrollControlBar::setAutomatic(bool enabled)
{
    const QSignalBlocker blocker(autoButton_);
    autoButton_->setChecked(enabled);
    speedButton_->setVisible(enabled);
    directionButton_->setVisible(enabled);
    place();
}
void ScrollControlBar::setOptions(int speed, int direction)
{
    speed_ = std::clamp(speed, 0, 2);
    direction_ = std::clamp(direction, 0, 2);
    place();
}

bool ScrollControlBar::explainAccessibilityPermission()
{
    QMessageBox dialog(QMessageBox::Information, QStringLiteral("需要“辅助功能”权限"),
                       ScrollInjector::permissionExplanation(), QMessageBox::NoButton, nullptr);
    auto* open = dialog.addButton(QStringLiteral("打开系统设置"), QMessageBox::AcceptRole);
    auto* cancel = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    dialog.setDefaultButton(cancel);
    dialog.exec();
    return dialog.clickedButton() == open;
}
void ScrollControlBar::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
        emit cancelRequested();
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        emit finishRequested();
    else
        QWidget::keyPressEvent(event);
}
}
