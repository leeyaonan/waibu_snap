#include "ui/recording_toast.h"
#include "ui/recording_border_window.h"
#include <QCursor>
#include <QGuiApplication>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
namespace waibusnap
{
RecordingToast::RecordingToast(RecordingEvent event, bool success,
                               std::function<void(const QString&)> reveal)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("recordingToast"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QStringLiteral(
        "QWidget { background: #282b31; color: white; } QPushButton { padding: 6px; }"));
    auto* layout = new QVBoxLayout(this);
    const QString title = !success            ? QStringLiteral("录制失败")
                          : event.autoStopped ? QStringLiteral("录制已自动停止")
                                              : QStringLiteral("录制已保存");
    layout->addWidget(new QLabel(title, this));
    QString details = success ? QStringLiteral("%1 · %2 · %3×%4")
                                    .arg(recordingDuration(event.recordedMs),
                                         QLocale().formattedDataSize(event.bytes))
                                    .arg(event.pixelSize.width())
                                    .arg(event.pixelSize.height())
                              : QString();
    if (event.droppedFrames)
        details += QStringLiteral(" · 因性能丢弃 %1 帧").arg(event.droppedFrames);
    if (!event.message.isEmpty())
        details += QStringLiteral("\n") + event.message;
    auto* summary = new QLabel(details, this);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* path = new QLabel(this);
    path->setText(path->fontMetrics().elidedText(event.path, Qt::ElideMiddle, 380));
    path->setToolTip(event.path);
    layout->addWidget(path);
    auto* folder = new QPushButton(QStringLiteral("打开文件夹"), this);
    folder->setObjectName(QStringLiteral("recordingToastOpenFolder"));
    folder->setEnabled(!event.path.isEmpty());
    layout->addWidget(folder);
    connect(folder, &QPushButton::clicked, this,
            [reveal = std::move(reveal), path = event.path]
            {
                if (reveal)
                    reveal(path);
            });
    auto* closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setObjectName(QStringLiteral("recordingToastClose"));
    layout->addWidget(closeButton);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &QWidget::close);
    setFixedWidth(412);
    adjustSize();
}
void RecordingToast::present()
{
    auto* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen)
        move(screen->availableGeometry().bottomRight() - QPoint(width() + 15, height() + 15));
    show();
    elapsed_.start();
    timer_.start(remainingMs_);
}
void RecordingToast::enterEvent(QEnterEvent*)
{
    if (timer_.isActive())
    {
        remainingMs_ = qMax(1, remainingMs_ - int(elapsed_.elapsed()));
        timer_.stop();
    }
}
void RecordingToast::leaveEvent(QEvent*)
{
    elapsed_.restart();
    timer_.start(remainingMs_);
}
}
