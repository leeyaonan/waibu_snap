#pragma once
#include "core/annotation.h"
#include "core/selection_geometry.h"
#include "interfaces/capture_provider.h"
#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include "output/image_save.h"
#include <QLabel>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <optional>
class QPushButton;
class QShortcut;
namespace waibusnap
{
class AnnotationTextEdit;
struct OverlayActions : ImageSaveActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<ImageOutputResult(const QImage&)> pinImage;
};
class SelectionOverlay final : public QWidget
{
    Q_OBJECT
  public:
    explicit SelectionOverlay(CaptureFrame frame, OverlayActions actions = {},
                              QVector<QRect> windows = {}, QString snappingError = {});
    QRect selection() const;
    QRect hoveredWindowPixels() const { return hoveredWindowPixels_; }
    bool hasPainted() const { return painted_; }
    bool isSelectionSaved() const { return saved_; }
    QPoint selectionGlobalPosition() const;
    const QVector<Annotation>& annotations() const { return history_.annotations(); }
    std::optional<AnnotationType> activeTool() const { return activeTool_; }
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
    void leaveEvent(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    enum class DragMode
    {
        None,
        Create,
        Move,
        Resize,
        Annotate
    };
    void complete(int outcome);
    void copySelection();
    void pinSelection();
    void saveSelection();
    void setSelection(QRect pixels);
    void ensureToolbar();
    void updateToolbar();
    void markDirty();
    void activateTool(AnnotationType type);
    void updateAnnotation(QPointF position);
    void undoAnnotation();
    void redoAnnotation();
    void startText(QPointF position);
    void finishText(bool commit, bool restoreFocus = true);
    void discardAnnotations();
    QPointF physicalPoint(QPointF position) const;
    void showStatus(const QString& text, bool temporary);
    bool finishSave(const ImageFileResult& output);
    QRectF logicalSelection() const;
    SelectionEdges edgesAt(QPointF position) const;
    void updateCursor(QPointF position);
    void dragTo(QPointF position);
    void updateHover(QPointF position);
    CaptureFrame frame_;
    OverlayActions actions_;
    QWidget* toolbar_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer statusTimeout_;
    AnnotationHistory history_;
    AnnotationRenderCache renderCache_;
    AnnotationStyle style_;
    std::optional<AnnotationType> activeTool_;
    std::optional<Annotation> draft_;
    AnnotationTextEdit* textEditor_ = nullptr;
    Annotation textDraft_;
    QVector<QPushButton*> toolButtons_;
    QPushButton* undoButton_ = nullptr;
    QPushButton* redoButton_ = nullptr;
    QShortcut* undoShortcut_ = nullptr;
    QShortcut* redoShortcut_ = nullptr;
    QRect selection_;
    QRect initialSelection_;
    QVector<QRect> windows_;
    QString snappingNotice_;
    QRect hoveredWindowPixels_;
    int pressedWindow_ = -1;
    qreal maximumPressDistance_ = 0;
    QPointF press_;
    DragMode dragMode_ = DragMode::None;
    SelectionEdges resizeEdges_;
    bool painted_ = false;
    bool finished_ = false;
    bool saved_ = false;
    bool mosaicNoticeShown_ = false;
    bool saveDialogOpen_ = false;
};
}
