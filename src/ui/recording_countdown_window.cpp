#include "ui/recording_countdown_window.h"
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>
namespace waibusnap
{
RecordingCountdownWindow::RecordingCountdownWindow()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setObjectName(QStringLiteral("recordingCountdown"));
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("background: #252830; color: white; border-radius: 16px;"));
    auto* layout = new QVBoxLayout(this);
    number_ = new QLabel(this);
    number_->setAlignment(Qt::AlignCenter);
    number_->setStyleSheet(QStringLiteral("font-size: 64px;"));
    layout->addWidget(number_);
    layout->addWidget(new QLabel(QStringLiteral("即将开始录制 · Esc 取消"), this));
    resize(240, 180);
}
void RecordingCountdownWindow::present(QRect target, int seconds)
{
    if (seconds <= 0)
    {
        hide();
        return;
    }
    setSeconds(seconds);
    move(target.center() - rect().center());
    winId();
    raiseOverlayAboveSystemChrome(windowHandle());
    move(target.center() - rect().center());
    observation_ = observeOverlayWindow(windowHandle(), this,
                                        [this](const QString&)
                                        {
                                            if (isVisible())
                                                reassertOverlayWindow(windowHandle(), true);
                                        });
    show();
    activateWindow();
    setFocus();
}
void RecordingCountdownWindow::setSeconds(int seconds)
{
    number_->setText(QString::number(seconds));
}
void RecordingCountdownWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
        close();
    else
        QWidget::keyPressEvent(event);
}
void RecordingCountdownWindow::closeEvent(QCloseEvent* event)
{
    hide();
    event->accept();
    emit cancelled();
}
}
