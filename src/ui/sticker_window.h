#pragma once
#include "core/annotation.h"
#include "interfaces/sticker_window_behavior.h"
#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>
class QLabel;
class QScreen;
class QPushButton;
class QComboBox;
class QMenu;
class QShortcut;
namespace waibusnap
{
class AnnotationTextEdit;
enum class StickerCloseDecision
{
    Cancel,
    Save,
    Discard
};
enum class StickerQuitDecision
{
    Cancel,
    SaveAll,
    DiscardAll
};
struct StickerActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<QString(QWidget*, const QString&)> chooseSavePath;
    std::function<StickerCloseDecision(QWidget*)> confirmClose;
    std::function<StickerQuitDecision(int)> confirmQuit;
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
    // 编辑只禁用窗口移动；滚轮仍以光标为锚点缩放。
    void setEditing(bool editing);
    bool isEditing() const { return editing_; }
    void setScale(qreal scale, QPointF globalAnchor);
    void refreshScreenGeometry();
    bool exportToPath(const QString& path);
    bool saveImage();
    void forceClose();
    bool hasPendingInteraction() const { return saveDialogOpen_ || confirmingClose_; }
    void setQuitInteraction(bool busy);
    const QVector<Annotation>& annotations() const { return history_.annotations(); }
    std::optional<AnnotationType> activeTool() const { return activeTool_; }
    QImage renderedImage() const;
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
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    void setCenteredScale(qreal scale);
    void applyGeometry(QPointF position, QSize size);
    void copyImage();
    void ensureEditToolbar();
    void updateEditToolbar();
    void activateTool(AnnotationType type);
    void undoAnnotation();
    void redoAnnotation();
    void markDirty();
    QPointF physicalPoint(QPointF point) const;
    void updateAnnotation(QPointF point);
    void startText(QPointF point);
    void finishText(bool commit, bool restoreFocus = true);
    void positionTextEditor();
    void showStatus(const QString& text, bool temporary);
    void positionControls();
    qreal screenDpr() const;
    void moveDrag(QPointF globalPosition);
    QImage image_;
    StickerActions actions_;
    QWidget* controls_ = nullptr;
    QWidget* editToolbar_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QPushButton* editMoreButton_ = nullptr;
    QPushButton* undoButton_ = nullptr;
    QPushButton* redoButton_ = nullptr;
    QShortcut* undoShortcut_ = nullptr;
    QShortcut* redoShortcut_ = nullptr;
    QVector<QPushButton*> toolButtons_;
    QComboBox* colors_ = nullptr;
    QComboBox* widths_ = nullptr;
    QComboBox* sizes_ = nullptr;
    AnnotationHistory history_;
    AnnotationRenderCache renderCache_;
    AnnotationStyle style_;
    std::optional<AnnotationType> activeTool_;
    std::optional<Annotation> draft_;
    AnnotationTextEdit* textEditor_ = nullptr;
    Annotation textDraft_;
    std::unique_ptr<StickerFocusSession> focusSession_;
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
    bool confirmingClose_ = false;
    bool forceClosing_ = false;
    bool quitInteraction_ = false;
    bool closed_ = false;
    bool hovered_ = false;
};
}
