#pragma once
#include <QLabel>
#include <QTimer>
namespace waibusnap
{
class ClipboardStickerToast final : public QLabel
{
  public:
    ClipboardStickerToast();
    void showMessage(const QString& text, QPoint cursor, QRect available);

  protected:
    void hideEvent(QHideEvent* event) override;

  private:
    QTimer timer_;
};
}
