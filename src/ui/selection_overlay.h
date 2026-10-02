#pragma once
#include "core/selection_geometry.h"
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
    enum class DragMode
    {
        None,
        Create,
        Move,
        Resize
    };
    void complete(bool confirmed);
    QRectF logicalSelection() const;
    SelectionEdges edgesAt(QPointF position) const;
    void updateCursor(QPointF position);
    void dragTo(QPointF position);
    CaptureFrame frame_;
    QRect selection_;
    QRect initialSelection_;
    QPointF press_;
    DragMode dragMode_ = DragMode::None;
    SelectionEdges resizeEdges_;
    bool painted_ = false;
    bool finished_ = false;
};
}
