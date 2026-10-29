#pragma once
#include "interfaces/capture_provider.h"
#include "interfaces/recording.h"
#include "ui/recording_target_picker.h"
#include <QObject>
#include <QTimer>
namespace waibusnap
{
struct RecordingDependencies
{
    std::function<bool()> supported = recordingCaptureSupported;
    std::function<bool()> hasPermission = hasScreenRecordingPermission;
    std::function<bool()> requestPermission = requestScreenRecordingPermission;
    std::function<DisplayResult()> cursorDisplay;
    std::function<bool(const DisplayTarget&)> stillMatches;
    std::function<void(const DisplayTarget&, std::function<void(CaptureResult)>)> freezeFrame;
    std::function<std::unique_ptr<RecordingSession>()> sessionFactory = createRecordingSession;
    std::function<std::unique_ptr<RecordingPicker>(CaptureFrame)> pickerFactory;
    std::function<qint64()> clock;
    std::function<QDateTime()> now = [] { return QDateTime::currentDateTime(); };
    std::function<qint64(const QString&)> availableBytes;
    std::function<std::optional<RecordingWindowInfo>(quint64)> windowInfo = recordingWindowInfo;
    std::function<std::unique_ptr<RecordingEnvironmentObserver>()> environmentFactory =
        createRecordingEnvironmentObserver;
    std::function<void(const RecordingEvent&, bool)> toastPresenter;
    // 测试可关闭真实窗口，倒计时和监视仍走同一套状态机。
    bool showWindows = true;
};
class RecordingCountdownWindow;
class RecordingBorderWindow;
class RecordingController final : public QObject
{
    Q_OBJECT
  public:
    enum class State
    {
        Idle,
        Preparing,
        Selecting,
        Countdown,
        Recording,
        Paused,
        Finalizing
    };
    Q_ENUM(State)
    enum class QuitChoice
    {
        Save,
        Discard,
        Cancel
    };
    explicit RecordingController(RecordingDependencies dependencies, QObject* parent = nullptr);
    ~RecordingController() override;
    State state() const { return state_; }
    bool active() const { return state_ != State::Idle; }
    bool captureSupported() const { return dependencies_.supported(); }
    qint64 recordedMs() const { return recordedMs_; }
    int countdownRemaining() const { return countdownRemaining_; }
    bool autoPaused() const { return autoPaused_; }
    QString statusText() const;
    const RecordingRequest& request() const { return request_; }
    void startSelection(RecordingPreferences preferences);
    void pause();
    void resume();
    void stopAndSave();
    void discard();
    void cancel();
    void quitRequested(QuitChoice choice);
    void topologyChanged();
    // 与定时器共用的单调时钟入口，允许 fake 时钟确定性推进。
    void poll();
  signals:
    void stateChanged(waibusnap::RecordingController::State state);
    void progressChanged(qint64 recordedMs);
    void noticeChanged(QString notice);
    void finished(waibusnap::RecordingEvent event);
    void failed(QString error);
    void permissionRequired();
    void quitReady();

  private:
    void setState(State state);
    void selected(quint64 token, RecordingSelection selection);
    void beginSession();
    void handleEvent(quint64 token, RecordingEvent event);
    void fail(QString error, QString path = {});
    void reset();
    void automaticStop(const QString& reason);
    bool validateDirectory();
    void updateBorder();
    RecordingDependencies dependencies_;
    RecordingPreferences preferences_;
    RecordingRequest request_;
    DisplayTarget display_;
    QRect targetFrame_;
    State state_ = State::Idle;
    std::unique_ptr<RecordingPicker> picker_;
    std::unique_ptr<RecordingSession> session_;
    std::unique_ptr<RecordingEnvironmentObserver> environment_;
    std::unique_ptr<RecordingCountdownWindow> countdown_;
    std::unique_ptr<RecordingBorderWindow> border_;
    QTimer timer_;
    quint64 token_ = 0;
    qint64 recordedMs_ = 0;
    qint64 countdownEnd_ = 0;
    qint64 nextDisk_ = 0;
    qint64 nextWindow_ = 0;
    qint64 nextProgress_ = 0;
    int countdownRemaining_ = 0;
    int invisibleCount_ = 0;
    bool lowWarned_ = false;
    bool diskFailureWarned_ = false;
    bool userPaused_ = false;
    bool autoPaused_ = false;
    bool quitAfterRecording_ = false;
    bool quitDuringStart_ = false;
    bool stopWhenStarted_ = false;
    qint64 quitDeadline_ = 0;
    QString autoStopReason_;
};
}
Q_DECLARE_METATYPE(waibusnap::RecordingEvent)
