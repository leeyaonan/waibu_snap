#include "../fixtures/recording_control_fakes.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QtTest>
using namespace waibusnap;
using State = RecordingController::State;
class RecordingControllerTest : public QObject
{
    Q_OBJECT
  private slots:
    void permissionDeniedDoesNotFreeze()
    {
        RecordingHarness h;
        h.fake->authorized = h.fake->granted = false;
        RecordingController c(h.dependencies());
        QSignalSpy permission(&c, &RecordingController::permissionRequired);
        c.startSelection(h.preferences());
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(permission.size(), 1);
        QCOMPARE(h.fake->requests, 1);
        QCOMPARE(h.fake->freezes, 0);
    }
    void grantAndFreezeFailure()
    {
        RecordingHarness h;
        h.fake->authorized = false;
        h.fake->freezeFails = true;
        RecordingController c(h.dependencies());
        QSignalSpy failed(&c, &RecordingController::failed);
        c.startSelection(h.preferences());
        h.drain();
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->requests, 1);
        QCOMPARE(h.fake->freezes, 1);
        QCOMPARE(failed.size(), 1);
    }
    void countdown_data()
    {
        QTest::addColumn<int>("seconds");
        QTest::newRow("immediate") << 0;
        QTest::newRow("three") << 3;
        QTest::newRow("five") << 5;
    }
    void countdown()
    {
        QFETCH(int, seconds);
        RecordingHarness h;
        RecordingController c(h.dependencies());
        c.startSelection(h.preferences(seconds));
        QCOMPARE(c.state(), State::Preparing);
        h.drain();
        QCOMPARE(c.state(), State::Selecting);
        h.select();
        if (seconds)
        {
            QCOMPARE(c.state(), State::Countdown);
            QCOMPARE(h.fake->starts, 0);
            h.advance(c, 1000);
            QCOMPARE(c.countdownRemaining(), seconds - 1);
            h.advance(c, (seconds - 1) * 1000);
        }
        QCOMPARE(h.fake->starts, 1);
        h.event(RecordingEventType::Started);
        QCOMPARE(c.state(), State::Recording);
        h.event(RecordingEventType::Progress, 12345);
        QCOMPARE(c.recordedMs(), 12345);
        c.pause();
        c.pause();
        QCOMPARE(c.state(), State::Paused);
        QCOMPARE(h.fake->pauses, 1);
        c.resume();
        c.resume();
        QCOMPARE(h.fake->resumes, 1);
        c.stopAndSave();
        c.stopAndSave();
        QCOMPARE(c.state(), State::Finalizing);
        QCOMPARE(h.fake->stops, 1);
        QCOMPARE(h.fake->destroyed, 0);
        h.event(RecordingEventType::Finished, 12345);
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->toasts, 1);
        QCOMPARE(h.fake->destroyed, 1);
        h.event(RecordingEventType::Failed);
        QCOMPARE(h.fake->toasts, 1);
    }
    void cancelAndLateFreeze()
    {
        RecordingHarness h;
        h.fake->holdFreeze = true;
        RecordingController c(h.dependencies());
        c.startSelection(h.preferences());
        c.cancel();
        c.cancel();
        h.fake->freezeDone({});
        h.drain();
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->starts, 0);
        h.fake->holdFreeze = false;
        c.startSelection(h.preferences(5));
        h.drain();
        h.select();
        QCOMPARE(c.state(), State::Countdown);
        c.cancel();
        h.advance(c, 5000);
        QCOMPARE(h.fake->starts, 0);
        c.startSelection(h.preferences());
        h.drain();
        h.fake->cancelled();
        h.drain();
        QCOMPARE(c.state(), State::Idle);
    }
    void discardAndLateCallbacks()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        h.start(c);
        const auto old = h.fake->event;
        const auto suspend = h.fake->suspend;
        c.discard();
        c.discard();
        c.cancel();
        c.stopAndSave();
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->discards, 1);
        h.start(c);
        RecordingEvent late;
        late.type = RecordingEventType::Finished;
        old(late);
        suspend();
        h.drain();
        QCOMPARE(c.state(), State::Recording);
        QCOMPARE(h.fake->toasts, 0);
        QCOMPARE(h.fake->observersStopped, 1);
    }
    void destructorRejectsCallbacks()
    {
        RecordingHarness h;
        {
            RecordingController c(h.dependencies());
            h.start(c);
        }
        RecordingEvent late;
        late.type = RecordingEventType::Finished;
        h.fake->event(late);
        h.fake->suspend();
        h.drain();
        QCOMPARE(h.fake->toasts, 0);
        QCOMPARE(h.fake->destroyed, 1);
    }
    void diskProtection()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        QSignalSpy notice(&c, &RecordingController::noticeChanged);
        h.start(c);
        h.fake->disk = -1;
        h.advance(c, 2000);
        h.advance(c, 2000);
        QCOMPARE(notice.size(), 1);
        h.fake->disk = 1024LL * 1024 * 1024;
        h.advance(c, 2000);
        h.advance(c, 2000);
        QCOMPARE(notice.size(), 2);
        h.fake->disk = 511LL * 1024 * 1024;
        h.advance(c, 2000);
        QCOMPARE(c.state(), State::Finalizing);
        h.event(RecordingEventType::Finished);
        QVERIFY(h.fake->lastToast.autoStopped);
        QVERIFY(h.fake->lastToast.message.contains(QStringLiteral("512")));
    }
    void windowAutoPauseAndUserPriority()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        h.start(c, RecordingTargetKind::Window);
        h.fake->window->onScreen = false;
        h.advance(c, 1000);
        QCOMPARE(c.state(), State::Recording);
        h.advance(c, 1000);
        QCOMPARE(c.state(), State::Paused);
        QVERIFY(c.autoPaused());
        h.fake->window->onScreen = true;
        h.advance(c, 1000);
        QCOMPARE(c.state(), State::Recording);
        c.pause();
        h.fake->window->onScreen = false;
        h.advance(c, 1000);
        h.advance(c, 1000);
        h.fake->window->onScreen = true;
        h.advance(c, 1000);
        QCOMPARE(c.state(), State::Paused);
        c.resume();
        QCOMPARE(c.state(), State::Recording);
        QCOMPARE(h.fake->resumes, 2);
        h.fake->window.reset();
        h.advance(c, 1000);
        QCOMPARE(c.state(), State::Finalizing);
    }
    void invalidDisplayAndEnvironment_data()
    {
        QTest::addColumn<bool>("environment");
        QTest::newRow("display") << false;
        QTest::newRow("sleep-lock") << true;
    }
    void invalidDisplayAndEnvironment()
    {
        QFETCH(bool, environment);
        RecordingHarness h;
        RecordingController c(h.dependencies());
        h.start(c);
        if (environment)
            h.fake->suspend();
        else
        {
            h.fake->matches = false;
            c.topologyChanged();
        }
        QCOMPARE(c.state(), State::Finalizing);
        QCOMPARE(h.fake->stops, 1);
        h.event(RecordingEventType::Finished);
        QVERIFY(h.fake->lastToast.autoStopped);
    }
    void quitChoicesAndSaveFailure()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        QSignalSpy ready(&c, &RecordingController::quitReady);
        h.start(c);
        c.quitRequested(RecordingController::QuitChoice::Cancel);
        QCOMPARE(c.state(), State::Recording);
        c.quitRequested(RecordingController::QuitChoice::Save);
        QCOMPARE(ready.size(), 0);
        h.event(RecordingEventType::Failed);
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(ready.size(), 0);
        h.start(c);
        c.quitRequested(RecordingController::QuitChoice::Save);
        h.event(RecordingEventType::Finished);
        QCOMPARE(ready.size(), 1);
        h.start(c);
        c.quitRequested(RecordingController::QuitChoice::Discard);
        QCOMPARE(ready.size(), 2);
        QCOMPARE(c.state(), State::Idle);
    }
    void quitBeforeStarted_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("started-finished") << 0;
        QTest::newRow("start-timeout") << 1;
        QTest::newRow("start-failed") << 2;
        QTest::newRow("permission-denied") << 3;
        QTest::newRow("started-save-failed") << 4;
        QTest::newRow("finalize-timeout") << 5;
    }
    void quitBeforeStarted()
    {
        QFETCH(int, ending);
        RecordingHarness h;
        RecordingController c(h.dependencies());
        QSignalSpy ready(&c, &RecordingController::quitReady);
        QSignalSpy failed(&c, &RecordingController::failed);
        c.startSelection(h.preferences(3));
        h.drain();
        h.select();
        h.advance(c, 3000);
        QCOMPARE(c.state(), State::Preparing);
        QCOMPARE(h.fake->starts, 1);
        if (ending != 0)
        {
            QFile temporary(c.request().tempPath);
            QVERIFY(temporary.open(QIODevice::WriteOnly));
            temporary.write("partial");
        }
        c.quitRequested(RecordingController::QuitChoice::Save);
        c.quitRequested(RecordingController::QuitChoice::Save);
        // 原生写入器尚未就绪，不能先调用 stop 导致失败后取消退出。
        QCOMPARE(h.fake->stops, 0);
        QCOMPARE(ready.size(), 0);
        if (ending == 0 || ending >= 4)
        {
            h.event(RecordingEventType::Started);
            QCOMPARE(h.fake->stops, 1);
            QCOMPARE(c.state(), State::Finalizing);
            if (ending == 5)
                h.advance(c, 30000);
            else
                h.event(ending == 0 ? RecordingEventType::Finished : RecordingEventType::Failed);
        }
        else if (ending == 1)
            h.advance(c, 10000);
        else
            h.event(ending == 2 ? RecordingEventType::Failed
                                : RecordingEventType::PermissionDenied);
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(h.fake->discards, 1);
        QCOMPARE(failed.size(), ending == 0 ? 0 : 1);
        QVERIFY(!QFileInfo::exists(h.fake->request.tempPath));
        if (ending == 1 || ending == 5)
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("超时")));
        h.event(RecordingEventType::Started);
        h.event(RecordingEventType::Finished);
        QCOMPARE(ready.size(), 1);
    }
    void defaultOutputAndPreferenceSnapshot()
    {
        RecordingHarness h;
        auto p = h.preferences();
        p.defaultOutput = RecordingOutput::Gif;
        p.gifDirectory = h.directory.path();
        p.gifFps = 15;
        p.gifMaxSeconds = 60;
        RecordingController c(h.dependencies());
        c.startSelection(p);
        p.defaultOutput = RecordingOutput::Video;
        p.gifFps = 10;
        h.drain();
        QCOMPARE(h.fake->defaultOutput, RecordingOutput::Gif);
        h.select();
        QCOMPARE(c.request().output, RecordingOutput::Gif);
        QCOMPARE(c.request().fps, 15);
        QCOMPARE(c.request().maxDurationMs, 60000);
        QVERIFY(!c.request().systemAudio && !c.request().microphoneEnabled);
    }
    void pathsAndSizes_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("region") << 0;
        QTest::newRow("window") << 1;
        QTest::newRow("display") << 2;
    }
    void pathsAndSizes()
    {
        QFETCH(int, kind);
        RecordingHarness h;
        RecordingController c(h.dependencies());
        h.start(c, RecordingTargetKind(kind));
        const auto r = c.request();
        QVERIFY(r.outputPixels.width() > 0 && r.outputPixels.width() % 2 == 0);
        QVERIFY(r.outputPixels.height() > 0 && r.outputPixels.height() % 2 == 0);
        QCOMPARE(QFileInfo(r.finalPath).absolutePath(), h.directory.path());
        QCOMPARE(QFileInfo(r.tempPath).absolutePath(), h.directory.path());
        QVERIFY(
            QFileInfo(r.tempPath).fileName().startsWith(QStringLiteral(".waibusnap-recording-")));
        QVERIFY(r.tempPath.endsWith(QStringLiteral(".mp4.part")));
        QVERIFY(r.finalPath.endsWith(QStringLiteral(".mp4")));
        QVERIFY(!QFileInfo::exists(r.tempPath));
        if (kind == 0)
            QCOMPARE(r.outputPixels, QSize(320, 240));
        if (kind == 2)
            QCOMPARE(r.sourcePixelRect, QRect(0, 0, 1600, 1200));
    }
    void gifRequestAndLifecycle_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("stop") << 0;
        QTest::newRow("auto-limit") << 1;
        QTest::newRow("disk") << 2;
        QTest::newRow("discard") << 3;
    }
    void gifRequestAndLifecycle()
    {
        QFETCH(int, ending);
        RecordingHarness h;
        auto p = h.preferences();
        p.gifDirectory = h.directory.path();
        p.gifFps = 10;
        p.gifMaxEdge = 960;
        p.gifMaxSeconds = 15;
        p.systemAudio = p.microphone = true;
        p.microphoneDeviceId = QStringLiteral("设备");
        RecordingController c(h.dependencies());
        c.startSelection(p);
        h.drain();
        RecordingSelection selection;
        selection.output = RecordingOutput::Gif;
        selection.pixelRect = QRect(0, 0, 1501, 1101);
        h.fake->selected(selection);
        h.drain();
        const auto r = c.request();
        QCOMPARE(r.output, RecordingOutput::Gif);
        QCOMPARE(r.outputPixels, recordingOutputSize(selection.pixelRect.size(),
                                                     RecordingOutput::Gif, p.resolution, 960));
        QCOMPARE(r.fps, 10);
        QCOMPARE(r.maxDurationMs, 15000);
        QVERIFY(!r.systemAudio && !r.microphoneEnabled && r.microphoneDeviceId.isEmpty());
        QCOMPARE(r.systemGain, 0);
        QCOMPARE(r.microphoneGain, 0);
        QVERIFY(r.finalPath.endsWith(QStringLiteral(".gif")));
        QVERIFY(r.tempPath.endsWith(QStringLiteral(".gif.part")));
        QCOMPARE(QFileInfo(r.finalPath).absolutePath(), h.directory.path());
        h.event(RecordingEventType::Started);
        c.pause();
        QCOMPARE(c.state(), State::Paused);
        c.resume();
        QCOMPARE(c.state(), State::Recording);
        if (ending == 3)
        {
            c.discard();
            QCOMPARE(c.state(), State::Idle);
            QCOMPARE(h.fake->discards, 1);
            return;
        }
        if (ending == 0)
            c.stopAndSave();
        if (ending == 1)
            h.event(RecordingEventType::AutoStop);
        if (ending == 2)
        {
            h.fake->disk = 1;
            h.advance(c, 2000);
        }
        QCOMPARE(c.state(), State::Finalizing);
        h.event(RecordingEventType::Finished, 2000);
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->lastToast.autoStopped, ending != 0);
        QCOMPARE(h.fake->toasts, 1);
        p.gifDirectory = h.directory.filePath(QStringLiteral("missing-gif"));
        c.startSelection(p);
        h.drain();
        h.fake->selected(selection);
        h.drain();
        QCOMPARE(c.state(), State::Idle);
        QVERIFY(!QFileInfo::exists(p.gifDirectory));
    }
    void invalidDirectoryAndRevalidation()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        auto p = h.preferences();
        p.videoDirectory = h.directory.filePath(QStringLiteral("missing"));
        c.startSelection(p);
        h.drain();
        h.select();
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->starts, 0);
        const auto sub = h.directory.filePath(QStringLiteral("countdown"));
        QVERIFY(QDir().mkpath(sub));
        p.videoDirectory = sub;
        p.countdownSeconds = 3;
        c.startSelection(p);
        h.drain();
        h.select();
        QCOMPARE(c.state(), State::Countdown);
        QVERIFY(QDir().rmdir(sub));
        h.advance(c, 3000);
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->starts, 0);
    }
    void invalidTargetsAndUnsupported()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        h.fake->supported = false;
        c.startSelection(h.preferences());
        QCOMPARE(c.state(), State::Idle);
        h.fake->supported = true;
        for (int scenario = 0; scenario < 3; ++scenario)
        {
            c.startSelection(h.preferences());
            h.drain();
            RecordingSelection s;
            s.pixelRect = QRect(0, 0, 15, 16);
            if (scenario == 1)
                s.output = RecordingOutput::Gif;
            if (scenario == 2)
            {
                s.kind = RecordingTargetKind::Window;
                h.fake->window.reset();
            }
            h.fake->selected(s);
            h.drain();
            QCOMPARE(c.state(), State::Idle);
        }
        QCOMPARE(h.fake->starts, 0);
    }
    void warningsAutoStopAndStartFailure()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        QSignalSpy notices(&c, &RecordingController::noticeChanged);
        h.start(c);
        h.event(RecordingEventType::Warning);
        QCOMPARE(c.state(), State::Recording);
        QCOMPARE(notices.size(), 1);
        h.event(RecordingEventType::AutoStop);
        QCOMPARE(c.state(), State::Finalizing);
        h.event(RecordingEventType::Finished);
        QVERIFY(h.fake->lastToast.autoStopped);
        h.fake->startAccepted = false;
        c.startSelection(h.preferences());
        h.drain();
        h.select();
        QCOMPARE(c.state(), State::Idle);
    }
    void countdownCancelQuitAndWindowLoss()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        QSignalSpy ready(&c, &RecordingController::quitReady);
        c.startSelection(h.preferences(3));
        h.drain();
        h.select();
        c.quitRequested(RecordingController::QuitChoice::Save);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(h.fake->starts, 0);
        c.startSelection(h.preferences(3));
        h.drain();
        h.select(RecordingTargetKind::Window);
        h.fake->window->onScreen = false;
        h.advance(c, 3000);
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(h.fake->starts, 0);
    }
    void synchronousFailureIsSafe()
    {
        RecordingHarness h;
        RecordingController c(h.dependencies());
        h.fake->synchronousDenied = true;
        QSignalSpy denied(&c, &RecordingController::permissionRequired);
        QSignalSpy failed(&c, &RecordingController::failed);
        c.startSelection(h.preferences());
        h.drain();
        h.select();
        QCOMPARE(c.state(), State::Idle);
        QCOMPARE(denied.size(), 1);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(h.fake->destroyed, 1);
    }
};
QTEST_MAIN(RecordingControllerTest)
#include "recording_controller_test.moc"
