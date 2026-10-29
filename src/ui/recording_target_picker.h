#pragma once
#include "interfaces/capture_provider.h"
#include "interfaces/overlay_window_behavior.h"
#include "interfaces/recording.h"
#include <QWidget>
#include <array>
class QLabel;
class QPushButton;
namespace waibusnap
{
struct RecordingSelection
{
    RecordingTargetKind kind = RecordingTargetKind::Region;
    RecordingOutput output = RecordingOutput::Video;
    QRect pixelRect;
    quint64 windowId = 0;
    QRect logicalFrame;
};
class RecordingPicker
{
  public:
    virtual ~RecordingPicker() = default;
    virtual void open(std::function<void(RecordingSelection)> selected,
                      std::function<void()> cancelled) = 0;
    virtual void dismiss() = 0;
    virtual void setDefaultOutput(RecordingOutput) {}
};
class RecordingTargetPicker final : public QWidget, public RecordingPicker
{
    Q_OBJECT
  public:
    explicit RecordingTargetPicker(CaptureFrame frame,
                                   QVector<RecordingWindowInfo> windows = recordingWindows(),
                                   RecordingOutput defaultOutput = RecordingOutput::Video);
    void open(std::function<void(RecordingSelection)> selected,
              std::function<void()> cancelled) override;
    void dismiss() override;
    void setDefaultOutput(RecordingOutput output) override;
    RecordingSelection selection() const { return selection_; }
    void setMode(RecordingTargetKind kind);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    void acceptTarget();
    void updateHint(bool tooSmall = false);
    void dragTo(QPointF point);
    QRectF highlighted() const;
    CaptureFrame frame_;
    QVector<RecordingWindowInfo> windows_;
    RecordingSelection selection_;
    std::optional<RecordingWindowInfo> hover_;
    QPointF press_;
    bool dragging_ = false;
    bool finished_ = false;
    QLabel* hint_ = nullptr;
    QWidget* toolbar_ = nullptr;
    QPushButton* start_ = nullptr;
    std::array<QPushButton*, 3> modes_{};
    std::function<void(RecordingSelection)> selected_;
    std::function<void()> cancelled_;
    std::unique_ptr<OverlayWindowObservation> observation_;
};
}
