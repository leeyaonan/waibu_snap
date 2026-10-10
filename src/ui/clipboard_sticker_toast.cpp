#include "ui/clipboard_sticker_toast.h"
#include <QHideEvent>
#include <algorithm>
namespace waibusnap
{
ClipboardStickerToast::ClipboardStickerToast()
    : QLabel(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("clipboardStickerToast"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::NoFocus);
    setTextFormat(Qt::PlainText);
    setWordWrap(true);
    setMargin(12);
    setStyleSheet(
        QStringLiteral("QLabel { background: #202020; color: white; border-radius: 6px; }"));
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &QWidget::close);
}
void ClipboardStickerToast::showMessage(const QString& text, QPoint cursor, QRect available)
{
    setText(text);
    const int width = available.isEmpty() ? 460 : std::min(460, available.width());
    resize(width, heightForWidth(width));
    QPoint position = cursor + QPoint(16, 16);
    if (!available.isEmpty())
        position = {std::clamp(position.x(), available.left(),
                               available.left() + std::max(0, available.width() - this->width())),
                    std::clamp(position.y(), available.top(),
                               available.top() + std::max(0, available.height() - height()))};
    move(position);
    show();
    timer_.start(2500);
}
void ClipboardStickerToast::hideEvent(QHideEvent* event)
{
    timer_.stop();
    QLabel::hideEvent(event);
}
}
