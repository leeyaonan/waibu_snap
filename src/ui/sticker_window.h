#pragma once
#include "output/image_output.h"
#include <QTimer>
#include <QWidget>
#include <functional>
class QLabel;
class QScreen;
class QPushButton;
namespace waibusnap
{
struct StickerActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<QString(QWidget*, const QString&)> chooseSavePath;
};
class StickerWindow final : public QWidget
{
    Q_OBJECT
  public:
    explicit StickerWindow(QImage image, QPoint position, bool saved = false,
                           StickerActions actions = {});
    const QImage& image() const { return image_; }
    qreal scale() const { return scale_; }
    bool isSaved() const { return saved_; }
    void setSaved(bool saved) { saved_ = saved; }
    // 4b 编辑态入口：编辑时停用移动 / 缩放；本轮没有进入编辑的 UI。
    void setEditing(bool editing);
    bool isEditing() const { return editing_; }
    void setScale(qreal scale, QPointF globalAnchor);
    void refreshScreenGeometry();
    bool exportToPath(const QString& path);
  signals:
    void closed();

  protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    void setCenteredScale(qreal scale);
    void applyGeometry(QPointF position, QSize size);
    void copyImage();
    void saveImage();
    void showStatus(const QString& text, bool temporary);
    void positionControls();
    qreal screenDpr() const;
    void moveDrag(QPointF globalPosition);
    QImage image_;
    StickerActions actions_;
    QWidget* controls_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QTimer statusTimeout_;
    QPointF dragFraction_;
    QPointF geometryPosition_;
    qreal wheelRemainder_ = 0;
    qreal scale_ = 1;
    bool saved_ = false;
    bool editing_ = false;
    bool dragging_ = false;
    bool saveDialogOpen_ = false;
    bool closed_ = false;
    bool hovered_ = false;
};
}
