#pragma once
#include "interfaces/recording.h"
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
namespace waibusnap
{
class RecordingToast final : public QWidget
{
    Q_OBJECT
  public:
    explicit RecordingToast(RecordingEvent event, bool success,
                            std::function<void(const QString&)> reveal);
    void present();

  protected:
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

  private:
    QTimer timer_;
    QElapsedTimer elapsed_;
    int remainingMs_ = 8000;
};
}
