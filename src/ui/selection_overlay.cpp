#include "ui/selection_overlay.h"
#include "app/watermark_service.h"
#include "core/selection_assistance.h"
#include "core/window_snapping.h"
#include "interfaces/overlay_window_behavior.h"
#include "interfaces/region_capture.h"
#include "output/annotation_renderer.h"
#include "output/watermark_renderer.h"
#include "session/monotonic_clock.h"
#include "session/session_metrics.h"
#include "ui/annotation_color_palette.h"
#include "ui/annotation_text_edit.h"
#include "ui/annotation_toolbar_style.h"
#include "ui/ocr_dialog.h"
#include "ui/scroll_capture_windows.h"
#include "ui/text_recognition_task.h"
#include "ui/toolbar_icons.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QSpinBox>
#include <QUuid>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
namespace waibusnap
{
namespace
{
constexpr qreal handleRadius = 6;
constexpr qreal windowClickDistance = 3;
}
SelectionOverlay::SelectionOverlay(CaptureFrame frame, OverlayActions actions,
                                   QVector<QRect> windows, QString snappingError)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      frame_(std::move(frame)), actions_(std::move(actions)), windows_(std::move(windows))
{
    if (actions_.historySessionId.isEmpty())
        actions_.historySessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    scrollSupported_ = regionCaptureSupported();
    if (!snappingError.isEmpty() || windows_.isEmpty())
    {
        windows_.clear();
        snappingNotice_ = QStringLiteral("窗口吸附不可用，请手动框选");
    }
    QVector<ShapeTemplateEntry> usable;
    for (const auto& entry : actions_.shapeTemplates)
    {
        if (!entry.enabled)
            continue;
        QImage mask(QDir(actions_.shapeTemplateDir).filePath(entry.file));
        if (mask.isNull())
            actions_.shapeTemplateNotice = QStringLiteral("部分形状模板缺失或损坏，已忽略。");
        else
        {
            mask.setDevicePixelRatio(1);
            usable.append(entry);
            templateMasks_.append(mask);
        }
    }
    actions_.shapeTemplates = std::move(usable);
    // Qt::Tool 的 Cocoa NSPanel 默认可能随应用失活隐去，但 Qt isVisible 仍为真。
    // 截图冻结层由会话显式 hide/finish 管理，不随其他应用获得焦点而隐去。
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setGeometry(frame_.display.logicalGeometry);
    setWindowTitle(QStringLiteral("WaibuSnap 选区"));
    if (!actions_.copyImage)
        actions_.copyImage = copyImageToClipboard;
    if (!actions_.setClipboardText)
        actions_.setClipboardText = [](const QString& text)
        { QApplication::clipboard()->setText(text); };
    recognitionProgress_.setSingleShot(true);
    connect(&recognitionProgress_, &QTimer::timeout, this,
            [this]
            {
                if (textMode_ && recognizingText_ && !finished_)
                    showStatus(QStringLiteral("正在识别文字…"), false);
            });
    statusTimeout_.setSingleShot(true);
    connect(&statusTimeout_, &QTimer::timeout, this,
            [this]
            {
                if (status_)
                    status_->hide();
                updateToolbar();
            });
    magnifierTimeout_.setSingleShot(true);
    magnifierTimeout_.setTimerType(Qt::PreciseTimer);
    connect(&magnifierTimeout_, &QTimer::timeout, this, &SelectionOverlay::hideMagnifier);
}
void SelectionOverlay::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!overlayLevelRaised_)
    {
        raiseOverlayAboveSystemChrome(windowHandle());
        overlayLevelRaised_ = true;
    }
}
void SelectionOverlay::ensureToolbar()
{
    if (toolbar_)
        return;
    style_.fontFamily = font().family();
    // 选区成立后才创建工具和快捷键，未选区首帧不初始化标注控件 / 文本排版。
    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("selectionToolbar"));
    optionsBar_ = new QWidget(this);
    optionsBar_->setObjectName(QStringLiteral("annotationOptionsBar"));
    ratioBar_ = new QWidget(this);
    ratioBar_->setObjectName(QStringLiteral("ratioBar"));
    ratioBar_->setStyleSheet(QStringLiteral(
        "QWidget#ratioBar { background: #202020; border: 1px solid #3a3a3a; border-radius: 8px; }"
        "QPushButton { background: transparent; color: #f2f2f2; border: none; border-radius: 6px; "
        "padding: 0 8px; font-size: 12px; }"
        "QPushButton:hover { background: #3a3a3a; }"
        "QPushButton:checked { background: #a65316; color: white; }"));
    toolbar_->setStyleSheet(annotation_toolbar::pillStyle(QStringLiteral("selectionToolbar"), 10));
    optionsBar_->setStyleSheet(
        annotation_toolbar::pillStyle(QStringLiteral("annotationOptionsBar"), 8));
    const auto makeButton =
        [](QWidget* parent, const QString& object, const QString& tip, ToolbarIcon icon)
    { return annotation_toolbar::makeButton(parent, object, tip, icon); };
    const auto makeSeparator = [](QWidget* parent)
    { return annotation_toolbar::makeSeparator(parent); };
    for (int index = 0; index < 3; ++index)
        separators_.append(makeSeparator(toolbar_));
    // 二级变体按钮与颜色 / 线宽同处一条参数栏：toolBar_ 只作为透明内层容器，
    // 由 optionsBar_ 统一绘制唯一的圆角背景，避免两层黑条上下堆叠。
    toolBar_ = new QWidget(optionsBar_);
    toolBar_->setObjectName(QStringLiteral("annotationToolBar"));
    toolBar_->setStyleSheet(
        QStringLiteral("QWidget#annotationToolBar { background: transparent; border: none; }") +
        annotation_toolbar::flatButtonStyle());
    toolBar_->setFixedSize(68, 36);
    toolBar_->hide();
    selectButton_ = makeButton(toolbar_, QStringLiteral("selectToolButton"),
                               QStringLiteral("选择标注：点击选中，拖动移动，拖动上方圆点旋转"),
                               ToolbarIcon::Select);
    selectButton_->setCheckable(true);
    connect(selectButton_, &QPushButton::clicked, this, &SelectionOverlay::toggleSelectMode);
    for (int slot = 0; slot < int(toolTypes_.size()); ++slot)
    {
        const auto type = toolTypes_[slot];
        auto* button = makeButton(toolbar_, annotationGroupObject(slot), annotationToolTip(type),
                                  static_cast<ToolbarIcon>(type));
        button->setCheckable(true);
        toolButtons_.append(button);
        connect(button, &QPushButton::clicked, this,
                [this, slot] { activateTool(toolTypes_[slot]); });
    }
    if (actions_.watermarks)
    {
        watermarkToolButton_ = makeButton(
            toolbar_, QStringLiteral("watermarkToolButton"),
            QStringLiteral("水印：拖动水印，拖动圆点旋转，拖动角缩放"), ToolbarIcon::Watermark);
        watermarkToolButton_->setCheckable(true);
        connect(watermarkToolButton_, &QPushButton::clicked, this,
                &SelectionOverlay::toggleWatermarkTool);
    }
    for (auto type : {AnnotationType::Rectangle, AnnotationType::Ellipse, AnnotationType::Line,
                      AnnotationType::Arrow, AnnotationType::Cover, AnnotationType::Mosaic})
    {
        const QString tip = type == AnnotationType::Mosaic
                                ? annotationToolTip(type)
                                : QStringLiteral("使用%1").arg(annotationToolName(type));
        auto* button =
            makeButton(toolBar_, annotationToolObject(type), tip, static_cast<ToolbarIcon>(type));
        button->setProperty("annotationType", static_cast<int>(type));
        button->setCheckable(true);
        subtoolButtons_.append(button);
        connect(button, &QPushButton::clicked, this, [this, type] { activateTool(type, false); });
    }
    ocrButton_ = makeButton(toolbar_, QStringLiteral("ocrToolButton"),
                            QStringLiteral("取字（文字识别）"), ToolbarIcon::Ocr);
    ocrButton_->setCheckable(true);
    connect(ocrButton_, &QPushButton::clicked, this, &SelectionOverlay::toggleTextMode);
    longImageButton_ =
        makeButton(toolbar_, QStringLiteral("longImageButton"),
                   scrollSupported_ ? QStringLiteral("截长图：以当前选区为滚动采集区域")
                                    : QStringLiteral("截长图暂不支持当前平台（Windows 后补）。"),
                   ToolbarIcon::LongImage);
    longImageButton_->setEnabled(scrollSupported_);
    connect(longImageButton_, &QPushButton::clicked, this, [this] { enterScrollReady(); });
    const QString actionObjects[] = {QStringLiteral("undoAnnotationButton"),
                                     QStringLiteral("redoAnnotationButton"),
                                     QStringLiteral("copyButton"),
                                     QStringLiteral("saveButton"),
                                     QStringLiteral("pinButton"),
                                     QStringLiteral("cancelButton")};
    const QString actionTips[] = {
        QStringLiteral("撤销标注（%1）")
            .arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)),
        QStringLiteral("重做标注（%1）")
            .arg(QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText)),
        QStringLiteral("复制（%1）")
            .arg(QKeySequence(QKeySequence::Copy).toString(QKeySequence::NativeText)),
        QStringLiteral("保存（快速保存）"),
        QStringLiteral("钉到屏幕"),
        QStringLiteral("取消（Esc）")};
    for (int index = 0; index < 6; ++index)
        actionButtons_.append(makeButton(toolbar_, actionObjects[index], actionTips[index],
                                         static_cast<ToolbarIcon>(12 + index)));
    ratioButton_ =
        makeButton(toolbar_, QStringLiteral("ratioButton"), QString(), ToolbarIcon::Ratio);
    ratioButton_->setCheckable(true);
    connect(ratioButton_, &QPushButton::clicked, this,
            [this]
            {
                finishText(true, false);
                updateToolbar();
                update();
            });
    for (int index = -1; index < actions_.ratios.enabled.size(); ++index)
    {
        const auto ratio =
            index < 0 ? std::nullopt : std::optional<AspectRatio>(actions_.ratios.enabled[index]);
        const QString text = ratio ? formatAspectRatio(*ratio) : QStringLiteral("自由");
        auto* button = new QPushButton(text, ratioBar_);
        button->setObjectName(index < 0 ? QStringLiteral("ratioFreeButton")
                                        : QStringLiteral("ratioOption%1Button").arg(index));
        button->setToolTip(ratio ? QStringLiteral("使用 %1").arg(text)
                                 : QStringLiteral("自由（不限制比例）"));
        button->setAccessibleName(button->toolTip());
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(28);
        ratioButtons_.append(button);
        connect(button, &QPushButton::clicked, this, [this, ratio] { chooseRatio(ratio); });
    }
    shapeBar_ = new QWidget(this);
    shapeBar_->setObjectName(QStringLiteral("shapeBar"));
    QString shapeStyle = ratioBar_->styleSheet();
    shapeStyle.replace(QStringLiteral("QWidget#ratioBar"), QStringLiteral("QWidget#shapeBar"));
    shapeBar_->setStyleSheet(shapeStyle);
    shapeButton_ =
        makeButton(toolbar_, QStringLiteral("shapeButton"), QString(), ToolbarIcon::Shape);
    shapeButton_->setCheckable(true);
    connect(shapeButton_, &QPushButton::clicked, this,
            [this]
            {
                finishText(true, false);
                updateToolbar();
                update();
            });
    for (int index = -1; index < 6 + actions_.shapeTemplates.size(); ++index)
    {
        const QString label = index < 0   ? QStringLiteral("自由")
                              : index < 6 ? captureShapeLabel(static_cast<CaptureShape>(index))
                                          : actions_.shapeTemplates[index - 6].label;
        auto* button = new QPushButton(label, shapeBar_);
        button->setObjectName(index < 0   ? QStringLiteral("shapeFreeButton")
                              : index < 6 ? QStringLiteral("shapeOption%1Button").arg(index)
                                          : QStringLiteral("shapeTemplate%1Button").arg(index - 6));
        button->setToolTip(index < 0   ? QStringLiteral("自由（矩形输出）")
                           : index < 6 ? QStringLiteral("使用 %1").arg(label)
                                       : QStringLiteral("使用模板 %1").arg(label));
        button->setAccessibleName(button->toolTip());
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(28);
        shapeButtons_.append(button);
        connect(button, &QPushButton::clicked, this, [this, index] { chooseShape(index); });
    }
    shapeWhiteBackgroundButton_ = new QPushButton(QStringLiteral("白底"), shapeBar_);
    shapeWhiteBackgroundButton_->setObjectName(QStringLiteral("shapeWhiteBackgroundButton"));
    shapeWhiteBackgroundButton_->setToolTip(QStringLiteral("输出时在形状外补白底"));
    shapeWhiteBackgroundButton_->setAccessibleName(shapeWhiteBackgroundButton_->toolTip());
    shapeWhiteBackgroundButton_->setCheckable(true);
    shapeWhiteBackgroundButton_->setFocusPolicy(Qt::NoFocus);
    shapeWhiteBackgroundButton_->setFixedHeight(28);
    connect(shapeWhiteBackgroundButton_, &QPushButton::clicked, this,
            [this]
            {
                markDirty();
                updateToolbar();
            });
    shapeBar_->hide();
    ratioBar_->hide();
    if (actions_.watermarks)
    {
        watermarkBar_ = new QWidget(this);
        watermarkBar_->setObjectName(QStringLiteral("watermarkBar"));
        watermarkBar_->setStyleSheet(
            annotation_toolbar::pillStyle(QStringLiteral("watermarkBar"), 8) +
            QStringLiteral("QComboBox, QSpinBox { background: #2e2e2e; color: #f2f2f2; "
                           "border: 1px solid #4a4a4a; border-radius: 4px; padding: 1px 4px; "
                           "font-size: 12px; }"));
        auto* watermarkLayout = new QHBoxLayout(watermarkBar_);
        watermarkLayout->setContentsMargins(4, 4, 4, 4);
        watermarkLayout->setSpacing(4);
        watermarkPresetCombo_ = new QComboBox(watermarkBar_);
        watermarkPresetCombo_->setObjectName(QStringLiteral("watermarkPresetCombo"));
        watermarkPresetCombo_->setFocusPolicy(Qt::NoFocus);
        watermarkPresetCombo_->setFixedHeight(26);
        watermarkPresetCombo_->setFixedWidth(88);
        watermarkLayout->addWidget(watermarkPresetCombo_);
        watermarkOpacitySpin_ = new QSpinBox(watermarkBar_);
        watermarkOpacitySpin_->setObjectName(QStringLiteral("watermarkOpacitySpin"));
        watermarkOpacitySpin_->setRange(watermarkMinimumOpacity, watermarkMaximumOpacity);
        watermarkOpacitySpin_->setSuffix(QStringLiteral("%"));
        watermarkOpacitySpin_->setToolTip(QStringLiteral("水印透明度"));
        watermarkOpacitySpin_->setAccessibleName(QStringLiteral("水印透明度"));
        watermarkOpacitySpin_->setFixedHeight(26);
        watermarkOpacitySpin_->setFixedWidth(58);
        watermarkLayout->addWidget(watermarkOpacitySpin_);
        watermarkScaleSpin_ = new QSpinBox(watermarkBar_);
        watermarkScaleSpin_->setObjectName(QStringLiteral("watermarkScaleSpin"));
        watermarkScaleSpin_->setRange(watermarkMinimumScale, watermarkMaximumScale);
        watermarkScaleSpin_->setSuffix(QStringLiteral("%"));
        watermarkScaleSpin_->setToolTip(QStringLiteral("水印缩放"));
        watermarkScaleSpin_->setAccessibleName(QStringLiteral("水印缩放"));
        watermarkScaleSpin_->setFixedHeight(26);
        watermarkScaleSpin_->setFixedWidth(58);
        watermarkLayout->addWidget(watermarkScaleSpin_);
        const auto textButton =
            [this, watermarkLayout](const QString& object, const QString& text, const QString& tip)
        {
            auto* button = new QPushButton(text, watermarkBar_);
            button->setObjectName(object);
            button->setToolTip(tip);
            button->setAccessibleName(tip);
            button->setFixedSize(24, 26);
            button->setFocusPolicy(Qt::NoFocus);
            watermarkLayout->addWidget(button);
            return button;
        };
        watermarkVisibilityButton_ = textButton(QStringLiteral("watermarkVisibilityButton"),
                                                QStringLiteral("隐"), QStringLiteral("隐藏水印"));
        watermarkResetButton_ = textButton(QStringLiteral("watermarkResetButton"),
                                           QStringLiteral("重"), QStringLiteral("重置为预设默认"));
        watermarkRemoveButton_ = textButton(QStringLiteral("watermarkRemoveButton"),
                                            QStringLiteral("删"), QStringLiteral("移除水印"));
        connect(watermarkPresetCombo_, &QComboBox::currentIndexChanged, this,
                [this]
                {
                    if (!actions_.watermarks || !watermarkPresetCombo_)
                        return;
                    const QString id = watermarkPresetCombo_->currentData().toString();
                    if (id.isEmpty() || id == actions_.watermarks->currentPresetId())
                        return;
                    if (!actions_.watermarks->setCurrentPreset(id).isEmpty())
                        return;
                    if (watermark_)
                        applyWatermarkPreset();
                    else if (watermarkToolActive_)
                        applyWatermarkPreset();
                    updateToolbar();
                    update();
                });
        connect(watermarkOpacitySpin_, &QSpinBox::valueChanged, this,
                [this](int value)
                {
                    if (!watermark_)
                        return;
                    watermark_->opacity = value;
                    markDirty();
                    update();
                });
        connect(watermarkScaleSpin_, &QSpinBox::valueChanged, this,
                [this](int value)
                {
                    if (!watermark_)
                        return;
                    watermark_->scale = value;
                    markDirty();
                    update();
                });
        connect(watermarkVisibilityButton_, &QPushButton::clicked, this,
                [this]
                {
                    if (!watermark_)
                        return;
                    watermark_->visible = !watermark_->visible;
                    markDirty();
                    syncWatermarkBar();
                    update();
                });
        connect(watermarkResetButton_, &QPushButton::clicked, this,
                &SelectionOverlay::resetWatermark);
        connect(watermarkRemoveButton_, &QPushButton::clicked, this,
                &SelectionOverlay::removeWatermark);
        watermarkBar_->hide();
    }
    undoButton_ = actionButtons_[0];
    redoButton_ = actionButtons_[1];
    extractAllTextButton_ = makeButton(toolbar_, QStringLiteral("extractAllTextButton"),
                                       QStringLiteral("提取全文"), ToolbarIcon::ExtractAllText);
    extractAllTextButton_->hide();
    connect(extractAllTextButton_, &QPushButton::clicked, this, &SelectionOverlay::openOcrDialog);
    copyShortcut_ = new QShortcut(QKeySequence::Copy, this);
    connect(copyShortcut_, &QShortcut::activated, this, &SelectionOverlay::copyShortcut);
    undoShortcut_ = new QShortcut(QKeySequence::Undo, this);
    redoShortcut_ = new QShortcut(QKeySequence::Redo, this);
    connect(undoShortcut_, &QShortcut::activated, this, &SelectionOverlay::undoAnnotation);
    connect(redoShortcut_, &QShortcut::activated, this, &SelectionOverlay::redoAnnotation);
    connect(undoButton_, &QPushButton::clicked, this, &SelectionOverlay::undoAnnotation);
    connect(redoButton_, &QPushButton::clicked, this, &SelectionOverlay::redoAnnotation);
    connect(actionButtons_[2], &QPushButton::clicked, this, &SelectionOverlay::copySelection);
    connect(actionButtons_[3], &QPushButton::clicked, this, &SelectionOverlay::saveSelection);
    connect(actionButtons_[4], &QPushButton::clicked, this, &SelectionOverlay::pinSelection);
    connect(actionButtons_[5], &QPushButton::clicked, this,
            [this] { complete(cancelledSessionOutcome); });
    colors_ = new AnnotationColorPalette(optionsBar_, QStringLiteral("annotationColor"),
                                         [this](int index)
                                         {
                                             finishText(true, false);
                                             style_.color = annotationColors()[index];
                                             updateToolbar();
                                         });
    widthLabel_ = new QLabel(optionsBar_);
    widthLabel_->setObjectName(QStringLiteral("annotationWidthLabel"));
    widthLabel_->setStyleSheet(QStringLiteral("color: #e8e8e8; background: transparent;"));
    const QString sizeNames[] = {QStringLiteral("小"), QStringLiteral("中"), QStringLiteral("大")};
    for (int index = 0; index < int(annotationTextSizes.size()); ++index)
    {
        auto* size = makeButton(
            optionsBar_, QStringLiteral("annotationTextSize%1Button").arg(index),
            QStringLiteral("%1 %2 像素").arg(sizeNames[index]).arg(annotationTextSizes[index]),
            ToolbarIcon::TextSize);
        const qreal scales[] = {0.72, 0.86, 1.0};
        size->setIcon(toolbarIcon(ToolbarIcon::TextSize, {}, scales[index]));
        size->setCheckable(true);
        size->setFocusPolicy(Qt::NoFocus);
        textSizeButtons_.append(size);
        connect(size, &QPushButton::clicked, this,
                [this, index] { setTextSize(annotationTextSizes[index]); });
    }
    textSizeLabel_ = new QLabel(optionsBar_);
    textSizeLabel_->setObjectName(QStringLiteral("annotationTextSizeLabel"));
    textSizeLabel_->setStyleSheet(
        QStringLiteral("color: #e8e8e8; background: transparent; font-size: 12px;"));
    textSizeLabel_->setAlignment(Qt::AlignCenter);
    const auto makeFormatButton =
        [this](const QString& text, const QString& object, const QString& tip, int style)
    {
        auto* button = new QPushButton(text, optionsBar_);
        button->setObjectName(object);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFixedSize(28, 28);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        QFont font = button->font();
        font.setBold(style == 0);
        font.setItalic(style == 1);
        font.setUnderline(style == 2);
        button->setFont(font);
        return button;
    };
    const auto shortcutText = [](Qt::Key key)
    { return QKeySequence(Qt::CTRL | key).toString(QKeySequence::NativeText); };
    boldButton_ = makeFormatButton(QStringLiteral("B"), QStringLiteral("annotationTextBoldButton"),
                                   QStringLiteral("加粗（%1）").arg(shortcutText(Qt::Key_B)), 0);
    italicButton_ =
        makeFormatButton(QStringLiteral("I"), QStringLiteral("annotationTextItalicButton"),
                         QStringLiteral("斜体（%1）").arg(shortcutText(Qt::Key_I)), 1);
    underlineButton_ =
        makeFormatButton(QStringLiteral("U"), QStringLiteral("annotationTextUnderlineButton"),
                         QStringLiteral("下划线（%1）").arg(shortcutText(Qt::Key_U)), 2);
    connect(boldButton_, &QPushButton::clicked, this,
            [this] { toggleTextStyle(TextStyleToggle::Bold); });
    connect(italicButton_, &QPushButton::clicked, this,
            [this] { toggleTextStyle(TextStyleToggle::Italic); });
    connect(underlineButton_, &QPushButton::clicked, this,
            [this] { toggleTextStyle(TextStyleToggle::Underline); });
    optionsBar_->hide();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("outputStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setIndent(0);
    status_->setStyleSheet(QStringLiteral(
        "background: #202020; color: #e8e8e8; border-radius: 8px; padding: 0 10px;"));
    status_->hide();
    for (auto* panel : {toolbar_, toolBar_, optionsBar_, ratioBar_, shapeBar_, watermarkBar_,
                        static_cast<QWidget*>(status_)})
    {
        if (!panel)
            continue;
        panel->setCursor(Qt::ArrowCursor);
        panel->setMouseTracking(true);
        panel->installEventFilter(this);
        for (QWidget* widget : panel->findChildren<QWidget*>())
        {
            widget->setMouseTracking(true);
            widget->installEventFilter(this);
        }
    }
}
QRect SelectionOverlay::availableRect() const
{
    const QRect available = frame_.display.availableLogicalGeometry.isEmpty()
                                ? frame_.display.logicalGeometry
                                : frame_.display.availableLogicalGeometry;
    const QRect local = available.translated(-frame_.display.logicalGeometry.topLeft()) & rect();
    return local.isEmpty() ? rect() : local;
}
void SelectionOverlay::layoutToolbar(int availableWidth)
{
    for (auto* separator : separators_)
        separator->hide();
    QVector<QWidget*> tools;
    if (selectButton_)
        tools.append(selectButton_);
    for (auto* tool : toolButtons_)
        tools.append(tool);
    if (watermarkToolButton_)
        tools.append(watermarkToolButton_);
    tools.append(ocrButton_);
    tools.append(longImageButton_);
    QVector<QWidget*> actions;
    if (textMode_)
        actions << extractAllTextButton_ << separators_[1];
    actions << undoButton_ << redoButton_ << separators_[2] << ratioButton_ << shapeButton_;
    for (int index = 2; index < actionButtons_.size(); ++index)
        actions.append(actionButtons_[index]);
    const auto rowWidth = [](const QVector<QWidget*>& row, int gap = 4)
    {
        int result = 16 - gap;
        for (auto* widget : row)
            result += (qobject_cast<QLabel*>(widget) ? 17 : 28) + gap;
        return result;
    };
    const auto placeRow = [](const QVector<QWidget*>& row, int y, int gap = 4)
    {
        int x = 8;
        for (auto* widget : row)
        {
            const bool separator = qobject_cast<QLabel*>(widget);
            widget->move(x + (separator ? 8 : 0), y + (separator ? 3 : 0));
            widget->show();
            x += (separator ? 17 : 28) + gap;
        }
    };
    QVector<QWidget*> single = tools;
    single << separators_[0];
    single += actions;
    if (rowWidth(single) <= availableWidth)
    {
        toolbar_->setFixedSize(rowWidth(single), 44);
        placeRow(single, 8);
    }
    else if (availableWidth > 320 && rowWidth(tools) <= availableWidth)
    {
        toolbar_->setFixedSize(std::max(rowWidth(tools), rowWidth(actions)), 76);
        placeRow(tools, 8);
        placeRow(actions, 40);
    }
    else
    {
        // 320 逻辑像素使用三列工具网格，操作组仍紧凑成行。
        // 取字多一枚操作按钮；320px 下缩小间距，保留全部 28px 按钮与三行工具。
        const int gap = rowWidth(actions) > availableWidth ? 1 : 4;
        const int actionsY = 8 + int((tools.size() + 2) / 3) * 32;
        toolbar_->setFixedSize(std::max(108, rowWidth(actions, gap)), actionsY + 36);
        for (int index = 0; index < tools.size(); ++index)
        {
            tools[index]->move(8 + index % 3 * 32, 8 + index / 3 * 32);
            tools[index]->show();
        }
        placeRow(actions, actionsY, gap);
    }
}
void SelectionOverlay::updateOptionsBar()
{
    const int activeSlot = activeTool_ ? annotationToolSlot(*activeTool_) : -1;
    const bool grouped = activeSlot == 0 || activeSlot == 1 || activeSlot == 4;
    int toolX = 4;
    for (auto* button : subtoolButtons_)
    {
        const auto type = static_cast<AnnotationType>(button->property("annotationType").toInt());
        const bool visible = grouped && annotationToolSlot(type) == activeSlot;
        button->setVisible(visible);
        button->setChecked(activeTool_ == type);
        if (visible)
        {
            button->move(toolX, 4);
            toolX += 32;
        }
    }
    for (int slot = 0; slot < toolButtons_.size(); ++slot)
    {
        auto* button = toolButtons_[slot];
        const auto type = toolTypes_[slot];
        button->setChecked(!selectMode_ && activeTool_ == type);
        const QString tip = annotationToolTip(type);
        if (button->toolTip() != tip)
        {
            button->setToolTip(tip);
            button->setAccessibleName(tip);
            button->setIcon(toolbarIcon(static_cast<ToolbarIcon>(type)));
        }
    }
    if (selectButton_)
        selectButton_->setChecked(selectMode_);
    const bool showTools = !selectMode_ && grouped && !textMode_;
    const bool colors =
        !selectMode_ && !textMode_ && activeTool_ && activeTool_ != AnnotationType::Mosaic;
    const bool widths = !selectMode_ && activeTool_ && annotationUsesLineWidth(*activeTool_);
    const bool sizes = colors && activeTool_ == AnnotationType::Text;
    const int barHeight = colors || sizes ? 68 : (showTools ? 44 : 0);
    // 一级组态、颜色 / 线宽 / 字号全部排在同一条参数栏内：
    // 左侧是矩形 / 椭圆等变体，右侧是颜色，最后是线宽数值或文本字号。
    int x = 4;
    toolBar_->setVisible(showTools);
    if (showTools)
    {
        toolBar_->move(x, (barHeight - toolBar_->height()) / 2);
        x += toolBar_->width() + 4;
    }
    colors_->setVisible(colors);
    colors_->setColor(style_.color);
    if (colors)
    {
        colors_->move(x, (barHeight - colors_->height()) / 2);
        x += colors_->width() + 4;
    }
    widthLabel_->setVisible(widths);
    widthLabel_->setText(QStringLiteral("线宽 %1px").arg(style_.lineWidth));
    widthLabel_->setToolTip(QStringLiteral("画布上滚轮调整线宽（1–24px，每格 1px）"));
    widthLabel_->setAccessibleName(widthLabel_->text() + QStringLiteral("；") +
                                   widthLabel_->toolTip());
    widthLabel_->setAlignment(Qt::AlignCenter);
    if (widths)
    {
        widthLabel_->setGeometry(x, (barHeight - 28) / 2, 64, 28);
        x += 64 + 4;
    }
    const int availableWidth = availableRect().width();
    const bool showTextSizeLabel = sizes && availableWidth >= 340;
    const bool showTextPresets = sizes && availableWidth >= 440;
    for (auto* button : {boldButton_, italicButton_, underlineButton_})
        button->setVisible(sizes);
    if (sizes)
    {
        boldButton_->move(x, (barHeight - 28) / 2);
        italicButton_->move(x + 32, (barHeight - 28) / 2);
        underlineButton_->move(x + 64, (barHeight - 28) / 2);
        x += 96 + 4;
    }
    boldButton_->setChecked(style_.bold);
    italicButton_->setChecked(style_.italic);
    underlineButton_->setChecked(style_.underline);
    textSizeLabel_->setVisible(showTextSizeLabel);
    textSizeLabel_->setText(QStringLiteral("字号 %1px").arg(style_.textSize));
    textSizeLabel_->setToolTip(QStringLiteral("画布上滚轮调整字号（9–96px，每格 1px）"));
    textSizeLabel_->setAccessibleName(textSizeLabel_->text() + QStringLiteral("；") +
                                      textSizeLabel_->toolTip());
    if (showTextSizeLabel)
    {
        textSizeLabel_->setGeometry(x, (barHeight - 28) / 2, 64, 28);
        x += 64 + 4;
    }
    for (int index = 0; index < textSizeButtons_.size(); ++index)
    {
        textSizeButtons_[index]->setVisible(showTextPresets);
        textSizeButtons_[index]->setChecked(style_.textSize == annotationTextSizes[index]);
        if (showTextPresets)
            textSizeButtons_[index]->move(x + index * 32, (barHeight - 28) / 2);
    }
    if (showTextPresets)
        x += int(textSizeButtons_.size()) * 32 + 4;
    if (!showTools && !colors && !sizes)
    {
        optionsBar_->hide();
        return;
    }
    optionsBar_->setFixedSize(x, barHeight);
    optionsBar_->setVisible(true);
}
std::optional<AspectRatio> SelectionOverlay::dragRatio() const
{
    if (activeShape_ >= 0)
    {
        if (!shiftRatio_)
            return {};
        if (activeShape_ < 6)
            return captureShapeNaturalRatio(static_cast<CaptureShape>(activeShape_));
        const auto& mask = templateMasks_[activeShape_ - 6];
        return AspectRatio{double(mask.width()), double(mask.height())};
    }
    return shiftRatio_ ? actions_.ratios.quick : activeRatio_;
}
void SelectionOverlay::chooseRatio(std::optional<AspectRatio> ratio)
{
    if (activeShape_ >= 0)
        return;
    finishText(true, false);
    if (ratio && activeRatio_ && sameAspectRatio(*ratio, *activeRatio_))
        ratio.reset();
    activeRatio_ = ratio;
    if (ratio && !selection_.isEmpty() && dragMode_ == DragMode::None)
    {
        setSelection(ratioAdjustedSelection(selection_, *ratio, frame_.pixels.size()));
        if (textMode_)
            startOcrRecognition();
    }
    updateToolbar();
    update();
}
void SelectionOverlay::updateRatioBar()
{
    QString tip =
        QStringLiteral("选择截图比例（当前：%1）")
            .arg(activeRatio_ ? formatAspectRatio(*activeRatio_) : QStringLiteral("自由"));
    if (actions_.ratios.quick)
        tip +=
            QStringLiteral("；按住 ⇧ 临时使用 %1").arg(formatAspectRatio(*actions_.ratios.quick));
    ratioButton_->setEnabled(activeShape_ < 0);
    if (activeShape_ >= 0)
        tip = QStringLiteral("形状截图使用模板比例锁定");
    ratioButton_->setToolTip(tip);
    ratioButton_->setAccessibleName(tip);
    int naturalWidth = 4;
    for (int index = 0; index < ratioButtons_.size(); ++index)
    {
        auto* button = ratioButtons_[index];
        button->ensurePolished();
        button->setFixedWidth(
            std::max(34, button->fontMetrics().horizontalAdvance(button->text()) + 16));
        button->setChecked(
            index == 0 ? !activeRatio_
                       : activeRatio_ &&
                             sameAspectRatio(*activeRatio_, actions_.ratios.enabled[index - 1]));
        naturalWidth += button->width() + 4;
    }
    const int width = std::min(naturalWidth, toolbar_->width());
    int x = 4;
    int y = 4;
    for (auto* button : ratioButtons_)
    {
        if (x > 4 && x + button->width() + 4 > width)
        {
            x = 4;
            y += 32;
        }
        button->move(x, y);
        x += button->width() + 4;
    }
    ratioBar_->setFixedSize(width, y + 32);
    ratioBar_->setVisible(activeShape_ < 0 && ratioButton_->isChecked());
}
QString SelectionOverlay::shapeLabel() const
{
    return activeShape_ < 0   ? QStringLiteral("自由")
           : activeShape_ < 6 ? captureShapeLabel(static_cast<CaptureShape>(activeShape_))
                              : actions_.shapeTemplates[activeShape_ - 6].label;
}
void SelectionOverlay::chooseShape(int index)
{
    finishText(true, false);
    activeShape_ = index == activeShape_ ? -1 : index;
    templatePreview_ = {};
    templatePreviewSize_ = {};
    if (activeShape_ >= 0)
    {
        activeRatio_.reset();
        ratioButton_->setChecked(false);
    }
    markDirty();
    if (activeShape_ >= 0)
        showStatus(QStringLiteral("标注超出形状的部分会在输出时裁掉。"), true);
    updateToolbar();
    update();
}
void SelectionOverlay::updateShapeBar()
{
    const QString tip = QStringLiteral("选择异形形状（当前：%1）").arg(shapeLabel());
    shapeButton_->setToolTip(tip);
    shapeButton_->setAccessibleName(tip);
    QVector<QPushButton*> buttons = shapeButtons_;
    buttons.append(shapeWhiteBackgroundButton_);
    int naturalWidth = 4;
    for (auto* button : buttons)
    {
        button->ensurePolished();
        button->setFixedWidth(
            std::min(toolbar_->width() - 8,
                     std::max(34, button->fontMetrics().horizontalAdvance(button->text()) + 16)));
        naturalWidth += button->width() + 4;
    }
    for (int index = 0; index < shapeButtons_.size(); ++index)
        shapeButtons_[index]->setChecked(index - 1 == activeShape_);
    const int width = std::min(naturalWidth, toolbar_->width());
    int x = 4;
    int y = 4;
    for (auto* button : buttons)
    {
        if (x > 4 && x + button->width() + 4 > width)
        {
            x = 4;
            y += 32;
        }
        button->move(x, y);
        x += button->width() + 4;
    }
    // 白底固定在末行右端，顺序与换行保持一致。
    shapeWhiteBackgroundButton_->move(width - shapeWhiteBackgroundButton_->width() - 4, y);
    shapeBar_->setFixedSize(width, y + 32);
    shapeBar_->setVisible(shapeButton_->isChecked());
}
QImage SelectionOverlay::outputImage(bool includeWatermark) const
{
    QImage image = renderAnnotatedSelection(frame_.pixels, annotations(), selection_);
    if (includeWatermark && watermark_ && watermark_->visible && !image.isNull() &&
        actions_.watermarks)
    {
        const auto* preset = actions_.watermarks->preset(watermark_->presetId);
        const auto* asset = preset ? actions_.watermarks->asset(preset->assetId) : nullptr;
        if (preset && asset)
        {
            QString error;
            const QImage assetImage = actions_.watermarks->assetImage(asset->id, &error);
            if (asset->kind == WatermarkKind::Text || !assetImage.isNull())
                image = composeWatermark(image, *asset, assetImage, *watermark_);
        }
    }
    if (activeShape_ < 0)
        return image;
    QImage mask;
    if (activeShape_ < 6)
    {
        mask = QImage(image.size(), QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(captureShapePath(static_cast<CaptureShape>(activeShape_), mask.rect()),
                         Qt::white);
    }
    else
        mask = templateMasks_[activeShape_ - 6];
    return applyShapeMask(image, mask, shapeWhiteBackgroundButton_->isChecked());
}
QRect SelectionOverlay::toolbarClusterRect() const
{
    QRect cluster;
    // toolBar_ 是 optionsBar_ 的透明内层容器，几何相对父级，不能直接并入覆盖层坐标。
    for (auto* panel :
         {toolbar_, optionsBar_, ratioBar_, shapeBar_, watermarkBar_, scrollReadyBar_})
        if (panel && !panel->isHidden())
            cluster = cluster.united(panel->geometry());
    return cluster;
}
void SelectionOverlay::positionStatus()
{
    if (status_->isHidden())
        return;
    const QRect bounds = availableRect();
    const int maximumWidth = std::min(toolbar_->width(), bounds.width());
    const int naturalWidth = status_->fontMetrics().horizontalAdvance(status_->text()) + 20;
    const int pillWidth = std::min(maximumWidth, naturalWidth);
    status_->setFixedWidth(pillWidth);
    // 由 QLabel 按实际样式计算换行高度，避免字体度量与控件排版差异裁掉末行。
    const int pillHeight = std::max(26, status_->heightForWidth(pillWidth) + 8);
    status_->setFixedHeight(pillHeight);
    const int x = std::clamp(toolbarClusterRect().right() + 1 - pillWidth, bounds.left(),
                             std::max(bounds.left(), bounds.right() + 1 - pillWidth));
    const int clusterBottom = toolbarClusterRect().bottom();
    int y = clusterBottom + 11;
    if (y + pillHeight > bounds.bottom() + 1)
        y = toolbar_->y() - pillHeight - 10;
    y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() + 1 - pillHeight));
    status_->move(x, y);
}
void SelectionOverlay::updateToolbar()
{
    if (scrollReady_)
    {
        toolbar_->hide();
        toolBar_->hide();
        optionsBar_->hide();
        ratioBar_->hide();
        shapeBar_->hide();
        if (watermarkBar_)
            watermarkBar_->hide();
        undoShortcut_->setEnabled(false);
        redoShortcut_->setEnabled(false);
        copyShortcut_->setEnabled(false);
        updateScrollReadyBar();
        positionStatus();
        return;
    }
    if (scrollReadyBar_)
        scrollReadyBar_->hide();
    if (finished_ || selection_.isEmpty() || dragMode_ == DragMode::Create)
    {
        if (toolbar_)
        {
            toolbar_->hide();
            toolBar_->hide();
            optionsBar_->hide();
            ratioBar_->hide();
            shapeBar_->hide();
            if (watermarkBar_)
                watermarkBar_->hide();
            status_->hide();
        }
        if (undoShortcut_)
        {
            undoShortcut_->setEnabled(false);
            redoShortcut_->setEnabled(false);
        }
        return;
    }
    ensureToolbar();
    const bool editingText = textEditor_ && textEditor_->isVisible();
    undoButton_->setEnabled(!textMode_ && history_.canUndo());
    redoButton_->setEnabled(!textMode_ && history_.canRedo());
    undoShortcut_->setEnabled(!textMode_ && !editingText && !saveDialogOpen_ &&
                              dragMode_ == DragMode::None);
    redoShortcut_->setEnabled(!textMode_ && !editingText && !saveDialogOpen_ &&
                              dragMode_ == DragMode::None);
    copyShortcut_->setEnabled(!editingText && !saveDialogOpen_ && !ocrDialog_);
    ocrButton_->setChecked(textMode_);
    extractAllTextButton_->setVisible(textMode_);
    const QRect bounds = availableRect();
    layoutToolbar(bounds.width());
    updateOptionsBar();
    updateRatioBar();
    updateShapeBar();
    syncWatermarkBar();
    const int optionsHeight = optionsBar_->isHidden() ? 0 : optionsBar_->height() + 6;
    const int watermarkHeight =
        !watermarkBar_ || watermarkBar_->isHidden() ? 0 : watermarkBar_->height() + 6;
    const int clusterHeight = toolbar_->height() + optionsHeight + watermarkHeight +
                              (ratioBar_->isHidden() ? 0 : ratioBar_->height() + 6) +
                              (shapeBar_->isHidden() ? 0 : shapeBar_->height() + 6);
    const QRectF region = logicalSelection();
    const int clusterWidth =
        std::max({toolbar_->width(), optionsBar_->isHidden() ? 0 : optionsBar_->width(),
                  ratioBar_->isHidden() ? 0 : ratioBar_->width(),
                  shapeBar_->isHidden() ? 0 : shapeBar_->width(),
                  !watermarkBar_ || watermarkBar_->isHidden() ? 0 : watermarkBar_->width()});
    const int x = std::clamp(qRound(region.right()) - clusterWidth, bounds.left(),
                             std::max(bounds.left(), bounds.right() + 1 - clusterWidth));
    int y = qRound(region.bottom()) + 10;
    if (y + clusterHeight > bounds.bottom() + 1)
    {
        y = qRound(region.top()) - clusterHeight - 10;
        if (y < bounds.top())
            y = bounds.bottom() + 1 - clusterHeight - 10;
    }
    y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() + 1 - clusterHeight));
    toolbar_->move(x, y);
    optionsBar_->move(x, y + toolbar_->height() + 6);
    if (watermarkBar_)
        watermarkBar_->move(x, y + toolbar_->height() + 6 + optionsHeight);
    ratioBar_->move(x, y + toolbar_->height() + 6 + optionsHeight + watermarkHeight);
    shapeBar_->move(x, y + toolbar_->height() + 6 + optionsHeight + watermarkHeight +
                           (ratioBar_->isHidden() ? 0 : ratioBar_->height() + 6));
    const bool enabled = dragMode_ == DragMode::None && !saveDialogOpen_;
    toolbar_->setEnabled(enabled);
    toolBar_->setEnabled(enabled);
    optionsBar_->setEnabled(enabled);
    ratioBar_->setEnabled(enabled);
    shapeBar_->setEnabled(enabled);
    if (watermarkBar_)
        watermarkBar_->setEnabled(enabled);
    toolbar_->show();
    positionStatus();
    if (!actions_.shapeTemplateNotice.isEmpty())
    {
        const QString notice = std::exchange(actions_.shapeTemplateNotice, QString());
        showStatus(notice, false);
    }
}

void SelectionOverlay::enterScrollReady(QRect preset)
{
    if (finished_ || scrollReady_ || saveDialogOpen_ || ocrDialog_ || !scrollSupported_)
        return;
    if (!preset.isEmpty())
        setSelection(preset & frame_.pixels.rect());
    finishText(true, false);
    exitTextMode();
    if (selection_.isEmpty())
    {
        showStatus(QStringLiteral("请先框选区域再截长图。"), true);
        return;
    }
    if (selection_.width() < 64 || selection_.height() < 64)
    {
        showStatus(QStringLiteral("区域太小，请重新框选（至少 64 × 64 像素）。"), true);
        return;
    }
    const bool annotated = history_.canUndo() || history_.canRedo();
    discardAnnotations();
    if (watermark_)
    {
        watermark_.reset();
        watermarkToolActive_ = false;
        watermarkSelected_ = false;
        markDirty();
    }
    if (annotated)
        markDirty();
    ensureToolbar();
    dragMode_ = DragMode::None;
    hoveredWindowPixels_ = {};
    hideMagnifier();
    scrollReady_ = true;
    setCursor(Qt::ArrowCursor);
    updateToolbar();
    if (annotated)
        showStatus(QStringLiteral("已清除标注，长图结果不含标注。"), true);
    else
        status_->hide();
    setFocus(Qt::OtherFocusReason);
    update();
    emit scrollReadyChanged(true);
}
void SelectionOverlay::updateScrollReadyBar()
{
    if (!scrollReadyBar_)
    {
        scrollReadyBar_ = new QWidget(this);
        scrollReadyBar_->setObjectName(QStringLiteral("scrollReadyBar"));
        scrollReadyBar_->setStyleSheet(
            QStringLiteral("QWidget#scrollReadyBar { background: #202020; border: 1px solid "
                           "#3a3a3a; border-radius: 8px; }"
                           "QLabel { color: #f2f2f2; border: none; font-size: 12px; }"
                           "QPushButton { color: white; background: #363636; border: none; "
                           "border-radius: 6px; padding: 0 6px; }"));
        auto* layout = new QHBoxLayout(scrollReadyBar_);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(4);
        auto* title = new QLabel(QStringLiteral("截长图：选区已锁定"), scrollReadyBar_);
        title->setObjectName(QStringLiteral("scrollReadyTitleLabel"));
        title->setWordWrap(true);
        layout->addWidget(title);
        auto* size = new QLabel(scrollReadyBar_);
        size->setObjectName(QStringLiteral("scrollReadySizeLabel"));
        size->setWordWrap(true);
        layout->addWidget(size);
        const auto button = [&](const QString& name, const QString& text)
        {
            auto* result = new QPushButton(text, scrollReadyBar_);
            result->setObjectName(name);
            result->setFixedHeight(28);
            result->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(result);
            return result;
        };
        connect(button(QStringLiteral("scrollStartButton"), QStringLiteral("开始采集")),
                &QPushButton::clicked, this, &SelectionOverlay::startScrollCapture);
        connect(button(QStringLiteral("scrollBackButton"), QStringLiteral("返回")),
                &QPushButton::clicked, this,
                [this]
                {
                    scrollReady_ = false;
                    updateToolbar();
                    update();
                    emit scrollReadyChanged(false);
                });
        connect(button(QStringLiteral("scrollReadyCancelButton"), QStringLiteral("取消")),
                &QPushButton::clicked, this, [this] { complete(scrollCancelledOutcome); });
    }
    scrollReadyBar_->findChild<QLabel*>(QStringLiteral("scrollReadySizeLabel"))
        ->setText(QStringLiteral("%1 × %2 像素").arg(selection_.width()).arg(selection_.height()));
    scrollReadyBar_->setGeometry(
        placeScrollBar(logicalSelection().toAlignedRect(), QSize(530, 44), availableRect()));
    scrollReadyBar_->show();
}
void SelectionOverlay::startScrollCapture()
{
    if (!scrollReady_ || finished_)
        return;
    scrollReadyBar_->hide();
    emit scrollCaptureRequested(selection_);
}
void SelectionOverlay::showStatus(const QString& text, bool temporary)
{
    ensureToolbar();
    statusTimeout_.stop();
    status_->setText(text);
    status_->setToolTip(text);
    status_->show();
    updateToolbar();
    if (selection_.isEmpty())
    {
        const QRect bounds = availableRect();
        status_->setFixedWidth(std::min(530, bounds.width()));
        status_->setFixedHeight(std::max(28, status_->heightForWidth(status_->width()) + 8));
        status_->move(bounds.center().x() - status_->width() / 2, bounds.top() + 64);
        status_->show();
    }
    if (temporary)
        statusTimeout_.start(3000);
}
void SelectionOverlay::copySelection()
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return;
    finishText(true);
    const QImage image = outputImage();
    const ImageOutputResult result = actions_.copyImage(image);
    if (result.success)
    {
        recordHistory(image);
        complete(copiedSessionOutcome);
    }
    else
        showStatus(QStringLiteral("复制失败：%1 点击「复制」重试。").arg(result.explanation),
                   false);
}
void SelectionOverlay::pinSelection()
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return;
    finishText(true);
    const QImage image = outputImage();
    const ImageOutputResult result =
        actions_.pinImage ? actions_.pinImage(image)
                          : ImageOutputResult{false, QStringLiteral("贴图管理器不可用。")};
    if (result.success)
    {
        recordHistory(image);
        complete(pinnedSessionOutcome);
    }
    else
        showStatus(QStringLiteral("钉图失败：%1 点击「钉到屏幕」重试。").arg(result.explanation),
                   false);
}
QPoint SelectionOverlay::selectionGlobalPosition() const
{
    return frame_.display.logicalGeometry.topLeft() + logicalSelection().topLeft().toPoint();
}
void SelectionOverlay::saveSelection()
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return;
    finishText(true);
    hideMagnifier();
    saveDialogOpen_ = true;
    toolbar_->setEnabled(false);
    toolBar_->setEnabled(false);
    optionsBar_->setEnabled(false);
    ratioBar_->setEnabled(false);
    shapeBar_->setEnabled(false);
    QPointer<SelectionOverlay> self(this);
    ImageSaveActions actions = actions_;
    if (activeShape_ >= 0)
        actions.forcedFormat = ImageFormat::Png;
    // 保存面板是独立 app-modal 窗口（NSModalPanelWindowLevel），低于覆盖层抬升后的
    // NSStatusWindowLevel，不临时降级会被全屏覆盖层遮挡（2026-10-13 实机取证）。
    const bool lowered = overlayLevelRaised_;
    if (lowered)
        lowerOverlayBelowSystemPanels(windowHandle());
    const auto target = chooseImageSaveTarget(
        this, actions, [self] { return self && !self->finished_; },
        [self](const QString& text)
        {
            if (self)
                self->showStatus(text, false);
        });
    if (!self)
        return;
    if (lowered)
        raiseOverlayAboveSystemChrome(windowHandle());
    saveDialogOpen_ = false;
    if (finished_)
        return;
    updateToolbar();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    if (!target.path.isEmpty())
    {
        const QImage image = outputImage();
        finishSave(saveImageToTarget(image, target), image);
    }
}
bool SelectionOverlay::exportToPath(const QString& path)
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None || path.isEmpty())
        return false;
    finishText(true);
    const QImage image = outputImage();
    return finishSave(exportImageToPath(image, activeShape_ >= 0
                                                   ? savePanelFilePath(path, ImageFormat::Png)
                                                   : path),
                      image);
}
void SelectionOverlay::recordHistory(const QImage& image, const QString& savedPath)
{
    if (actions_.recordHistory)
    {
        const auto result = actions_.recordHistory(image, savedPath);
        if (!result.success)
            qWarning().noquote() << "截图输出成功，历史写入失败：" << result.error;
    }
}
bool SelectionOverlay::finishSave(const ImageFileResult& output, const QImage& image)
{
    if (!output.result.success)
    {
        showStatus(QStringLiteral("保存失败：%1 点击「保存」选择路径并重试。")
                       .arg(output.result.explanation),
                   false);
        return false;
    }
    saved_ = true;
    recordHistory(image, output.path);
    showStatus((activeShape_ >= 0 ? QStringLiteral("形状截图以 PNG 保存\n") : QString()) +
                   QStringLiteral("已保存：%1").arg(output.path),
               true);
    return true;
}
void SelectionOverlay::setSelection(QRect pixels)
{
    if (pixels == selection_)
        return;
    selection_ = pixels;
    if (!pixels.isEmpty())
        hoveredWindowPixels_ = {};
    markDirty();
}
void SelectionOverlay::markDirty()
{
    saved_ = false;
    statusTimeout_.stop();
    if (status_)
        status_->hide();
}
QRect SelectionOverlay::selection() const { return selection_; }
QRectF SelectionOverlay::logicalSelection() const
{
    const qreal scale = frame_.display.devicePixelRatio;
    return {selection_.x() / scale, selection_.y() / scale, selection_.width() / scale,
            selection_.height() / scale};
}
SelectionEdges SelectionOverlay::edgesAt(QPointF position) const
{
    if (selection_.isEmpty())
        return {};
    const QRectF region = logicalSelection();
    if (!region.adjusted(-handleRadius, -handleRadius, handleRadius, handleRadius)
             .contains(position))
        return {};
    SelectionEdges edges;
    const qreal leftDistance = std::abs(position.x() - region.left());
    const qreal rightDistance = std::abs(position.x() - region.right());
    const qreal topDistance = std::abs(position.y() - region.top());
    const qreal bottomDistance = std::abs(position.y() - region.bottom());
    if (std::min(leftDistance, rightDistance) <= handleRadius)
        edges |= leftDistance <= rightDistance ? SelectionEdge::Left : SelectionEdge::Right;
    if (std::min(topDistance, bottomDistance) <= handleRadius)
        edges |= topDistance <= bottomDistance ? SelectionEdge::Top : SelectionEdge::Bottom;
    return edges;
}
void SelectionOverlay::updateCursor(QPointF position)
{
    if (scrollReady_)
    {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (textMode_)
    {
        setCursor(textAt(position, false).valid() ? Qt::IBeamCursor : Qt::ArrowCursor);
        return;
    }
    if (watermarkToolActive_)
    {
        if (watermarkRotationHandleHit(position))
            setCursor(Qt::CrossCursor);
        else if (watermarkScaleHandleHit(position))
            setCursor(Qt::SizeFDiagCursor);
        else if (watermarkHit(position))
            setCursor(Qt::SizeAllCursor);
        else
            setCursor(Qt::ArrowCursor);
        return;
    }
    if (selectMode_)
    {
        if (rotationHandleHit(position))
            setCursor(Qt::CrossCursor);
        else if (annotationAt(position) >= 0)
            setCursor(Qt::SizeAllCursor);
        else
            setCursor(Qt::ArrowCursor);
        return;
    }
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations().size())
    {
        if (rotationHandleHit(position))
        {
            setCursor(Qt::CrossCursor);
            return;
        }
        if (annotationHitTest(annotations()[selectedAnnotation_], physicalPoint(position), 4))
        {
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    if (activeTool_ && !selection_.isEmpty())
    {
        setCursor(*activeTool_ == AnnotationType::Text ? Qt::IBeamCursor : Qt::CrossCursor);
        return;
    }
    const SelectionEdges edges = dragMode_ == DragMode::Resize ? resizeEdges_ : edgesAt(position);
    const bool horizontal =
        edges.testFlag(SelectionEdge::Left) || edges.testFlag(SelectionEdge::Right);
    const bool vertical =
        edges.testFlag(SelectionEdge::Top) || edges.testFlag(SelectionEdge::Bottom);
    if (dragMode_ == DragMode::Create)
        setCursor(Qt::CrossCursor);
    else if (dragMode_ == DragMode::Move)
        setCursor(Qt::SizeAllCursor);
    else if (horizontal && vertical)
        setCursor(edges.testFlag(SelectionEdge::Left) == edges.testFlag(SelectionEdge::Top)
                      ? Qt::SizeFDiagCursor
                      : Qt::SizeBDiagCursor);
    else if (horizontal)
        setCursor(Qt::SizeHorCursor);
    else if (vertical)
        setCursor(Qt::SizeVerCursor);
    else
        setCursor(!selection_.isEmpty() && logicalSelection().contains(position) ? Qt::SizeAllCursor
                                                                                 : Qt::CrossCursor);
}
void SelectionOverlay::dragTo(QPointF position)
{
    lastDragPosition_ = position;
    const auto ratio = dragRatio();
    if (dragMode_ != DragMode::Annotate)
        showMouseMagnifier(position);
    maximumPressDistance_ = std::max(
        maximumPressDistance_, std::hypot(position.x() - press_.x(), position.y() - press_.y()));
    if (dragMode_ == DragMode::Annotate)
        updateAnnotation(position);
    else if (dragMode_ == DragMode::Create)
    {
        if (pressedWindow_ < 0 || maximumPressDistance_ > windowClickDistance)
            setSelection(
                ratio ? ratioCreatedSelection(press_, position, *ratio,
                                              frame_.display.devicePixelRatio, frame_.pixels.size())
                      : normalizedPixelSelection(press_, position, frame_.display.devicePixelRatio,
                                                 frame_.pixels.size()));
    }
    else if (dragMode_ == DragMode::Move)
        setSelection(movedPixelSelection(initialSelection_, position - press_,
                                         frame_.display.devicePixelRatio, frame_.pixels.size()));
    else if (dragMode_ == DragMode::Resize)
        setSelection(
            ratio
                ? ratioResizedSelection(initialSelection_, resizeEdges_, position - press_, *ratio,
                                        frame_.display.devicePixelRatio, frame_.pixels.size())
                : resizedPixelSelection(initialSelection_, resizeEdges_, position - press_,
                                        frame_.display.devicePixelRatio, frame_.pixels.size()));
    updateCursor(position);
    updateToolbar();
    update();
}
void SelectionOverlay::updateHover(QPointF position)
{
    const int index =
        selection_.isEmpty() && dragMode_ == DragMode::None && !finished_ && !saveDialogOpen_
            ? windowAt(windows_, position)
            : -1;
    const QRect pixels =
        index >= 0 ? windowPixelSelection(windows_[index], frame_.display.devicePixelRatio,
                                          frame_.pixels.size())
                   : QRect();
    if (pixels != hoveredWindowPixels_)
    {
        hoveredWindowPixels_ = pixels;
        update();
    }
}
void SelectionOverlay::leaveEvent(QEvent*)
{
    hoveredWindowPixels_ = {};
    update();
}
void SelectionOverlay::paintEvent(QPaintEvent*)
{
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // QImage 带目标屏 DPR，以原像素绘制，不生成缩放副本。
        painter.drawImage(QPointF(0, 0), frame_.pixels);
        const qreal scale = frame_.display.devicePixelRatio;
        painter.save();
        painter.scale(1 / scale, 1 / scale);
        paintAnnotations(painter, annotations(), frame_.pixels, &renderCache_);
        if (draft_)
            paintAnnotation(painter, *draft_, frame_.pixels);
        painter.restore();
        paintWatermarkOverlay(painter);
        const QRectF region = logicalSelection();
        QPainterPath shade;
        shade.addRect(rect());
        QPainterPath outline;
        if (!selection_.isEmpty())
        {
            if (!scrollReady_ && activeShape_ >= 0 && activeShape_ < 6)
            {
                outline = captureShapePath(static_cast<CaptureShape>(activeShape_), region);
                shade = shade.subtracted(outline);
            }
            else
                shade.addRect(region);
        }
        painter.setRenderHint(QPainter::Antialiasing, activeShape_ >= 0);
        painter.fillPath(shade, QColor(0, 0, 0, 75));
        if (!scrollReady_ && !selection_.isEmpty() && activeShape_ >= 6)
        {
            const QSize target(std::max(1, qRound(region.width())),
                               std::max(1, qRound(region.height())));
            const QSize size = target.width() > 1024 || target.height() > 1024
                                   ? target.scaled(1024, 1024, Qt::KeepAspectRatio)
                                   : target;
            if (size != templatePreviewSize_)
            {
                templatePreviewSize_ = size;
                templatePreview_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
                templatePreview_.fill(QColor(0, 0, 0, 75));
                QPainter preview(&templatePreview_);
                preview.setRenderHint(QPainter::SmoothPixmapTransform);
                preview.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                preview.drawImage(templatePreview_.rect(), templateMasks_[activeShape_ - 6]);
            }
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(region, templatePreview_);
        }
        if (!hoveredWindowPixels_.isEmpty())
        {
            const QRectF hover(hoveredWindowPixels_.x() / scale, hoveredWindowPixels_.y() / scale,
                               hoveredWindowPixels_.width() / scale,
                               hoveredWindowPixels_.height() / scale);
            painter.fillRect(hover, QColor(60, 170, 255, 55));
            painter.setPen(QPen(QColor(90, 200, 255), 2, Qt::DashLine));
            painter.drawRect(hover);
        }
        painter.setPen(QPen(Qt::white, 1.0 / scale));
        if (!selection_.isEmpty())
        {
            if (!scrollReady_ && activeShape_ >= 0 && activeShape_ < 6)
                painter.drawPath(outline);
            else
                painter.drawRect(region);
            if (textMode_)
            {
                QPainterPath highlight;
                highlight.setFillRule(Qt::WindingFill);
                for (const auto& box : ocrHighlightBoxes())
                    highlight.addRect(QRectF((box.x() + selection_.x()) / scale,
                                             (box.y() + selection_.y()) / scale,
                                             box.width() / scale, box.height() / scale));
                QColor color = palette().color(QPalette::Highlight);
                color.setAlpha(77);
                painter.save();
                painter.setClipRect(region);
                painter.setBrush(color);
                painter.setPen(QPen(palette().color(QPalette::Highlight), 1 / scale));
                painter.drawPath(highlight.simplified());
                painter.restore();
            }
            if (!scrollReady_ && !textMode_ && dragMode_ != DragMode::Create)
            {
                painter.setBrush(QColor(245, 130, 35));
                const qreal x[] = {region.left(), region.center().x(), region.right()};
                const qreal y[] = {region.top(), region.center().y(), region.bottom()};
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        if (row != 1 || column != 1)
                            painter.drawRect(QRectF(x[column] - 3, y[row] - 3, 6, 6));
            }
        }
        paintAnnotationSelection(painter);
        paintWatermarkSelection(painter);
        const QString hint =
            scrollReady_ ? QStringLiteral("回车开始采集 · Esc 取消 · 尽量避开固定侧栏")
            : textMode_ ? QStringLiteral("拖选文字 · 双击选词 · Esc 退出取字")
            : watermarkToolActive_
                ? (watermark_ && (watermark_->layout == WatermarkLayout::DiagonalForward ||
                                  watermark_->layout == WatermarkLayout::DiagonalReverse)
                       ? QStringLiteral("平铺水印：参数条调缩放 / 透明度 · Esc 退出")
                       : QStringLiteral("拖动水印 · 拖圆点旋转 · 拖角缩放 · Esc 退出"))
            : selectMode_ ? QStringLiteral("点击选中标注 · 拖动移动 · 拖圆点旋转 · Esc 取消")
            : activeTool_ ? QStringLiteral("拖拽标注 · 再点工具调整选区 · Esc 取消")
                          : QStringLiteral("单击吸附 · 拖拽框选 · Esc 取消");
        const QString text = selectionSizeText() + QStringLiteral("   ") + hint;
        const QRect box(16, 16, std::max(0, std::min(width() - 32, 580)), 36);
        painter.fillRect(box, QColor(20, 20, 20, 220));
        painter.setPen(Qt::white);
        painter.drawText(box.adjusted(12, 0, -12, 0), Qt::AlignVCenter, text);
        if (!snappingNotice_.isEmpty())
        {
            const QRect notice = box.translated(0, 40);
            painter.fillRect(notice, QColor(20, 20, 20, 220));
            painter.drawText(notice.adjusted(12, 0, -12, 0), Qt::AlignVCenter, snappingNotice_);
        }
        paintMagnifier(painter);
    }
    if (!painted_)
    {
        painted_ = true;
        emit firstPaintCompleted(monotonicNs());
    }
}
void SelectionOverlay::mousePressEvent(QMouseEvent* event)
{
    if (scrollReady_)
        return;
    if (ocrDialog_ || finished_ || saveDialogOpen_ || event->button() != Qt::LeftButton)
        return;
    hideMagnifier();
    if (watermarkToolActive_)
    {
        setFocus(Qt::MouseFocusReason);
        if (watermark_)
        {
            if (watermarkRotationHandleHit(event->position()))
            {
                beginWatermarkGesture(int(WatermarkGesture::Rotate), event->position());
                event->accept();
                return;
            }
            if (watermarkScaleHandleHit(event->position()))
            {
                beginWatermarkGesture(int(WatermarkGesture::Scale), event->position());
                event->accept();
                return;
            }
            if (watermarkHit(event->position()))
            {
                beginWatermarkGesture(int(WatermarkGesture::Move), event->position());
                event->accept();
                return;
            }
        }
        if (logicalSelection().contains(event->position()))
        {
            if (!watermark_)
                applyWatermarkPreset();
            else
            {
                watermarkSelected_ = false;
                updateToolbar();
                update();
            }
            event->accept();
            return;
        }
    }
    if (textMode_)
    {
        setFocus(Qt::MouseFocusReason);
        ocrAnchor_ = textAt(event->position(), false);
        selectingText_ = ocrAnchor_.valid();
        ocrSelection_ = text_layout::selectionRange(ocrResult_.lines, ocrAnchor_, ocrAnchor_);
        update();
        return;
    }
    const bool textWasEditing = textEditor_ && textEditor_->isVisible();
    const qsizetype textCountBefore = annotations().size();
    finishText(true);
    const bool textCommitted = textWasEditing && annotations().size() > textCountBefore;
    setFocus(Qt::MouseFocusReason);
    lastAnnotationSelected_ = false;
    press_ = event->position();
    lastDragPosition_ = press_;
    shiftRatio_ = event->modifiers().testFlag(Qt::ShiftModifier);
    if (textCommitted)
    {
        // 点画布外提交文本时保留当前文本框选中，显示选框供直接拖动 / 旋转；
        // 下一次点击再开始新的文本或标注，避免提交动作顺手把选框清掉。
        updateToolbar();
        updateCursor(press_);
        update();
        event->accept();
        return;
    }
    if (selectMode_)
    {
        beginAnnotationGesture(press_, event->modifiers());
        event->accept();
        return;
    }
    if (activeTool_ && selectedAnnotation_ >= 0)
    {
        if (rotationHandleHit(press_))
        {
            startAnnotationGesture(AnnotationGesture::Rotate, selectedAnnotation_, press_);
            event->accept();
            return;
        }
        if (annotationHitTest(annotations()[selectedAnnotation_], physicalPoint(press_), 4))
        {
            pendingSelectedGesture_ = true;
            pendingSelectedPress_ = press_;
            event->accept();
            return;
        }
        clearAnnotationSelection();
    }
    if (activeTool_ && !selection_.isEmpty())
    {
        if (*activeTool_ == AnnotationType::Text)
            startText(press_);
        else
        {
            draft_ = Annotation{*activeTool_,          style_, physicalPoint(press_),
                                physicalPoint(press_), {},     {}};
            if (*activeTool_ == AnnotationType::Freehand)
                appendFreehandPoint(*draft_, draft_->first);
            dragMode_ = DragMode::Annotate;
            updateToolbar();
            update();
        }
        return;
    }
    initialSelection_ = selection_;
    maximumPressDistance_ = 0;
    // 选定比例后已有选区，选区外单击仍需能吸附到窗口内接矩形。
    pressedWindow_ = selection_.isEmpty() || activeRatio_ ? windowAt(windows_, press_) : -1;
    hoveredWindowPixels_ = {};
    resizeEdges_ = edgesAt(press_);
    if (resizeEdges_)
        dragMode_ = DragMode::Resize;
    else if (!selection_.isEmpty() && logicalSelection().contains(press_))
        dragMode_ = DragMode::Move;
    else
    {
        dragMode_ = DragMode::Create;
        setSelection({});
    }
    updateCursor(press_);
    showMouseMagnifier(press_);
    updateToolbar();
    update();
}
void SelectionOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (scrollReady_)
        return;
    if (ocrDialog_ || finished_ || saveDialogOpen_)
        return;
    if (watermarkGesture_ != WatermarkGesture::None)
    {
        updateWatermarkGesture(event->position(), event->modifiers());
        return;
    }
    if (textMode_)
    {
        if (selectingText_)
            selectOcrTo(event->position());
        updateCursor(event->position());
        return;
    }
    if (pendingSelectedGesture_)
    {
        if (std::hypot(event->position().x() - pendingSelectedPress_.x(),
                       event->position().y() - pendingSelectedPress_.y()) < 2)
            return;
        pendingSelectedGesture_ = false;
        if (!beginSelectedAnnotationGesture(pendingSelectedPress_))
            return;
    }
    if (annotationGesture_ != AnnotationGesture::None)
    {
        updateAnnotationGesture(event->position(), event->modifiers());
        return;
    }
    if (dragMode_ != DragMode::None)
    {
        shiftRatio_ = event->modifiers().testFlag(Qt::ShiftModifier);
        dragTo(event->position());
    }
    else
    {
        updateHover(event->position());
        updateCursor(event->position());
    }
}
void SelectionOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (scrollReady_)
        return;
    if (ocrDialog_)
        return;
    if (watermarkGesture_ != WatermarkGesture::None && event->button() == Qt::LeftButton)
    {
        updateWatermarkGesture(event->position(), event->modifiers());
        finishWatermarkGesture();
        event->accept();
        return;
    }
    if (textMode_ && !finished_ && !saveDialogOpen_ && event->button() == Qt::LeftButton)
    {
        if (selectingText_)
            selectOcrTo(event->position());
        selectingText_ = false;
        return;
    }
    if (pendingSelectedGesture_)
    {
        pendingSelectedGesture_ = false;
        if (activeTool_ == AnnotationType::Text)
        {
            selectedAnnotation_ = -1;
            startText(pendingSelectedPress_);
        }
        else
        {
            clearAnnotationSelection();
            updateToolbar();
            updateCursor(mapFromGlobal(QCursor::pos()));
            update();
        }
        event->accept();
        return;
    }
    if (annotationGesture_ != AnnotationGesture::None)
    {
        finishAnnotationGesture();
        event->accept();
        return;
    }
    if (finished_ || saveDialogOpen_ || dragMode_ == DragMode::None ||
        event->button() != Qt::LeftButton)
        return;
    shiftRatio_ = event->modifiers().testFlag(Qt::ShiftModifier);
    dragTo(event->position());
    if (dragMode_ == DragMode::Create && pressedWindow_ >= 0 &&
        maximumPressDistance_ <= windowClickDistance)
    {
        const QRect window = windowPixelSelection(
            windows_[pressedWindow_], frame_.display.devicePixelRatio, frame_.pixels.size());
        setSelection(activeRatio_ ? largestInscribedRatioRect(window, *activeRatio_) : window);
    }
    if (dragMode_ == DragMode::Annotate && draft_)
    {
        const bool widthTool = annotationUsesLineWidth(draft_->type);
        if (history_.add(std::move(*draft_)))
        {
            markDirty();
            lastAnnotationSelected_ = widthTool;
            selectedAnnotation_ = int(annotations().size()) - 1;
        }
        else
            lastAnnotationSelected_ = false;
        draft_.reset();
    }
    dragMode_ = DragMode::None;
    shiftRatio_ = false;
    hideMagnifier();
    updateHover(event->position());
    updateCursor(event->position());
    updateToolbar();
    update();
}
void SelectionOverlay::keyPressEvent(QKeyEvent* event)
{
    if (scrollReady_)
    {
        if (event->key() == Qt::Key_Escape)
            complete(scrollCancelledOutcome);
        else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            startScrollCapture();
        event->accept();
        return;
    }
    if (ocrDialog_)
    {
        if (event->key() == Qt::Key_Escape)
            ocrDialog_->reject();
        event->accept();
    }
    else if (finished_ || saveDialogOpen_)
        event->accept();
    else if (watermarkToolActive_)
    {
        if (event->key() == Qt::Key_Escape)
        {
            watermarkToolActive_ = false;
            watermarkSelected_ = false;
            watermarkGesture_ = WatermarkGesture::None;
            updateToolbar();
            updateCursor(mapFromGlobal(QCursor::pos()));
            update();
        }
        else if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
            removeWatermark();
        event->accept();
    }
    else if (textEditor_ && textEditor_->isVisible())
    {
        // 焦点层即使有事件泄漏到覆盖层，也不能取消会话或撤销已提交标注。
        if (event->key() == Qt::Key_Escape && !textEditor_->isComposing())
            finishText(false);
        event->accept();
    }
    else if (event->key() == Qt::Key_Shift &&
             (dragMode_ == DragMode::Create || dragMode_ == DragMode::Resize))
    {
        shiftRatio_ = true;
        dragTo(lastDragPosition_);
        event->accept();
    }
    else if (event->key() == Qt::Key_Escape && selectMode_ && selectedAnnotation_ >= 0)
    {
        clearAnnotationSelection();
        updateToolbar();
        updateCursor(mapFromGlobal(QCursor::pos()));
        update();
        event->accept();
    }
    else if (event->key() == Qt::Key_Escape)
    {
        if (ocrDialog_)
            ocrDialog_->reject();
        else if (textMode_)
            exitTextMode();
        else
            complete(cancelledSessionOutcome);
    }
    else if (event->matches(QKeySequence::Copy))
        copyShortcut();
    else if (textMode_)
        event->accept();
    else if (event->matches(QKeySequence::Undo))
        undoAnnotation();
    else if (event->matches(QKeySequence::Redo))
        redoAnnotation();
    else if (nudgeSelection(event))
        event->accept();
    else
        QWidget::keyPressEvent(event);
}
void SelectionOverlay::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Shift && !event->isAutoRepeat() && !finished_ && !saveDialogOpen_ &&
        (dragMode_ == DragMode::Create || dragMode_ == DragMode::Resize))
    {
        shiftRatio_ = false;
        dragTo(lastDragPosition_);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}
QString SelectionOverlay::selectionSizeText() const
{
    const bool dragging = dragMode_ == DragMode::Create || dragMode_ == DragMode::Resize;
    const auto ratio = dragging ? dragRatio() : activeRatio_;
    const QString size =
        QStringLiteral("%1 × %2 像素").arg(selection_.width()).arg(selection_.height());
    if (activeShape_ >= 0)
        return (dragging && shiftRatio_ ? QStringLiteral("⇧ ") : QString()) + shapeLabel() +
               QStringLiteral(" · ") + size;
    if (!ratio)
        return size;
    return (dragging && shiftRatio_ ? QStringLiteral("⇧ ") : QString()) +
           formatAspectRatio(*ratio) + QStringLiteral(" · ") + size;
}
bool SelectionOverlay::nudgeSelection(QKeyEvent* event)
{
    if (textMode_ || watermarkToolActive_ || !hasFocus() || activeTool_ || selectMode_ ||
        selection_.isEmpty() || dragMode_ != DragMode::None)
        return false;
    const auto modifiers = event->modifiers();
    if (modifiers & ~(Qt::ShiftModifier | Qt::AltModifier) ||
        modifiers == (Qt::ShiftModifier | Qt::AltModifier))
        return false;
    SelectionEdge edge;
    switch (event->key())
    {
    case Qt::Key_Left:
        edge = SelectionEdge::Left;
        break;
    case Qt::Key_Up:
        edge = SelectionEdge::Top;
        break;
    case Qt::Key_Right:
        edge = SelectionEdge::Right;
        break;
    case Qt::Key_Down:
        edge = SelectionEdge::Bottom;
        break;
    default:
        return false;
    }
    const auto mode = modifiers == Qt::ShiftModifier ? SelectionNudge::Expand
                      : modifiers == Qt::AltModifier ? SelectionNudge::Shrink
                                                     : SelectionNudge::Move;
    setSelection(
        activeRatio_ && mode != SelectionNudge::Move
            ? ratioNudgedSelection(selection_, edge, mode, *activeRatio_, frame_.pixels.size())
            : nudgedPixelSelection(selection_, edge, mode, frame_.pixels.size()));
    updateToolbar();
    showMagnifier(selectionNudgeAnchor(selection_, edge, mode), true);
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
    return true;
}
void SelectionOverlay::showMagnifier(QPoint anchor, bool keyboard)
{
    if (scrollReady_ || textMode_ || finished_ || saveDialogOpen_ || activeTool_)
        return;
    const QRect sampleRect = magnifierSamplingRect(anchor, frame_.pixels.size());
    if (sampleRect.isEmpty())
        return;
    if (!magnifierVisible_ || anchor != magnifierAnchor_)
    {
        magnifierAnchor_ = anchor;
        magnifierSampleRect_ = sampleRect;
        magnifierSample_ = frame_.pixels.copy(sampleRect);
        magnifierSample_.setDevicePixelRatio(1);
    }
    magnifierVisible_ = true;
    if (keyboard)
        magnifierTimeout_.start(700);
    else
        magnifierTimeout_.stop();
    update();
}
void SelectionOverlay::showMouseMagnifier(QPointF position)
{
    const QPointF source = physicalPoint(position);
    // 沿用物理坐标路径，最近像素取整后夹在实际像素索引内；不累计逻辑步长。
    showMagnifier({std::clamp(qRound(source.x()), 0, frame_.pixels.width() - 1),
                   std::clamp(qRound(source.y()), 0, frame_.pixels.height() - 1)},
                  false);
}
void SelectionOverlay::hideMagnifier()
{
    magnifierTimeout_.stop();
    magnifierVisible_ = false;
    magnifierAnchor_ = {};
    magnifierSampleRect_ = {};
    magnifierSample_ = {};
    update();
}
QString SelectionOverlay::magnifierPositionText() const
{
    return magnifierVisible_
               ? QStringLiteral("%1, %2").arg(magnifierAnchor_.x()).arg(magnifierAnchor_.y())
               : QString();
}
QRect SelectionOverlay::magnifierRect() const
{
    if (!magnifierVisible_)
        return {};
    const QSize panel(magnifierSample_.width() * magnifierCellSize + 8,
                      magnifierSample_.height() * magnifierCellSize + 32);
    const QPointF anchor = QPointF(magnifierAnchor_) / frame_.display.devicePixelRatio;
    QRect obstacle;
    for (auto* widget :
         {toolbar_, toolBar_, optionsBar_, ratioBar_, shapeBar_, static_cast<QWidget*>(status_)})
        if (widget && widget->isVisible())
            obstacle = obstacle.united(widget->geometry());
    return placedMagnifier(anchor, panel, availableRect(), obstacle);
}
void SelectionOverlay::paintMagnifier(QPainter& painter)
{
    if (!magnifierVisible_)
        return;
    const QRect panel = magnifierRect();
    const qreal dpr = devicePixelRatioF();
    painter.save();
    // 在目标设备像素坐标中整数对齐；每源像素在 1 / 1.5 / 2 倍为 8 / 12 / 16 像素。
    painter.scale(1 / dpr, 1 / dpr);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const QPoint origin(qRound(panel.x() * dpr), qRound(panel.y() * dpr));
    const int cell = qRound(magnifierCellSize * dpr);
    const int padding = qRound(4 * dpr);
    const QRect grid(origin + QPoint(padding, padding), magnifierSample_.size() * cell);
    const QRect background(origin, QSize(grid.width() + padding * 2, qRound(panel.height() * dpr)));
    painter.fillRect(background, QColor(20, 20, 20));
    painter.drawImage(grid, magnifierSample_, magnifierSample_.rect());
    const QPoint offset = magnifierAnchor_ - magnifierSampleRect_.topLeft();
    const QRectF marker(grid.topLeft() + offset * cell, QSize(cell, cell));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::black, 3 * dpr));
    painter.drawRect(marker.adjusted(1.5 * dpr, 1.5 * dpr, -1.5 * dpr, -1.5 * dpr));
    painter.setPen(QPen(Qt::white, dpr));
    painter.drawRect(marker.adjusted(1.5 * dpr, 1.5 * dpr, -1.5 * dpr, -1.5 * dpr));
    painter.setPen(Qt::white);
    QFont textFont = painter.font();
    textFont.setPixelSize(qRound(12 * dpr));
    painter.setFont(textFont);
    painter.drawText(QRect(grid.x(), grid.y() + grid.height(), grid.width(), qRound(24 * dpr)),
                     Qt::AlignCenter, magnifierPositionText());
    painter.restore();
}
void SelectionOverlay::closeEvent(QCloseEvent* event)
{
    event->accept();
    if (!finished_)
    {
        hoveredWindowPixels_ = {};
        finished_ = true;
        exitTextMode();
        hideMagnifier();
        discardAnnotations();
        emit finished({}, scrollReady_ ? scrollCancelledOutcome : cancelledSessionOutcome);
    }
}
void SelectionOverlay::complete(int outcome)
{
    if (finished_)
        return;
    scrollReady_ = false;
    finished_ = true;
    exitTextMode();
    hideMagnifier();
    hoveredWindowPixels_ = {};
    discardAnnotations();
    hide();
    emit finished(outcome == copiedSessionOutcome || outcome == pinnedSessionOutcome ? selection()
                                                                                     : QRect(),
                  outcome);
}
QPointF SelectionOverlay::physicalPoint(QPointF position) const
{
    const qreal scale = frame_.display.devicePixelRatio;
    return {std::clamp(position.x() * scale, qreal(0), qreal(frame_.pixels.width())),
            std::clamp(position.y() * scale, qreal(0), qreal(frame_.pixels.height()))};
}
void SelectionOverlay::activateTool(AnnotationType type, bool toggle)
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    watermarkToolActive_ = false;
    watermarkSelected_ = false;
    watermarkGesture_ = WatermarkGesture::None;
    exitTextMode();
    finishText(true);
    clearAnnotationSelection();
    selectMode_ = false;
    lastAnnotationSelected_ = false;
    widthWheel_.reset();
    toolTypes_[annotationToolSlot(type)] = type;
    activeTool_ =
        toggle && activeTool_ == type ? std::nullopt : std::optional<AnnotationType>(type);
    hideMagnifier();
    if (activeTool_ == AnnotationType::Mosaic && !mosaicNoticeShown_)
    {
        mosaicNoticeShown_ = true;
        showStatus(QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖"), true);
    }
    setFocus(Qt::OtherFocusReason);
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::toggleSelectMode()
{
    if (ocrDialog_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None)
        return;
    watermarkToolActive_ = false;
    watermarkSelected_ = false;
    watermarkGesture_ = WatermarkGesture::None;
    exitTextMode();
    finishText(true);
    activeTool_.reset();
    lastAnnotationSelected_ = false;
    widthWheel_.reset();
    selectMode_ = !selectMode_;
    if (selectMode_)
    {
        hideMagnifier();
        showStatus(QStringLiteral("选择标注：拖动移动，拖动上方圆点旋转；Esc 取消选择"), true);
    }
    else
        clearAnnotationSelection();
    setFocus(Qt::OtherFocusReason);
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::toggleWatermarkTool()
{
    if (finished_ || saveDialogOpen_ || ocrDialog_ || dragMode_ != DragMode::None ||
        !actions_.watermarks)
        return;
    if (!actions_.watermarks->currentPreset())
    {
        showStatus(QStringLiteral("请先在设置 → 水印中创建预设。"), true);
        return;
    }
    exitTextMode();
    finishText(true);
    clearAnnotationSelection();
    activeTool_.reset();
    selectMode_ = false;
    lastAnnotationSelected_ = false;
    widthWheel_.reset();
    watermarkToolActive_ = !watermarkToolActive_;
    if (watermarkToolActive_ && !watermark_ && !selection_.isEmpty())
        applyWatermarkPreset();
    if (!watermarkToolActive_)
    {
        watermarkSelected_ = false;
        watermarkGesture_ = WatermarkGesture::None;
    }
    setFocus(Qt::OtherFocusReason);
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::applyWatermarkPreset()
{
    if (selection_.isEmpty() || !actions_.watermarks)
        return;
    const auto* preset = actions_.watermarks->currentPreset();
    const auto* asset = preset ? actions_.watermarks->asset(preset->assetId) : nullptr;
    if (!preset || !asset)
        return;
    QString error;
    const QImage assetImage = actions_.watermarks->assetImage(asset->id, &error);
    if (asset->kind == WatermarkKind::Image && assetImage.isNull())
    {
        showStatus(error.isEmpty() ? QStringLiteral("水印图片不可用。") : error, false);
        watermark_.reset();
        watermarkSelected_ = false;
        updateToolbar();
        update();
        return;
    }
    WatermarkInstance instance;
    instance.presetId = preset->id;
    instance.layout = preset->layout;
    instance.opacity = preset->opacity;
    instance.scale = preset->scale;
    instance.visible = true;
    instance.normalizedCenter = watermarkInitialCenter(
        preset->layout, watermarkBaseSize(*asset, assetImage), selection_.size());
    watermark_ = instance;
    watermarkSelected_ = true;
    markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::removeWatermark()
{
    if (!watermark_)
        return;
    watermark_.reset();
    watermarkSelected_ = false;
    watermarkGesture_ = WatermarkGesture::None;
    markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::resetWatermark()
{
    if (!watermark_)
    {
        applyWatermarkPreset();
        return;
    }
    applyWatermarkPreset();
}
void SelectionOverlay::syncWatermarkBar()
{
    if (!watermarkToolButton_ || !watermarkBar_ || !actions_.watermarks)
        return;
    const bool hasPreset = actions_.watermarks->currentPreset() != nullptr;
    watermarkToolButton_->setEnabled(hasPreset);
    watermarkToolButton_->setChecked(watermarkToolActive_);
    watermarkToolButton_->setToolTip(
        hasPreset ? QStringLiteral("水印：拖动水印，拖动圆点旋转，拖动角缩放")
                  : QStringLiteral("请先在设置 → 水印中创建预设"));
    watermarkToolButton_->setAccessibleName(watermarkToolButton_->toolTip());
    const bool showBar = watermarkToolActive_ && hasPreset && !selection_.isEmpty() && !textMode_ &&
                         !finished_ && !saveDialogOpen_ && !ocrDialog_;
    watermarkBar_->setVisible(showBar);
    if (!showBar)
        return;
    {
        const QSignalBlocker blocker(watermarkPresetCombo_);
        watermarkPresetCombo_->clear();
        const auto& preferences = actions_.watermarks->preferences();
        for (const auto& preset : preferences.presets)
            watermarkPresetCombo_->addItem(preset.name, preset.id);
        const QString current = actions_.watermarks->currentPresetId();
        const int index = watermarkPresetCombo_->findData(current);
        if (index >= 0)
            watermarkPresetCombo_->setCurrentIndex(index);
    }
    if (watermark_)
    {
        const QSignalBlocker opacityBlocker(watermarkOpacitySpin_);
        const QSignalBlocker scaleBlocker(watermarkScaleSpin_);
        watermarkOpacitySpin_->setValue(watermark_->opacity);
        watermarkScaleSpin_->setValue(watermark_->scale);
        watermarkOpacitySpin_->setEnabled(true);
        watermarkScaleSpin_->setEnabled(true);
        watermarkVisibilityButton_->setText(watermark_->visible ? QStringLiteral("隐")
                                                                : QStringLiteral("显"));
        watermarkVisibilityButton_->setToolTip(watermark_->visible ? QStringLiteral("隐藏水印")
                                                                   : QStringLiteral("显示水印"));
        watermarkResetButton_->setEnabled(true);
        watermarkRemoveButton_->setEnabled(true);
    }
    else
    {
        watermarkOpacitySpin_->setEnabled(false);
        watermarkScaleSpin_->setEnabled(false);
        watermarkVisibilityButton_->setText(QStringLiteral("隐"));
        watermarkVisibilityButton_->setToolTip(QStringLiteral("隐藏水印"));
        watermarkResetButton_->setEnabled(hasPreset);
        watermarkRemoveButton_->setEnabled(false);
    }
    watermarkBar_->adjustSize();
}
QPointF SelectionOverlay::watermarkOutputPoint(QPointF position) const
{
    return physicalPoint(position) - QPointF(selection_.topLeft());
}
QRectF SelectionOverlay::watermarkLogicalBounds() const
{
    if (!watermark_ || !actions_.watermarks || selection_.isEmpty() ||
        watermark_->layout == WatermarkLayout::DiagonalForward ||
        watermark_->layout == WatermarkLayout::DiagonalReverse)
        return {};
    const auto* preset = actions_.watermarks->preset(watermark_->presetId);
    const auto* asset = preset ? actions_.watermarks->asset(preset->assetId) : nullptr;
    if (!preset || !asset)
        return {};
    QString error;
    const QImage assetImage = actions_.watermarks->assetImage(asset->id, &error);
    const QSizeF base = watermarkBaseSize(*asset, assetImage);
    if (base.isEmpty())
        return {};
    const qreal dpr = frame_.display.devicePixelRatio;
    const QSizeF size(base.width() * watermark_->scale / 100.0,
                      base.height() * watermark_->scale / 100.0);
    const QPointF center =
        QPointF(selection_.x() + watermark_->normalizedCenter.x() * selection_.width(),
                selection_.y() + watermark_->normalizedCenter.y() * selection_.height()) /
        dpr;
    const QRectF local(-size.width() / (2 * dpr), -size.height() / (2 * dpr), size.width() / dpr,
                       size.height() / dpr);
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(watermark_->rotation);
    return transform.mapRect(local);
}
QPointF SelectionOverlay::watermarkRotationHandleLogical() const
{
    const QRectF bounds = watermarkLogicalBounds();
    if (bounds.isEmpty())
        return {};
    const QPointF center = bounds.center();
    const QPointF top(bounds.center().x(), bounds.top());
    const QPointF direction = top - center;
    const qreal length = std::hypot(direction.x(), direction.y());
    if (length <= 0)
        return top;
    return top + direction / length * 24;
}
QPointF SelectionOverlay::watermarkScaleHandleLogical() const
{
    const QRectF bounds = watermarkLogicalBounds();
    if (bounds.isEmpty())
        return {};
    return bounds.bottomRight();
}
bool SelectionOverlay::watermarkHit(QPointF position) const
{
    if (!watermark_ || !watermark_->visible ||
        watermark_->layout == WatermarkLayout::DiagonalForward ||
        watermark_->layout == WatermarkLayout::DiagonalReverse)
        return false;
    const QRectF bounds = watermarkLogicalBounds();
    return !bounds.isEmpty() && bounds.contains(position);
}
bool SelectionOverlay::watermarkRotationHandleHit(QPointF position) const
{
    const QPointF handle = watermarkRotationHandleLogical();
    return !handle.isNull() &&
           std::hypot(position.x() - handle.x(), position.y() - handle.y()) <= 10;
}
bool SelectionOverlay::watermarkScaleHandleHit(QPointF position) const
{
    const QPointF handle = watermarkScaleHandleLogical();
    return !handle.isNull() &&
           std::hypot(position.x() - handle.x(), position.y() - handle.y()) <= 10;
}
void SelectionOverlay::beginWatermarkGesture(int gesture, QPointF position)
{
    if (!watermark_)
        return;
    watermarkSelected_ = true;
    watermarkGesture_ = WatermarkGesture(gesture);
    watermarkBeforeGesture_ = *watermark_;
    watermarkPressOutput_ = watermarkOutputPoint(position);
    watermarkGestureCenter_ =
        QPointF(watermarkBeforeGesture_.normalizedCenter.x() * selection_.width(),
                watermarkBeforeGesture_.normalizedCenter.y() * selection_.height());
    watermarkGestureStartDistance_ =
        std::hypot(watermarkPressOutput_.x() - watermarkGestureCenter_.x(),
                   watermarkPressOutput_.y() - watermarkGestureCenter_.y());
    watermarkGestureStartAngle_ =
        std::atan2(watermarkPressOutput_.y() - watermarkGestureCenter_.y(),
                   watermarkPressOutput_.x() - watermarkGestureCenter_.x()) *
        180.0 / 3.14159265358979323846;
    updateToolbar();
    update();
}
void SelectionOverlay::updateWatermarkGesture(QPointF position, Qt::KeyboardModifiers modifiers)
{
    if (!watermark_ || watermarkGesture_ == WatermarkGesture::None)
        return;
    const QPointF output = watermarkOutputPoint(position);
    if (watermarkGesture_ == WatermarkGesture::Move)
    {
        QPointF center = watermarkGestureCenter_ + output - watermarkPressOutput_;
        watermark_->normalizedCenter =
            QPointF(std::clamp(center.x() / std::max(1, selection_.width()), 0.0, 1.0),
                    std::clamp(center.y() / std::max(1, selection_.height()), 0.0, 1.0));
    }
    else if (watermarkGesture_ == WatermarkGesture::Rotate)
    {
        const qreal angle = std::atan2(output.y() - watermarkGestureCenter_.y(),
                                       output.x() - watermarkGestureCenter_.x()) *
                            180.0 / 3.14159265358979323846;
        qreal rotation = watermarkBeforeGesture_.rotation + angle - watermarkGestureStartAngle_;
        if (modifiers.testFlag(Qt::ShiftModifier))
            rotation = snapAnnotationRotation(rotation);
        watermark_->rotation = rotation;
    }
    else
    {
        const qreal distance = std::hypot(output.x() - watermarkGestureCenter_.x(),
                                          output.y() - watermarkGestureCenter_.y());
        if (watermarkGestureStartDistance_ > 1)
            watermark_->scale = std::clamp(
                qRound(watermarkBeforeGesture_.scale * distance / watermarkGestureStartDistance_),
                watermarkMinimumScale, watermarkMaximumScale);
    }
    syncWatermarkBar();
    update();
}
void SelectionOverlay::finishWatermarkGesture()
{
    if (watermarkGesture_ == WatermarkGesture::None)
        return;
    watermarkGesture_ = WatermarkGesture::None;
    markDirty();
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::paintWatermarkOverlay(QPainter& painter) const
{
    if (!watermark_ || !watermark_->visible || !actions_.watermarks || selection_.isEmpty())
        return;
    const auto* preset = actions_.watermarks->preset(watermark_->presetId);
    const auto* asset = preset ? actions_.watermarks->asset(preset->assetId) : nullptr;
    if (!preset || !asset)
        return;
    QString error;
    const QImage assetImage = actions_.watermarks->assetImage(asset->id, &error);
    if (asset->kind == WatermarkKind::Image && assetImage.isNull())
        return;
    const qreal dpr = frame_.display.devicePixelRatio;
    painter.save();
    painter.setClipRect(logicalSelection());
    painter.translate(selection_.x() / dpr, selection_.y() / dpr);
    painter.scale(1 / dpr, 1 / dpr);
    paintWatermark(painter, selection_.size(), *asset, assetImage, *watermark_);
    painter.restore();
}
void SelectionOverlay::paintWatermarkSelection(QPainter& painter) const
{
    if (!watermarkToolActive_ || !watermarkSelected_ || !watermark_ || textMode_ || finished_)
        return;
    if (watermark_->layout == WatermarkLayout::DiagonalForward ||
        watermark_->layout == WatermarkLayout::DiagonalReverse)
        return;
    const QPolygonF polygon = QPolygonF(watermarkLogicalBounds());
    if (polygon.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(245, 130, 35), 1.4, Qt::DashLine);
    pen.setDashPattern({4, 3});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(polygon);
    const QPointF rotation = watermarkRotationHandleLogical();
    painter.drawLine(polygon.boundingRect().center(), rotation);
    painter.setPen(QPen(QColor(245, 130, 35), 1.5));
    painter.setBrush(QColor(245, 130, 35));
    painter.drawEllipse(rotation, 4, 4);
    painter.drawRect(QRectF(watermarkScaleHandleLogical() - QPointF(4, 4), QSizeF(8, 8)));
    painter.restore();
}
int SelectionOverlay::annotationAt(QPointF position) const
{
    if (selection_.isEmpty())
        return -1;
    const QPointF physical = physicalPoint(position);
    const qreal tolerance = 4;
    const auto& items = annotations();
    for (qsizetype index = items.size() - 1; index >= 0; --index)
        if (annotationHitTest(items[index], physical, tolerance))
            return int(index);
    return -1;
}
bool SelectionOverlay::rotationHandleHit(QPointF position) const
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations().size())
        return false;
    const auto& annotation = annotations()[selectedAnnotation_];
    const qreal dpr = frame_.display.devicePixelRatio;
    const QPointF physical = physicalPoint(position);
    return std::hypot(physical.x() - annotationRotationHandle(annotation, 24 * dpr).x(),
                      physical.y() - annotationRotationHandle(annotation, 24 * dpr).y()) <=
           10 / dpr;
}
bool SelectionOverlay::beginAnnotationGesture(QPointF position, Qt::KeyboardModifiers modifiers)
{
    if (!selectMode_ || selection_.isEmpty())
        return false;
    Q_UNUSED(modifiers);
    if (rotationHandleHit(position))
    {
        startAnnotationGesture(AnnotationGesture::Rotate, selectedAnnotation_, position);
        return true;
    }
    const int hit = annotationAt(position);
    if (hit < 0)
    {
        clearAnnotationSelection();
        updateToolbar();
        updateCursor(position);
        update();
        return true;
    }
    startAnnotationGesture(AnnotationGesture::Move, hit, position);
    return true;
}
bool SelectionOverlay::beginSelectedAnnotationGesture(QPointF position)
{
    if (!activeTool_ || selection_.isEmpty() || selectedAnnotation_ < 0 ||
        selectedAnnotation_ >= annotations().size())
        return false;
    if (rotationHandleHit(position))
    {
        startAnnotationGesture(AnnotationGesture::Rotate, selectedAnnotation_, position);
        return true;
    }
    const auto& annotation = annotations()[selectedAnnotation_];
    if (!annotationHitTest(annotation, physicalPoint(position), 4))
        return false;
    startAnnotationGesture(AnnotationGesture::Move, selectedAnnotation_, position);
    return true;
}
void SelectionOverlay::startAnnotationGesture(AnnotationGesture gesture, int index,
                                              QPointF position)
{
    if (index < 0 || index >= annotations().size())
        return;
    selectedAnnotation_ = index;
    const auto& annotation = annotations()[index];
    annotationBeforeEdit_ = annotation;
    annotationPressPhysical_ = physicalPoint(position);
    annotationStartCenter_ = annotationCenter(annotation);
    annotationStartAngle_ = std::atan2(annotationPressPhysical_.y() - annotationStartCenter_.y(),
                                       annotationPressPhysical_.x() - annotationStartCenter_.x()) *
                            180.0 / 3.14159265358979323846;
    annotationGesture_ = gesture;
    history_.beginEdit();
    updateToolbar();
    update();
}
void SelectionOverlay::updateAnnotationGesture(QPointF position, Qt::KeyboardModifiers modifiers)
{
    if (annotationGesture_ == AnnotationGesture::None || selectedAnnotation_ < 0 ||
        selectedAnnotation_ >= annotations().size())
        return;
    const QPointF physical = physicalPoint(position);
    Annotation next = annotationBeforeEdit_;
    if (annotationGesture_ == AnnotationGesture::Move)
        translateAnnotation(next, physical - annotationPressPhysical_);
    else
    {
        const qreal angle = std::atan2(physical.y() - annotationStartCenter_.y(),
                                       physical.x() - annotationStartCenter_.x()) *
                            180.0 / 3.14159265358979323846;
        qreal rotation = annotationBeforeEdit_.rotation + angle - annotationStartAngle_;
        if (modifiers.testFlag(Qt::ShiftModifier))
            rotation = snapAnnotationRotation(rotation);
        setAnnotationRotation(next, rotation);
    }
    if (history_.updateAnnotation(selectedAnnotation_, std::move(next)))
        update();
}
void SelectionOverlay::finishAnnotationGesture()
{
    if (annotationGesture_ == AnnotationGesture::None)
        return;
    if (history_.commitEdit())
        markDirty();
    annotationGesture_ = AnnotationGesture::None;
    annotationBeforeEdit_ = {};
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::clearAnnotationSelection()
{
    if (annotationGesture_ != AnnotationGesture::None)
        history_.cancelEdit();
    annotationGesture_ = AnnotationGesture::None;
    pendingSelectedGesture_ = false;
    selectedAnnotation_ = -1;
    annotationBeforeEdit_ = {};
}
QPointF SelectionOverlay::annotationLogicalPoint(QPointF point) const
{
    return point / frame_.display.devicePixelRatio;
}
void SelectionOverlay::paintAnnotationSelection(QPainter& painter) const
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations().size() || textMode_ ||
        finished_)
        return;
    const auto& annotation = annotations()[selectedAnnotation_];
    const qreal dpr = frame_.display.devicePixelRatio;
    const QPolygonF physicalCorners = annotationCorners(annotation);
    QPolygonF corners;
    for (const QPointF point : physicalCorners)
        corners << annotationLogicalPoint(point);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(245, 130, 35), 1.4, Qt::DashLine);
    pen.setDashPattern({4, 3});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(corners);
    const QRectF base = annotationBaseBounds(annotation);
    const QPointF top = annotationLogicalPoint(
        annotationFromBase(annotation, QPointF(base.center().x(), base.top())));
    const QPointF handle = annotationLogicalPoint(annotationRotationHandle(annotation, 24 * dpr));
    painter.drawLine(top, handle);
    painter.setPen(QPen(QColor(245, 130, 35), 1.5));
    painter.setBrush(QColor(245, 130, 35));
    painter.drawEllipse(handle, 4, 4);
    painter.restore();
}
void SelectionOverlay::wheelEvent(QWheelEvent* event)
{
    const bool widthTool = activeTool_ && annotationUsesLineWidth(*activeTool_);
    const bool textTool = activeTool_ && *activeTool_ == AnnotationType::Text;
    if (finished_ || saveDialogOpen_ || ocrDialog_ || textMode_ || scrollReady_ ||
        watermarkToolActive_ || (!widthTool && !textTool) ||
        !logicalSelection().contains(event->position()))
    {
        widthWheel_.reset();
        event->ignore();
        return;
    }
    for (auto* panel :
         {toolbar_, toolBar_, optionsBar_, ratioBar_, shapeBar_, static_cast<QWidget*>(status_)})
        if (panel && panel->isVisible() && panel->geometry().contains(event->position().toPoint()))
        {
            event->accept();
            return;
        }
    if (textTool)
    {
        adjustTextSize(event->angleDelta(), event->pixelDelta(), event->phase());
        event->accept();
        return;
    }
    if (event->phase() == Qt::ScrollBegin)
        widthWheel_.reset();
    const int width =
        widthWheel_.adjust(style_.lineWidth, event->angleDelta(), event->pixelDelta());
    style_.lineWidth = width;
    if (draft_)
        draft_->style.lineWidth = width;
    else if (lastAnnotationSelected_ && history_.setLastLineWidth(width))
        markDirty();
    updateOptionsBar();
    if (event->phase() == Qt::ScrollEnd)
        widthWheel_.reset();
    update();
    event->accept();
}
void SelectionOverlay::updateAnnotation(QPointF position)
{
    if (!draft_)
        return;
    const QPointF point = physicalPoint(position);
    if (draft_->type == AnnotationType::Freehand)
        appendFreehandPoint(*draft_, point);
    else
        draft_->last = point;
}
void SelectionOverlay::undoAnnotation()
{
    if (textMode_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None ||
        annotationGesture_ != AnnotationGesture::None)
        return;
    finishText(true);
    clearAnnotationSelection();
    lastAnnotationSelected_ = false;
    if (history_.undo())
        markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::redoAnnotation()
{
    if (textMode_ || finished_ || saveDialogOpen_ || dragMode_ != DragMode::None ||
        annotationGesture_ != AnnotationGesture::None)
        return;
    finishText(true);
    clearAnnotationSelection();
    lastAnnotationSelected_ = false;
    if (history_.redo())
        markDirty();
    updateToolbar();
    update();
}
void SelectionOverlay::startText(QPointF position)
{
    if (!textEditor_)
    {
        textEditor_ = new AnnotationTextEdit(this);
        textEditor_->installEventFilter(this);
        connect(textEditor_, &AnnotationTextEdit::commitRequested, this,
                [this] { finishText(true); });
        connect(textEditor_, &AnnotationTextEdit::cancelRequested, this,
                [this] { finishText(false); });
        connect(textEditor_, &AnnotationTextEdit::formatToggleRequested, this,
                [this](int format) { toggleTextStyle(static_cast<TextStyleToggle>(format)); });
    }
    lastAnnotationSelected_ = false;
    textFocusCommitPending_ = false;
    textDraft_ = Annotation{AnnotationType::Text, style_, physicalPoint(position), {}, {}, {}};
    QPalette palette = textEditor_->palette();
    palette.setColor(QPalette::Text, style_.color);
    QColor placeholder = style_.color;
    placeholder.setAlpha(160);
    palette.setColor(QPalette::PlaceholderText, placeholder);
    textEditor_->setPalette(palette);
    textEditor_->clear();
    textEditor_->setEditingArea(position.toPoint(), rect());
    textEditor_->show();
    textEditor_->raise();
    textEditor_->setFocus(Qt::MouseFocusReason);
    // 必须在 show 之后应用：applyTextEditorFont 对隐藏编辑框直接返回，
    // 否则首段文字会使用 QTextEdit 默认小字号，直到第一次滚轮才被纠正。
    applyTextEditorFont();
    textDraft_.first = physicalPoint(textEditor_->pos());
    updateToolbar();
}
void SelectionOverlay::finishText(bool commit, bool restoreFocus)
{
    textFocusCommitPending_ = false;
    if (!textEditor_ || !textEditor_->isVisible())
        return;
    if (commit)
        QGuiApplication::inputMethod()->commit();
    else
        QGuiApplication::inputMethod()->reset();
    textEditor_->clearPreedit();
    textDraft_.text = textEditor_->toPlainText();
    textDraft_.style = style_;
    textEditor_->hide();
    textEditor_->clear();
    bool added = false;
    if (commit)
    {
        added = history_.add(std::move(textDraft_));
        if (added)
        {
            markDirty();
            selectedAnnotation_ = int(annotations().size()) - 1;
            if (restoreFocus)
                showStatus(QStringLiteral("文本框已选中：拖动移动，拖圆点旋转"), true);
        }
    }
    lastAnnotationSelected_ = added;
    textDraft_ = {};
    if (restoreFocus)
        setFocus(Qt::OtherFocusReason);
    updateToolbar();
    update();
}
void SelectionOverlay::applyTextEditorFont()
{
    if (!textEditor_ || !textEditor_->isVisible())
        return;
    QFont editorFont = annotationFont(style_);
    // 编辑器在逻辑坐标中排版，支持非整数 DPR；提交后的文本按物理像素统一绘制。
    editorFont.setPointSizeF(style_.textSize * 72.0 /
                             (logicalDpiY() * frame_.display.devicePixelRatio));
    textEditor_->setAnnotationFont(editorFont);
}
void SelectionOverlay::setTextSize(int size)
{
    textFocusCommitPending_ = false;
    style_.textSize = std::clamp(size, annotationMinimumTextSize, annotationMaximumTextSize);
    if (textEditor_ && textEditor_->isVisible())
    {
        textDraft_.style = style_;
        applyTextEditorFont();
        if (!textEditor_->hasFocus())
            textEditor_->setFocus(Qt::OtherFocusReason);
        if (!textEditor_->hasFocus())
            QTimer::singleShot(0, this,
                               [this]
                               {
                                   if (textEditor_ && textEditor_->isVisible() &&
                                       !textEditor_->hasFocus())
                                       textEditor_->setFocus(Qt::OtherFocusReason);
                               });
    }
    else if (lastAnnotationSelected_ && history_.setLastTextStyle(style_.textSize, style_.bold,
                                                                  style_.italic, style_.underline))
        markDirty();
    updateOptionsBar();
    update();
}
void SelectionOverlay::toggleTextStyle(TextStyleToggle toggle)
{
    if (!activeTool_ || *activeTool_ != AnnotationType::Text)
        return;
    textFocusCommitPending_ = false;
    bool* value = toggle == TextStyleToggle::Bold     ? &style_.bold
                  : toggle == TextStyleToggle::Italic ? &style_.italic
                                                      : &style_.underline;
    *value = !*value;
    if (textEditor_ && textEditor_->isVisible())
    {
        textDraft_.style = style_;
        applyTextEditorFont();
        if (!textEditor_->hasFocus())
            textEditor_->setFocus(Qt::OtherFocusReason);
        if (!textEditor_->hasFocus())
            QTimer::singleShot(0, this,
                               [this]
                               {
                                   if (textEditor_ && textEditor_->isVisible() &&
                                       !textEditor_->hasFocus())
                                       textEditor_->setFocus(Qt::OtherFocusReason);
                               });
    }
    else if (lastAnnotationSelected_ && history_.setLastTextStyle(style_.textSize, style_.bold,
                                                                  style_.italic, style_.underline))
        markDirty();
    updateOptionsBar();
    update();
}
void SelectionOverlay::adjustTextSize(QPoint angleDelta, QPoint pixelDelta, Qt::ScrollPhase phase)
{
    if (!activeTool_ || *activeTool_ != AnnotationType::Text)
        return;
    if (phase == Qt::ScrollBegin)
        widthWheel_.reset();
    setTextSize(widthWheel_.adjust(style_.textSize, angleDelta, pixelDelta,
                                   annotationMinimumTextSize, annotationMaximumTextSize));
    if (phase == Qt::ScrollEnd)
        widthWheel_.reset();
}
bool SelectionOverlay::eventFilter(QObject* watched, QEvent* event)
{
    // 文本格式与字号按钮在按下时直接应用；不把焦点从编辑框抢走，
    // 否则 FocusOut 会先提交当前文本，按一次加粗就结束输入。
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
    {
        if (watched == boldButton_)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                toggleTextStyle(TextStyleToggle::Bold);
            }
            event->accept();
            return true;
        }
        if (watched == italicButton_)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                toggleTextStyle(TextStyleToggle::Italic);
            }
            event->accept();
            return true;
        }
        if (watched == underlineButton_)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                toggleTextStyle(TextStyleToggle::Underline);
            }
            event->accept();
            return true;
        }
        const int sizeIndex = textSizeButtons_.indexOf(qobject_cast<QPushButton*>(watched));
        if (sizeIndex >= 0)
        {
            if (event->type() == QEvent::MouseButtonPress)
            {
                textFocusCommitPending_ = false;
                setTextSize(annotationTextSizes[sizeIndex]);
            }
            event->accept();
            return true;
        }
    }
    if (watched == textEditor_ && event->type() == QEvent::Wheel)
    {
        auto* wheel = static_cast<QWheelEvent*>(event);
        adjustTextSize(wheel->angleDelta(), wheel->pixelDelta(), wheel->phase());
        event->accept();
        return true;
    }
    if (watched != textEditor_ && event->type() == QEvent::Wheel)
    {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == ratioButton_ || watched == shapeButton_ ||
         (shapeBar_ && shapeBar_->isAncestorOf(qobject_cast<QWidget*>(watched))) ||
         (ratioBar_ && ratioBar_->isAncestorOf(qobject_cast<QWidget*>(watched)))))
        finishText(true, false);
    if ((watched == toolbar_ || watched == toolBar_ || watched == optionsBar_ ||
         watched == ratioBar_ || watched == shapeBar_ || qobject_cast<QLabel*>(watched)) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease))
    {
        // 空白 / 提示标签提交并消化手势；比例按钮已在按下时提交，其他按钮沿用 clicked。
        if (event->type() == QEvent::MouseButtonPress)
            finishText(true, false);
        event->accept();
        return true;
    }
    if (watched != textEditor_ && event->type() == QEvent::MouseMove)
    {
        // 子工具栏保持箭头光标，同时同步其下方画布的模式状态。
        const auto* mouse = static_cast<QMouseEvent*>(event);
        updateCursor(mapFromGlobal(mouse->globalPosition().toPoint()));
    }
    if (watched == textEditor_ && event->type() == QEvent::FocusOut)
    {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        // 候选窗口 / 系统面板的临时焦点变化不提交；点击其他控件才提交。
        if (reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason)
        {
            const auto over = [](QWidget* control)
            {
                return control && control->isVisible() &&
                       control->rect().contains(control->mapFromGlobal(QCursor::pos()));
            };
            // 文本格式 / 字号按钮在按下时自行应用样式，不能先被 FocusOut 提交文本。
            bool textControl = over(boldButton_) || over(italicButton_) || over(underlineButton_);
            for (auto* control : textSizeButtons_)
                textControl |= over(control);
            if (!textControl)
            {
                textFocusCommitPending_ = true;
                QTimer::singleShot(0, this,
                                   [this]
                                   {
                                       if (textFocusCommitPending_)
                                       {
                                           textFocusCommitPending_ = false;
                                           finishText(true, false);
                                       }
                                   });
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
void SelectionOverlay::discardAnnotations()
{
    finishText(false);
    clearAnnotationSelection();
    history_.clear();
    draft_.reset();
    activeTool_.reset();
    selectMode_ = false;
    lastAnnotationSelected_ = false;
    updateToolbar();
}
void SelectionOverlay::toggleTextMode()
{
    if (finished_ || saveDialogOpen_ || ocrDialog_ || selection_.isEmpty() ||
        dragMode_ != DragMode::None)
        return;
    watermarkToolActive_ = false;
    watermarkSelected_ = false;
    watermarkGesture_ = WatermarkGesture::None;
    if (textMode_)
        exitTextMode();
    else
    {
        finishText(true);
        clearAnnotationSelection();
        activeTool_.reset();
        selectMode_ = false;
        lastAnnotationSelected_ = false;
        for (auto* button : toolButtons_)
            button->setChecked(false);
        if (selectButton_)
            selectButton_->setChecked(false);
        textMode_ = true;
        hideMagnifier();
        startOcrRecognition();
    }
    setFocus(Qt::OtherFocusReason);
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::exitTextMode()
{
    if (!textMode_)
        return;
    ++recognitionToken_;
    recognitionProgress_.stop();
    recognizingText_ = false;
    selectingText_ = false;
    textMode_ = false;
    lastAnnotationSelected_ = false;
    ocrResult_ = {};
    ocrAnchor_ = {};
    ocrSelection_.clear();
    if (ocrDialog_)
        ocrDialog_->reject();
    statusTimeout_.stop();
    if (status_)
        status_->hide();
    updateToolbar();
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}
void SelectionOverlay::startOcrRecognition()
{
    if (!textMode_ || finished_)
        return;
    if (!actions_.recognizer)
        actions_.recognizer = createTextRecognizer();
    const quint64 token = ++recognitionToken_;
    recognizingText_ = true;
    ocrResult_ = {};
    ocrSelection_.clear();
    selectingText_ = false;
    statusTimeout_.stop();
    status_->hide();
    if (ocrDialog_)
        ocrDialog_->setRecognizing();
    auto* task = new TextRecognitionTask(
        actions_.recognizer, renderAnnotatedSelection(frame_.pixels, annotations(), selection_),
        token, this);
    connect(
        task, &TextRecognitionTask::completed, this,
        [this](quint64 completedToken, TextRecognitionResult result)
        {
            if (!textMode_ || finished_ || completedToken != recognitionToken_)
                return;
            recognitionProgress_.stop();
            recognizingText_ = false;
            ocrResult_ = std::move(result);
            if (ocrDialog_)
                ocrDialog_->setResult(ocrResult_);
            if (!ocrResult_.ok)
                showStatus(QStringLiteral("识别失败：%1").arg(ocrResult_.explanation), false);
            else if (ocrResult_.lines.isEmpty())
                showStatus(QStringLiteral("未识别到文字"), false);
            else
            {
                status_->hide();
                updateToolbar();
            }
            updateCursor(mapFromGlobal(QCursor::pos()));
            update();
        },
        Qt::QueuedConnection);
    connect(task, &QThread::finished, task, &QObject::deleteLater);
    recognitionProgress_.start(300);
    task->start();
    update();
}
void SelectionOverlay::openOcrDialog()
{
    if (!textMode_ || finished_ || saveDialogOpen_ || ocrDialog_)
        return;
    auto* dialog = new OcrDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    ocrDialog_ = dialog;
    if (!recognizingText_)
        dialog->setResult(ocrResult_);
    connect(dialog, &OcrDialog::retryRequested, this, &SelectionOverlay::startOcrRecognition);
    connect(dialog, &QDialog::finished, this,
            [this]
            {
                ocrDialog_ = nullptr;
                if (!finished_)
                {
                    updateToolbar();
                    activateWindow();
                    setFocus(Qt::OtherFocusReason);
                }
            });
    updateToolbar();
    dialog->open();
}
text_layout::TokenPosition SelectionOverlay::textAt(QPointF position, bool nearest) const
{
    if (!textMode_ || recognizingText_ || !ocrResult_.ok)
        return {};
    const QPointF point = physicalPoint(position) - QPointF(selection_.topLeft());
    int lineIndex = -1;
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i < ocrResult_.lines.size(); ++i)
    {
        const auto& line = ocrResult_.lines[i];
        if (!nearest)
        {
            const auto tokens = text_layout::selectableTokens(line);
            for (int j = 0; j < tokens.size(); ++j)
                if (tokens[j].box.contains(point))
                    return {i, j};
        }
        else
        {
            const qreal distance = std::abs(line.box.center().y() - point.y());
            if (distance < best && !line.text.isEmpty())
            {
                lineIndex = i;
                best = distance;
            }
        }
    }
    return lineIndex < 0
               ? text_layout::TokenPosition{}
               : text_layout::TokenPosition{
                     lineIndex, text_layout::nearestToken(ocrResult_.lines[lineIndex], point.x())};
}
void SelectionOverlay::selectOcrTo(QPointF position)
{
    ocrSelection_ =
        text_layout::selectionRange(ocrResult_.lines, ocrAnchor_, textAt(position, true));
    update();
}
void SelectionOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (scrollReady_)
        return;
    if (!textMode_)
    {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (ocrDialog_ || finished_ || saveDialogOpen_ || event->button() != Qt::LeftButton)
        return;
    ocrAnchor_ = textAt(event->position(), false);
    selectingText_ = false;
    ocrSelection_ = text_layout::selectionRange(ocrResult_.lines, ocrAnchor_, ocrAnchor_);
    update();
}
QString SelectionOverlay::selectedOcrText() const
{
    return text_layout::selectionText(ocrResult_.lines, ocrSelection_);
}
QVector<QRectF> SelectionOverlay::ocrHighlightBoxes() const
{
    QVector<QRectF> boxes;
    for (const auto& range : ocrSelection_)
    {
        const auto tokens = text_layout::selectableTokens(ocrResult_.lines[range.line]);
        for (int i = range.firstToken; i <= range.lastToken; ++i)
            boxes.append(tokens[i].box);
    }
    return boxes;
}
void SelectionOverlay::copyShortcut()
{
    if (scrollReady_)
        return;
    if (finished_ || saveDialogOpen_ || ocrDialog_)
        return;
    if (!textMode_)
    {
        copySelection();
        return;
    }
    const QString text = selectedOcrText();
    if (!text.isEmpty())
    {
        actions_.setClipboardText(text);
        showStatus(QStringLiteral("已复制 %1 个字符").arg(text.size()), true);
        statusTimeout_.start(2000);
    }
}

}
