#pragma once
#include "interfaces/capture_provider.h"
#include <QWidget>
namespace waibusnap
{
class SelectionOverlay final : public QWidget
{
    Q_OBJECT
  public:
    explicit SelectionOverlay(CaptureFrame frame);
    QRect selection() const;
    bool hasPainted() const { return painted_; }
  signals:
    void firstPaintCompleted(qint64 timestamp);
    void finished(QRect pixels, bool confirmed);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    void complete(bool confirmed);
    CaptureFrame frame_;
    QPointF anchor_;
    QPointF cursor_;
    bool dragging_ = false;
    bool painted_ = false;
    bool finished_ = false;
};
}
