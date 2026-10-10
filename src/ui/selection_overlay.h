#pragma once
#include "core/annotation.h"
#include "core/selection_geometry.h"
#include "core/text_layout.h"
#include "interfaces/capture_provider.h"
#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include "output/image_save.h"
#include <QLabel>
#include <QPointer>
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
class OcrDialog;
struct OverlayActions : ImageSaveActions
{
    std::shared_ptr<const TextRecognizer> recognizer;
    std::function<void(const QString&)> setClipboardText;
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
    bool isTextMode() const { return textMode_; }
    bool isRecognizingText() const { return recognizingText_; }
    QString selectedOcrText() const;
    QVector<QRectF> ocrHighlightBoxes() const;
    QRect hoveredWindowPixels() const { return hoveredWindowPixels_; }
    bool hasPainted() const { return painted_; }
    bool isSelectionSaved() const { return saved_; }
    QString selectionSizeText() const;
    bool magnifierVisible() const { return magnifierVisible_; }
    QPoint magnifierAnchor() const { return magnifierAnchor_; }
    QRect magnifierSampleRect() const { return magnifierSampleRect_; }
    QImage magnifierSample() const { return magnifierSample_; }
    QString magnifierPositionText() const;
    QRect magnifierRect() const;
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
    void mouseDoubleClickEvent(QMouseEvent*) override;
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
    void copyShortcut();
    void toggleTextMode();
    void exitTextMode();
    void startOcrRecognition();
    void openOcrDialog();
    text_layout::TokenPosition textAt(QPointF position, bool nearest) const;
    void selectOcrTo(QPointF position);
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
    bool nudgeSelection(QKeyEvent* event);
    void showMagnifier(QPoint anchor, bool keyboard);
    void showMouseMagnifier(QPointF position);
    void hideMagnifier();
    void paintMagnifier(QPainter& painter);
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
    QTimer magnifierTimeout_;
    QPoint magnifierAnchor_;
    QRect magnifierSampleRect_;
    QImage magnifierSample_;
    bool magnifierVisible_ = false;
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
    QPushButton* ocrButton_ = nullptr;
    QPushButton* extractAllTextButton_ = nullptr;
    QShortcut* copyShortcut_ = nullptr;
    bool textMode_ = false;
    bool recognizingText_ = false;
    bool selectingText_ = false;
    quint64 recognitionToken_ = 0;
    QTimer recognitionProgress_;
    TextRecognitionResult ocrResult_;
    text_layout::TokenPosition ocrAnchor_;
    QVector<text_layout::SelectionRange> ocrSelection_;
    QPointer<OcrDialog> ocrDialog_;
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
