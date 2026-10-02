#pragma once
#include "core/selection_geometry.h"
#include "interfaces/capture_provider.h"
#include "output/image_output.h"
#include <QLabel>
#include <QTimer>
#include <QWidget>
#include <functional>
namespace waibusnap
{
struct OverlayActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<QString(QWidget*, const QString&)> chooseSavePath;
};
class SelectionOverlay final : public QWidget
{
    Q_OBJECT
  public:
    explicit SelectionOverlay(CaptureFrame frame, OverlayActions actions = {});
    QRect selection() const;
    bool hasPainted() const { return painted_; }
    bool isSelectionSaved() const { return saved_; }
    // 不打开对话框；路径确认由保存入口负责，供导出测试和后续输出入口复用。
    bool exportToPath(const QString& path);
  signals:
    void firstPaintCompleted(qint64 timestamp);
    void finished(QRect pixels, int outcome);

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
    void complete(int outcome);
    void copySelection();
    void saveSelection();
    void setSelection(QRect pixels);
    void updateToolbar();
    void showStatus(const QString& text, bool temporary);
    QRectF logicalSelection() const;
    SelectionEdges edgesAt(QPointF position) const;
    void updateCursor(QPointF position);
    void dragTo(QPointF position);
    CaptureFrame frame_;
    OverlayActions actions_;
    QWidget* toolbar_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer statusTimeout_;
    QRect selection_;
    QRect initialSelection_;
    QPointF press_;
    DragMode dragMode_ = DragMode::None;
    SelectionEdges resizeEdges_;
    bool painted_ = false;
    bool finished_ = false;
    bool saved_ = false;
    bool saveDialogOpen_ = false;
};
}
