#pragma once
#include "interfaces/overlay_window_behavior.h"
#include <QWidget>
namespace waibusnap
{
QString recordingDuration(qint64 milliseconds);
class RecordingBorderWindow final : public QWidget
{
    Q_OBJECT
  public:
    RecordingBorderWindow();
    void present(QRect frame, qint64 recordedMs);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QString text_;
    std::unique_ptr<OverlayWindowObservation> observation_;
};
}
