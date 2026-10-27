#pragma once
#include "interfaces/capture_provider.h"
#include <QWidget>
class QLabel;
namespace waibusnap
{
class QuickGunnerRegionPicker : public QWidget
{
    Q_OBJECT
  public:
    explicit QuickGunnerRegionPicker(CaptureFrame frame, QWidget* parent = nullptr);
    QRect selection() const { return selection_; }
  signals:
    void regionSelected(QRect pixelRect);
    void cancelled();

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    void dragTo(QPointF point);
    void updateHint(bool tooSmall = false);
    CaptureFrame frame_;
    QRect selection_;
    QPointF press_;
    QLabel* hint_ = nullptr;
    bool dragging_ = false;
    bool finished_ = false;
};
}
