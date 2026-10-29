#pragma once
#include "interfaces/overlay_window_behavior.h"
#include <QWidget>
class QLabel;
namespace waibusnap
{
class RecordingCountdownWindow final : public QWidget
{
    Q_OBJECT
  public:
    explicit RecordingCountdownWindow();
    void present(QRect target, int seconds);
    void setSeconds(int seconds);
  signals:
    void cancelled();

  protected:
    void keyPressEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    QLabel* number_ = nullptr;
    std::unique_ptr<OverlayWindowObservation> observation_;
};
}
