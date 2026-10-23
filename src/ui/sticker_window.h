#pragma once
#include "core/annotation.h"
#include "interfaces/sticker_window_behavior.h"
#include "output/annotation_renderer.h"
#include "output/image_output.h"
#include "output/image_save.h"
#include "ui/toolbar_icons.h"
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>
class QLabel;
class QPainter;
class QScreen;
class QPushButton;
class QComboBox;
class QGridLayout;
class QHideEvent;
class QHBoxLayout;
class QMenu;
class QShortcut;
namespace waibusnap
{
class AnnotationTextEdit;
class AnnotationColorPalette;
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
struct StickerCloseConfirmation
{
    StickerCloseConfirmation(StickerCloseDecision decision = StickerCloseDecision::Cancel,
                             bool doNotRemind = false)
        : decision(decision), doNotRemind(doNotRemind)
    {
    }
    StickerCloseDecision decision;
    bool doNotRemind;
};
struct StickerActions : ImageSaveActions
{
    std::function<ImageOutputResult(const QImage&)> copyImage;
    std::function<StickerCloseConfirmation(QWidget*)> confirmClose;
    std::function<StickerQuitDecision(int)> confirmQuit;
    std::function<bool()> loadCloseReminder;
    std::function<QString(bool)> saveCloseReminder;
    // nullopt = 从未配置；true / false = 已显式选择。
    std::function<std::optional<bool>()> loadDoubleClickClose;
    std::function<QString(bool)> saveDoubleClickClose;
    // 返回 true 表示用户选择“开启并关闭”。
    std::function<bool(QWidget*)> confirmDoubleClickClose;
};
class StickerWindow final : public QWidget
{
    Q_OBJECT
  public:
    explicit StickerWindow(QImage image, QPoint position, bool saved = false,
                           StickerActions actions = {}, qreal initialScale = 1);
    const QImage& image() const { return image_; }
    qreal scale() const { return scale_; }
    bool isSaved() const { return saved_; }
    void setSaved(bool saved) { saved_ = saved; }
    // 绘图工具在画布上滚轮调线宽；其他状态沿用光标锚点缩放。
    void setEditing(bool editing);
    bool isEditing() const { return editing_; }
    void setScale(qreal scale, QPointF globalAnchor);
    void refreshScreenGeometry();
    bool exportToPath(const QString& path);
    bool saveImage();
    void forceClose();
    bool hasPendingInteraction() const
    {
        return saveDialogOpen_ || confirmingClose_ || confirmingDoubleClickClose_;
    }
    void setQuitInteraction(bool busy);
    const QVector<Annotation>& annotations() const { return history_.annotations(); }
    std::optional<AnnotationType> activeTool() const { return activeTool_; }
    bool isSelectMode() const { return selectMode_; }
    int selectedAnnotation() const { return selectedAnnotation_; }
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
    void contextMenuEvent(QContextMenuEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    enum class AnnotationGesture
    {
        None,
        Move,
        Rotate
    };
    void setCenteredScale(qreal scale);
    void updateScaleLabel();
    void applyGeometry(QPointF position, QSize size);
    void copyImage();
    void ensureEditToolbar();
    void updateEditToolbar();
    void layoutEditMainBar(int availableWidth);
    void layoutEditOptions(int availableWidth);
    void positionEditToolbar();
    void positionStatus();
    void showControls();
    void hideControls();
    void activateTool(AnnotationType type, bool toggle = true);
    void toggleSelectMode();
    bool beginAnnotationGesture(QPointF point, Qt::KeyboardModifiers modifiers);
    bool beginSelectedAnnotationGesture(QPointF point);
    void startAnnotationGesture(AnnotationGesture gesture, int index, QPointF point);
    void updateAnnotationGesture(QPointF point, Qt::KeyboardModifiers modifiers);
    void finishAnnotationGesture();
    void clearAnnotationSelection();
    int annotationAt(QPointF point) const;
    bool rotationHandleHit(QPointF point) const;
    void paintAnnotationSelection(QPainter& painter) const;
    qreal annotationLogicalFactor() const;
    void undoAnnotation();
    void redoAnnotation();
    void markDirty();
    QPointF physicalPoint(QPointF point) const;
    void updateAnnotation(QPointF point);
    void startText(QPointF point);
    void finishText(bool commit, bool restoreFocus = true);
    void positionTextEditor();
    void focusForKeyboard();
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
    void showStatus(const QString& text, bool temporary);
    bool finishSave(const ImageFileResult& output);
    void positionControls();
    qreal screenDpr() const;
    void moveDrag(QPointF globalPosition);
    QImage image_;
    StickerActions actions_;
    QWidget* controls_ = nullptr;
    QWidget* controlsBar_ = nullptr;
    QWidget* editToolbar_ = nullptr;
    QWidget* editMainBar_ = nullptr;
    QWidget* editOptionsBar_ = nullptr;
    QGridLayout* editMainLayout_ = nullptr;
    QHBoxLayout* editOptionsLayout_ = nullptr;
    QLabel* editSeparator1_ = nullptr;
    QLabel* editSeparator2_ = nullptr;
    QMenu* editMenu_ = nullptr;
    QMenu* formatMenu_ = nullptr;
    QPushButton* undoButton_ = nullptr;
    QPushButton* redoButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QPushButton* doneButton_ = nullptr;
    QShortcut* undoShortcut_ = nullptr;
    QShortcut* redoShortcut_ = nullptr;
    QVector<QPushButton*> toolButtons_;
    std::array<AnnotationType, 5> toolTypes_ = defaultAnnotationTools;
    QWidget* toolBar_ = nullptr;
    QVector<QPushButton*> subtoolButtons_;
    AnnotationColorPalette* colors_ = nullptr;
    QLabel* widthLabel_ = nullptr;
    QLabel* textSizeLabel_ = nullptr;
    QPushButton* boldButton_ = nullptr;
    QPushButton* italicButton_ = nullptr;
    QPushButton* underlineButton_ = nullptr;
    AnnotationWidthWheel widthWheel_;
    QComboBox* sizes_ = nullptr;
    QVector<QPushButton*> textSizeButtons_;
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
    std::unique_ptr<StickerFocusSession> focusSession_;
    QWidget* statusWindow_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* scaleButton_ = nullptr;
    QTimer statusTimeout_;
    QTimer controlsHideTimer_;
    QPointF dragFraction_;
    QPointF pressGlobal_;
    QPointF geometryPosition_;
    qreal wheelRemainder_ = 0;
    qreal scale_ = 1;
    bool transparent_ = false;
    bool saved_ = false;
    bool mosaicNoticeShown_ = false;
    bool editing_ = false;
    bool dragging_ = false;
    bool clickCandidate_ = false;
    bool saveDialogOpen_ = false;
    bool confirmingClose_ = false;
    bool confirmingDoubleClickClose_ = false;
    bool forceClosing_ = false;
    bool quitInteraction_ = false;
    bool closed_ = false;
    bool hovered_ = false;
    bool controlsHovered_ = false;
};
}
