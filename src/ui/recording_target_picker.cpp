#include "ui/recording_target_picker.h"
#include "core/selection_geometry.h"
#include <QButtonGroup>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
namespace waibusnap
{
RecordingTargetPicker::RecordingTargetPicker(CaptureFrame frame,
                                             QVector<RecordingWindowInfo> windows,
                                             RecordingOutput defaultOutput)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      frame_(std::move(frame)), windows_(std::move(windows))
{
    setObjectName(QStringLiteral("recordingPicker"));
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setGeometry(frame_.display.logicalGeometry);
    toolbar_ = new QWidget(this);
    toolbar_->setStyleSheet(QStringLiteral(
        "QWidget { color: white; background: #26282d; border-radius: 6px; }"
        "QPushButton { padding: 6px; border: 1px solid #666; }"
        "QPushButton:checked { background: #54657c; } QPushButton:disabled { color: #888; }"));
    auto* layout = new QHBoxLayout(toolbar_);
    layout->setContentsMargins(6, 4, 6, 4);
    const std::array<QString, 3> labels = {QStringLiteral("区域"), QStringLiteral("窗口"),
                                           QStringLiteral("全屏")};
    const std::array<QString, 3> names = {QStringLiteral("recordingPickerAreaButton"),
                                          QStringLiteral("recordingPickerWindowButton"),
                                          QStringLiteral("recordingPickerDisplayButton")};
    for (size_t i = 0; i < modes_.size(); ++i)
    {
        modes_[i] = new QPushButton(labels[i], toolbar_);
        modes_[i]->setObjectName(names[i]);
        modes_[i]->setCheckable(true);
        layout->addWidget(modes_[i]);
        connect(modes_[i], &QPushButton::clicked, this,
                [this, i] { setMode(RecordingTargetKind(i)); });
    }
    auto* video = new QPushButton(QStringLiteral("视频"), toolbar_);
    video->setObjectName(QStringLiteral("recordingPickerOutputVideo"));
    video->setCheckable(true);
    video->setChecked(true);
    video->setToolTip(QStringLiteral("MP4"));
    layout->addWidget(video);
    auto* gif = new QPushButton(QStringLiteral("GIF"), toolbar_);
    gif->setObjectName(QStringLiteral("recordingPickerOutputGif"));
    gif->setCheckable(true);
    gif->setToolTip(QStringLiteral("GIF · 无声音"));
    layout->addWidget(gif);
    auto* outputs = new QButtonGroup(this);
    outputs->addButton(video);
    outputs->addButton(gif);
    connect(video, &QPushButton::clicked, this,
            [this]
            {
                selection_.output = RecordingOutput::Video;
                updateHint();
            });
    connect(gif, &QPushButton::clicked, this,
            [this]
            {
                selection_.output = RecordingOutput::Gif;
                updateHint();
            });
    start_ = new QPushButton(QStringLiteral("开始录制"), toolbar_);
    start_->setObjectName(QStringLiteral("recordingPickerStart"));
    layout->addWidget(start_);
    connect(start_, &QPushButton::clicked, this, &RecordingTargetPicker::acceptTarget);
    auto* cancel = new QPushButton(QStringLiteral("取消"), toolbar_);
    cancel->setObjectName(QStringLiteral("recordingPickerCancel"));
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &QWidget::close);
    toolbar_->adjustSize();
    toolbar_->setMinimumHeight(44);
    toolbar_->move(qMax(0, (width() - toolbar_->width()) / 2), 12);
    hint_ = new QLabel(this);
    hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hint_->setStyleSheet(
        QStringLiteral("color: white; background: rgba(0,0,0,210); padding: 8px;"));
    hint_->setWordWrap(true);
    hint_->setMaximumWidth(qMax(100, width() - 24));
    setMode(RecordingTargetKind::Region);
    setDefaultOutput(defaultOutput);
}
void RecordingTargetPicker::setDefaultOutput(RecordingOutput output)
{
    selection_.output = output;
    findChild<QPushButton*>(QStringLiteral("recordingPickerOutputVideo"))
        ->setChecked(output == RecordingOutput::Video);
    findChild<QPushButton*>(QStringLiteral("recordingPickerOutputGif"))
        ->setChecked(output == RecordingOutput::Gif);
    updateHint();
}
void RecordingTargetPicker::open(std::function<void(RecordingSelection)> selected,
                                 std::function<void()> cancelled)
{
    selected_ = std::move(selected);
    cancelled_ = std::move(cancelled);
    winId();
    raiseOverlayAboveSystemChrome(windowHandle());
    setGeometry(frame_.display.logicalGeometry);
    observation_ = observeOverlayWindow(windowHandle(), this,
                                        [this](const QString&)
                                        {
                                            if (!finished_ && isVisible())
                                                reassertOverlayWindow(windowHandle(), true);
                                        });
    show();
    raise();
    activateWindow();
    setFocus();
}
void RecordingTargetPicker::dismiss()
{
    finished_ = true;
    observation_.reset();
    hide();
}
void RecordingTargetPicker::setMode(RecordingTargetKind kind)
{
    const auto output = selection_.output;
    selection_ = {};
    selection_.output = output;
    selection_.kind = kind;
    hover_.reset();
    dragging_ = false;
    setCursor(kind == RecordingTargetKind::Region ? Qt::CrossCursor : Qt::ArrowCursor);
    for (size_t i = 0; i < modes_.size(); ++i)
        modes_[i]->setChecked(i == size_t(kind));
    if (kind == RecordingTargetKind::Display)
    {
        selection_.pixelRect = frame_.pixels.rect();
        selection_.logicalFrame = frame_.display.logicalGeometry;
    }
    updateHint();
    update();
}
QRectF RecordingTargetPicker::highlighted() const
{
    if (selection_.kind == RecordingTargetKind::Window)
    {
        const QRect global = selection_.windowId ? selection_.logicalFrame
                                                 : (hover_ ? hover_->logicalFrame : QRect());
        return QRectF(global.translated(-frame_.display.logicalGeometry.topLeft()));
    }
    const qreal dpr = frame_.display.devicePixelRatio;
    return QRectF(selection_.pixelRect.x() / dpr, selection_.pixelRect.y() / dpr,
                  selection_.pixelRect.width() / dpr, selection_.pixelRect.height() / dpr);
}
void RecordingTargetPicker::updateHint(bool tooSmall)
{
    const bool valid =
        selection_.kind == RecordingTargetKind::Window
            ? selection_.windowId != 0
            : selection_.pixelRect.width() >= 16 && selection_.pixelRect.height() >= 16;
    start_->setEnabled(valid && !dragging_);
    QString text;
    if (selection_.kind == RecordingTargetKind::Window)
    {
        QString label = hover_ ? hover_->label : QStringLiteral("单击选择窗口");
        for (const auto& window : windows_)
            if (window.id == selection_.windowId)
                label = window.label;
        text = label + QStringLiteral(" · Enter 开始 · Esc 取消");
    }
    else
        text = QStringLiteral("%1×%2 px · %3 · Enter 开始 · Esc 取消")
                   .arg(selection_.pixelRect.width())
                   .arg(selection_.pixelRect.height())
                   .arg(tooSmall ? QStringLiteral("选区太小，最小 16×16 物理像素，请重新拖拽")
                        : selection_.kind == RecordingTargetKind::Display
                            ? QStringLiteral("整块屏幕")
                            : QStringLiteral("拖拽框选，最小 16×16 物理像素"));
    if (selection_.output == RecordingOutput::Gif)
        text += QStringLiteral(" · GIF · 无声音");
    hint_->setText(text);
    hint_->setToolTip(text);
    hint_->adjustSize();
    hint_->move(qMax(0, (width() - hint_->width()) / 2), qMax(64, height() - hint_->height() - 20));
}
void RecordingTargetPicker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.drawImage(QRectF(rect()), frame_.pixels, QRectF(frame_.pixels.rect()));
    QPainterPath shade;
    shade.setFillRule(Qt::OddEvenFill);
    shade.addRect(QRectF(rect()));
    const auto target = highlighted();
    if (!target.isEmpty())
        shade.addRect(target);
    p.fillPath(shade, QColor(0, 0, 0, 140));
    p.setPen(QPen(Qt::white, 1));
    if (!target.isEmpty())
        p.drawRect(target.adjusted(0, 0, -1, -1));
}
void RecordingTargetPicker::dragTo(QPointF point)
{
    selection_.pixelRect = normalizedPixelSelection(press_, point, frame_.display.devicePixelRatio,
                                                    frame_.pixels.size());
    const auto r = highlighted();
    selection_.logicalFrame =
        r.toAlignedRect().translated(frame_.display.logicalGeometry.topLeft());
    updateHint();
    update();
}
void RecordingTargetPicker::mousePressEvent(QMouseEvent* event)
{
    if (finished_ || event->button() != Qt::LeftButton)
        return;
    if (selection_.kind == RecordingTargetKind::Region)
    {
        dragging_ = true;
        press_ = event->position();
        dragTo(press_);
    }
    else if (selection_.kind == RecordingTargetKind::Window && hover_)
    {
        selection_.windowId = hover_->id;
        selection_.logicalFrame = hover_->logicalFrame;
        updateHint();
        update();
    }
}
void RecordingTargetPicker::mouseMoveEvent(QMouseEvent* event)
{
    if (finished_)
        return;
    if (dragging_)
        dragTo(event->position());
    else if (selection_.kind == RecordingTargetKind::Window)
    {
        hover_.reset();
        const QPoint global =
            event->position().toPoint() + frame_.display.logicalGeometry.topLeft();
        for (const auto& window : windows_)
            if (window.onScreen && window.logicalFrame.contains(global))
            {
                hover_ = window;
                break;
            }
        updateHint();
        update();
    }
}
void RecordingTargetPicker::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragging_ && event->button() == Qt::LeftButton)
    {
        dragTo(event->position());
        dragging_ = false;
        updateHint(selection_.pixelRect.width() < 16 || selection_.pixelRect.height() < 16);
    }
}
void RecordingTargetPicker::acceptTarget()
{
    if (finished_)
        return;
    if (!start_->isEnabled())
    {
        updateHint(true);
        return;
    }
    dismiss();
    if (selected_)
        selected_(selection_);
}
void RecordingTargetPicker::keyPressEvent(QKeyEvent* event)
{
    switch (event->key())
    {
    case Qt::Key_Escape:
        close();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        acceptTarget();
        break;
    case Qt::Key_1:
        setMode(RecordingTargetKind::Region);
        break;
    case Qt::Key_2:
        setMode(RecordingTargetKind::Window);
        break;
    case Qt::Key_3:
        setMode(RecordingTargetKind::Display);
        break;
    case Qt::Key_G:
        findChild<QPushButton*>(QStringLiteral("recordingPickerOutputGif"))->click();
        break;
    case Qt::Key_V:
        findChild<QPushButton*>(QStringLiteral("recordingPickerOutputVideo"))->click();
        break;
    case Qt::Key_Left:
        setMode(RecordingTargetKind((int(selection_.kind) + 2) % 3));
        break;
    case Qt::Key_Right:
        setMode(RecordingTargetKind((int(selection_.kind) + 1) % 3));
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}
void RecordingTargetPicker::closeEvent(QCloseEvent* event)
{
    const bool notify = !finished_;
    dismiss();
    event->accept();
    if (notify && cancelled_)
        cancelled_();
}
}
