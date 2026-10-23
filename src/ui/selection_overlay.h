#pragma once
#include "app/history_store.h"
#include "core/annotation.h"
#include "core/aspect_ratio.h"
#include "core/selection_geometry.h"
#include "core/shape_mask.h"
#include "core/text_layout.h"
#include "core/watermark.h"
#include "interfaces/capture_provider.h"
#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include "output/image_save.h"
#include "ui/toolbar_icons.h"
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <optional>
class QComboBox;
class QPainter;
class QPushButton;
class QSpinBox;
class QShortcut;
namespace waibusnap
{
class AnnotationTextEdit;
class AnnotationColorPalette;
class OcrDialog;
class WatermarkService;
struct OverlayActions : ImageSaveActions
{
    QString historySessionId;
    QString historyGroupId;
    std::function<HistoryStatus(const QImage&, const QString& savedPath)> recordHistory;
    CaptureRatioSettings ratios{defaultCaptureRatios(), {}, AspectRatio{1, 1}};
    QVector<ShapeTemplateEntry> shapeTemplates;
    QString shapeTemplateDir;
    QString shapeTemplateNotice;
    std::shared_ptr<const TextRecognizer> recognizer;
    std::function<void(const QString&)> setClipboardText;
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<ImageOutputResult(const QImage&)> pinImage;
    WatermarkService* watermarks = nullptr;
};
class SelectionOverlay final : public QWidget
{
    Q_OBJECT
  public:
    explicit SelectionOverlay(CaptureFrame frame, OverlayActions actions = {},
                              QVector<QRect> windows = {}, QString snappingError = {});
    QRect selection() const;
    bool isScrollReady() const { return scrollReady_; }
    void enterScrollReady(QRect preset = {});
    bool isTextMode() const { return textMode_; }
    bool isRecognizingText() const { return recognizingText_; }
    QString selectedOcrText() const;
    QVector<QRectF> ocrHighlightBoxes() const;
    QRect hoveredWindowPixels() const { return hoveredWindowPixels_; }
    bool systemPanelOpen() const { return saveDialogOpen_; }
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
    bool isSelectMode() const { return selectMode_; }
    int selectedAnnotation() const { return selectedAnnotation_; }
    bool hasWatermark() const { return watermark_.has_value(); }
    bool isWatermarkToolActive() const { return watermarkToolActive_; }
    // 不打开对话框；路径确认由保存入口负责，供导出测试和后续输出入口复用。
    bool exportToPath(const QString& path);
  signals:
    void scrollReadyChanged(bool ready);
    void scrollCaptureRequested(QRect pixels);
    void firstPaintCompleted(qint64 timestamp);
    void finished(QRect pixels, int outcome);

  protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void leaveEvent(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    friend struct OverlayScrollTestAccess;
    void updateScrollReadyBar();
    void startScrollCapture();
    bool scrollSupported_ = false;
    bool scrollReady_ = false;
    QWidget* scrollReadyBar_ = nullptr;
    QPushButton* longImageButton_ = nullptr;
    enum class DragMode
    {
        None,
        Create,
        Move,
        Resize,
        Annotate
    };
    enum class AnnotationGesture
    {
        None,
        Move,
        Rotate
    };
    void complete(int outcome);
    void copySelection();
    void copyShortcut();
    void toggleTextMode();
    void toggleWatermarkTool();
    void applyWatermarkPreset();
    void removeWatermark();
    void resetWatermark();
    void updateWatermarkBar();
    void paintWatermarkOverlay(QPainter& painter) const;
    void paintWatermarkSelection(QPainter& painter) const;
    bool watermarkHit(QPointF position) const;
    bool watermarkRotationHandleHit(QPointF position) const;
    bool watermarkScaleHandleHit(QPointF position) const;
    QPointF watermarkOutputPoint(QPointF position) const;
    QRectF watermarkLogicalBounds() const;
    QPointF watermarkRotationHandleLogical() const;
    QPointF watermarkScaleHandleLogical() const;
    void beginWatermarkGesture(int gesture, QPointF position);
    void updateWatermarkGesture(QPointF position, Qt::KeyboardModifiers modifiers);
    void finishWatermarkGesture();
    void syncWatermarkBar();
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
    void layoutToolbar(int availableWidth);
    void updateOptionsBar();
    void updateRatioBar();
    void updateShapeBar();
    void chooseShape(int index);
    QString shapeLabel() const;
    QImage outputImage(bool includeWatermark = true) const;
    void chooseRatio(std::optional<AspectRatio> ratio);
    std::optional<AspectRatio> dragRatio() const;
    QRect toolbarClusterRect() const;
    QRect availableRect() const;
    void positionStatus();
    void markDirty();
    void activateTool(AnnotationType type, bool toggle = true);
    void toggleSelectMode();
    bool beginAnnotationGesture(QPointF position, Qt::KeyboardModifiers modifiers);
    bool beginSelectedAnnotationGesture(QPointF position);
    void startAnnotationGesture(AnnotationGesture gesture, int index, QPointF position);
    void updateAnnotationGesture(QPointF position, Qt::KeyboardModifiers modifiers);
    void finishAnnotationGesture();
    void clearAnnotationSelection();
    int annotationAt(QPointF position) const;
    bool rotationHandleHit(QPointF position) const;
    void paintAnnotationSelection(QPainter& painter) const;
    QPointF annotationLogicalPoint(QPointF point) const;
    void updateAnnotation(QPointF position);
    void undoAnnotation();
    void redoAnnotation();
    void startText(QPointF position);
    void finishText(bool commit, bool restoreFocus = true);
    enum class TextStyleToggle
    {
        Bold,
        Italic,
        Underline
    };
    void toggleTextStyle(TextStyleToggle toggle);
    void setTextSize(int size);
    void applyTextEditorFont();
    void adjustTextSize(QPoint angleDelta, QPoint pixelDelta, Qt::ScrollPhase phase);
    void discardAnnotations();
    bool nudgeSelection(QKeyEvent* event);
    void showMagnifier(QPoint anchor, bool keyboard);
    void showMouseMagnifier(QPointF position);
    void hideMagnifier();
    void paintMagnifier(QPainter& painter);
    QPointF physicalPoint(QPointF position) const;
    void showStatus(const QString& text, bool temporary);
    bool finishSave(const ImageFileResult& output, const QImage& image);
    void recordHistory(const QImage& image, const QString& savedPath = {});
    QRectF logicalSelection() const;
    SelectionEdges edgesAt(QPointF position) const;
    void updateCursor(QPointF position);
    void dragTo(QPointF position);
    void updateHover(QPointF position);
    CaptureFrame frame_;
    OverlayActions actions_;
    QWidget* toolbar_ = nullptr;
    QWidget* optionsBar_ = nullptr;
    QWidget* ratioBar_ = nullptr;
    QWidget* shapeBar_ = nullptr;
    QWidget* watermarkBar_ = nullptr;
    QPushButton* shapeButton_ = nullptr;
    QPushButton* shapeWhiteBackgroundButton_ = nullptr;
    QVector<QPushButton*> shapeButtons_;
    QVector<QImage> templateMasks_;
    QImage templatePreview_;
    QSize templatePreviewSize_;
    int activeShape_ = -1;
    QPushButton* ratioButton_ = nullptr;
    QVector<QPushButton*> ratioButtons_;
    std::optional<AspectRatio> activeRatio_;
    bool shiftRatio_ = false;
    QPointF lastDragPosition_;
    AnnotationColorPalette* colors_ = nullptr;
    QLabel* widthLabel_ = nullptr;
    QLabel* textSizeLabel_ = nullptr;
    QPushButton* boldButton_ = nullptr;
    QPushButton* italicButton_ = nullptr;
    QPushButton* underlineButton_ = nullptr;
    AnnotationWidthWheel widthWheel_;
    QVector<QPushButton*> textSizeButtons_;
    QVector<QPushButton*> actionButtons_;
    QVector<QLabel*> separators_;
    QLabel* status_ = nullptr;
    bool overlayLevelRaised_ = false;
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
    QPushButton* selectButton_ = nullptr;
    bool selectMode_ = false;
    int selectedAnnotation_ = -1;
    AnnotationGesture annotationGesture_ = AnnotationGesture::None;
    bool pendingSelectedGesture_ = false;
    QPointF pendingSelectedPress_;
    QPointF annotationPressPhysical_;
    QPointF annotationStartCenter_;
    qreal annotationStartAngle_ = 0;
    Annotation annotationBeforeEdit_;
    // 刚提交的线宽类标注仍可作为滚轮目标；切换工具 / 新起笔画 / 撤销后清除。
    bool lastAnnotationSelected_ = false;
    // Mouse FocusOut 延迟一拍提交，让文本格式 / 字号按钮有机会取消提交。
    bool textFocusCommitPending_ = false;
    AnnotationTextEdit* textEditor_ = nullptr;
    Annotation textDraft_;
    QVector<QPushButton*> toolButtons_;
    std::array<AnnotationType, 5> toolTypes_ = defaultAnnotationTools;
    QWidget* toolBar_ = nullptr;
    QVector<QPushButton*> subtoolButtons_;
    QPushButton* undoButton_ = nullptr;
    QPushButton* redoButton_ = nullptr;
    QPushButton* ocrButton_ = nullptr;
    QPushButton* extractAllTextButton_ = nullptr;
    QPushButton* watermarkToolButton_ = nullptr;
    QComboBox* watermarkPresetCombo_ = nullptr;
    QSpinBox* watermarkOpacitySpin_ = nullptr;
    QSpinBox* watermarkScaleSpin_ = nullptr;
    QPushButton* watermarkVisibilityButton_ = nullptr;
    QPushButton* watermarkResetButton_ = nullptr;
    QPushButton* watermarkRemoveButton_ = nullptr;
    std::optional<WatermarkInstance> watermark_;
    bool watermarkToolActive_ = false;
    bool watermarkSelected_ = false;
    enum class WatermarkGesture
    {
        None,
        Move,
        Rotate,
        Scale
    };
    WatermarkGesture watermarkGesture_ = WatermarkGesture::None;
    QPointF watermarkPressOutput_;
    WatermarkInstance watermarkBeforeGesture_;
    QPointF watermarkGestureCenter_;
    qreal watermarkGestureStartDistance_ = 0;
    qreal watermarkGestureStartAngle_ = 0;
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
