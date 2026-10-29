#pragma once
#include "app/recording_controller.h"
#include <QCoreApplication>
#include <QTemporaryDir>
namespace waibusnap
{
struct RecordingFakeState
{
    RecordingSession::EventCallback event;
    std::function<void(RecordingSelection)> selected;
    std::function<void()> cancelled;
    std::function<void()> suspend;
    std::function<void(CaptureResult)> freezeDone;
    int starts = 0, pauses = 0, resumes = 0, stops = 0, discards = 0, destroyed = 0;
    int freezes = 0, requests = 0, toasts = 0, observersStopped = 0;
    bool startAccepted = true, synchronousDenied = false;
    bool supported = true, authorized = true, granted = true, matches = true;
    bool freezeFails = false, holdFreeze = false;
    qint64 clock = 0;
    qint64 disk = 4LL * 1024 * 1024 * 1024;
    RecordingRequest request;
    RecordingOutput defaultOutput = RecordingOutput::Video;
    RecordingEvent lastToast;
    std::optional<RecordingWindowInfo> window =
        RecordingWindowInfo{123, QRect(-180, 40, 200, 100), QStringLiteral("测试窗口"), true};
};
class FakeRecordingSession final : public RecordingSession
{
  public:
    explicit FakeRecordingSession(std::shared_ptr<RecordingFakeState> state)
        : state_(std::move(state))
    {
    }
    ~FakeRecordingSession() override { ++state_->destroyed; }
    bool start(const RecordingRequest& request, EventCallback callback, QString* error) override
    {
        ++state_->starts;
        state_->request = request;
        state_->event = std::move(callback);
        if (state_->synchronousDenied)
        {
            RecordingEvent denied;
            denied.type = RecordingEventType::PermissionDenied;
            denied.message = QStringLiteral("权限已撤销");
            state_->event(denied);
            *error = denied.message;
            return false;
        }
        if (!state_->startAccepted)
            *error = QStringLiteral("写入器初始化失败");
        return state_->startAccepted;
    }
    void pause() override { ++state_->pauses; }
    void resume() override { ++state_->resumes; }
    void stop() override { ++state_->stops; }
    void discard() override { ++state_->discards; }

  private:
    std::shared_ptr<RecordingFakeState> state_;
};
class FakeRecordingPicker final : public RecordingPicker
{
  public:
    explicit FakeRecordingPicker(std::shared_ptr<RecordingFakeState> state)
        : state_(std::move(state))
    {
    }
    void open(std::function<void(RecordingSelection)> selected,
              std::function<void()> cancelled) override
    {
        state_->selected = std::move(selected);
        state_->cancelled = std::move(cancelled);
    }
    void dismiss() override {}
    void setDefaultOutput(RecordingOutput output) override { state_->defaultOutput = output; }

  private:
    std::shared_ptr<RecordingFakeState> state_;
};
class FakeRecordingEnvironment final : public RecordingEnvironmentObserver
{
  public:
    explicit FakeRecordingEnvironment(std::shared_ptr<RecordingFakeState> state)
        : state_(std::move(state))
    {
    }
    void start(std::function<void()> suspend) override { state_->suspend = std::move(suspend); }
    void stop() override { ++state_->observersStopped; }

  private:
    std::shared_ptr<RecordingFakeState> state_;
};
struct RecordingHarness
{
    QTemporaryDir directory;
    std::shared_ptr<RecordingFakeState> fake = std::make_shared<RecordingFakeState>();
    DisplayTarget display{77, QRect(-300, 0, 800, 600), 2, 9, QRect(-300, 0, 800, 570)};
    RecordingDependencies dependencies()
    {
        RecordingDependencies d;
        const auto f = fake;
        const auto target = display;
        d.showWindows = false;
        d.supported = [f] { return f->supported; };
        d.hasPermission = [f] { return f->authorized; };
        d.requestPermission = [f]
        {
            ++f->requests;
            return f->granted;
        };
        d.cursorDisplay = [target] { return DisplayResult{target, {}}; };
        d.stillMatches = [f](const DisplayTarget&) { return f->matches; };
        d.clock = [f] { return f->clock; };
        d.now = [] { return QDateTime::fromMSecsSinceEpoch(1234567890000); };
        d.availableBytes = [f](const QString&) { return f->disk; };
        d.windowInfo = [f](quint64) { return f->window; };
        d.environmentFactory = [f] { return std::make_unique<FakeRecordingEnvironment>(f); };
        d.sessionFactory = [f] { return std::make_unique<FakeRecordingSession>(f); };
        d.pickerFactory = [f](CaptureFrame) { return std::make_unique<FakeRecordingPicker>(f); };
        d.toastPresenter = [f](const RecordingEvent& event, bool)
        {
            ++f->toasts;
            f->lastToast = event;
        };
        d.freezeFrame = [f, target](const DisplayTarget&, std::function<void(CaptureResult)> done)
        {
            ++f->freezes;
            if (f->holdFreeze)
            {
                f->freezeDone = std::move(done);
                return;
            }
            CaptureResult result;
            result.frame.display = target;
            if (f->freezeFails)
                result.error = CaptureError::Unavailable;
            else
            {
                result.frame.pixels = QImage(1600, 1200, QImage::Format_RGB32);
                result.frame.pixels.fill(Qt::blue);
            }
            done(result);
        };
        return d;
    }
    RecordingPreferences preferences(int countdown = 0) const
    {
        RecordingPreferences p;
        p.videoDirectory = directory.path();
        p.countdownSeconds = countdown;
        return p;
    }
    static void drain()
    {
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
    }
    void select(RecordingTargetKind kind = RecordingTargetKind::Region)
    {
        RecordingSelection selection;
        selection.output = fake->defaultOutput;
        selection.kind = kind;
        selection.pixelRect = QRect(60, 80, 321, 241);
        selection.windowId = kind == RecordingTargetKind::Window ? 123 : 0;
        fake->selected(selection);
        drain();
    }
    void event(RecordingEventType type, qint64 ms = 0)
    {
        RecordingEvent event;
        event.type = type;
        event.recordedMs = ms;
        event.path = fake->request.finalPath;
        event.pixelSize = fake->request.outputPixels;
        event.bytes = 12345;
        event.message = type == RecordingEventType::Failed ? QStringLiteral("保存失败") : QString();
        fake->event(event);
        drain();
    }
    void start(RecordingController& controller,
               RecordingTargetKind kind = RecordingTargetKind::Region)
    {
        controller.startSelection(preferences());
        drain();
        select(kind);
        event(RecordingEventType::Started);
    }
    void advance(RecordingController& controller, qint64 ms)
    {
        fake->clock += ms;
        controller.poll();
        drain();
    }
};
}
