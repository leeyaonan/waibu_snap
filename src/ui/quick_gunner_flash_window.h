#pragma once
#include <QTimer>
#include <QWidget>
namespace waibusnap
{
class QuickGunnerFlashWindow final : public QWidget
{
  public:
    QuickGunnerFlashWindow();
    void flash(QRect logicalRegion);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QTimer timer_;
};
}
