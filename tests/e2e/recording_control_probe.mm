#include "recording_control_probe.h"
#include "app/app_settings.h"
#include "app/recording_controller.h"
#include "ui/recording_border_window.h"
#include "ui/recording_toast.h"
#import <AppKit/AppKit.h>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
using namespace waibusnap;
namespace
{
class ControlTarget final : public QWidget
{
  public:
    explicit ControlTarget(bool retina, bool stationary, bool large, bool fourK)
    {
        setWindowTitle(QStringLiteral("录制受控测试画面"));
        resize(520, 360);
        const auto* screen = QGuiApplication::screenAt(QCursor::pos());
        if (retina)
            for (const auto* candidate : QGuiApplication::screens())
                if (!screen || candidate->devicePixelRatio() > screen->devicePixelRatio())
                    screen = candidate;
        if (large)
        {
            setWindowFlag(Qt::FramelessWindowHint);
            const qreal dpr = screen ? screen->devicePixelRatio() : 1;
            setFixedSize(fourK ? QSize(qRound(3840 / dpr), qRound(2160 / dpr)) : QSize(1280, 720));
        }
        move((screen ? screen->geometry().topLeft() : QPoint()) + QPoint(100, 120));
        connect(&timer_, &QTimer::timeout, this,
                [this]
                {
                    ++counter_;
                    update();
                });
        if (!stationary)
            timer_.start(50);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(30, 70, 110));
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("WaibuSnap R4\n受控画面 %1").arg(counter_));
        p.fillRect(QRect(20 + counter_ % 350, 30, 30, 30), Qt::green);
    }

  private:
    QTimer timer_;
    int counter_ = 0;
};
bool waitFor(const std::function<bool()>& condition, int timeout = 15000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout)
        QTest::qWait(20);
    return condition();
}
}
int runRecordingControlTarget(bool retina, bool stationary, bool large, bool fourK)
{
    ControlTarget target(retina, stationary, large, fourK);
    target.show();
    target.winId();
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(target.winId());
    QTest::qWait(100);
    const auto frame = target.frameGeometry();
    QTextStream targetOutput(stdout);
    targetOutput << "TARGET " << view.window.windowNumber << ' ' << frame.x() << ' ' << frame.y()
                 << ' ' << frame.width() << ' ' << frame.height();
    if (large)
        targetOutput << ' ' << target.devicePixelRatioF();
    targetOutput << Qt::endl;
    QTimer::singleShot(90000, qApp, &QCoreApplication::quit);
    return qApp->exec();
}
int runRecordingControlProbe(const QString& report, const QString& keep, bool retina, bool gif,
                             const QString& settingsFile, bool quitDuringStart)
{
    qApp->setQuitOnLastWindowClosed(false);
    QStringList lines;
    int failures = 0;
    const auto check = [&](bool success, const QString& label)
    {
        const auto line = (success ? QStringLiteral("PASS ") : QStringLiteral("FAIL ")) + label;
        lines.append(line);
        QTextStream(stdout) << line << Qt::endl;
        failures += !success;
        return success;
    };
    const auto finish = [&]
    {
        if (!report.isEmpty())
        {
            QFile file(report);
            if (file.open(QIODevice::WriteOnly))
                QTextStream(&file) << lines.join(QLatin1Char('\n')) << '\n';
        }
        return failures ? 1 : 0;
    };
    if (!hasScreenRecordingPermission())
    {
        lines.append(QStringLiteral("SKIP 探针尚未获得屏幕录制权限；未发起授权请求。"));
        finish();
        return 77;
    }
    QTemporaryDir temporary;
    const auto directory = keep.isEmpty() ? temporary.path() : keep;
    if (!check(QDir().mkpath(directory), QStringLiteral("输出目录可用")))
        return finish();
    QProcess target;
    QStringList targetArguments{QStringLiteral("--control-target")};
    if (retina)
        targetArguments.append(QStringLiteral("--control-retina"));
    target.start(QCoreApplication::applicationFilePath(), targetArguments);
    if (!check(target.waitForStarted(5000), QStringLiteral("受控目标子进程启动")))
        return finish();
    QByteArray targetOutput;
    const bool ready = waitFor(
        [&]
        {
            targetOutput += target.readAllStandardOutput();
            return targetOutput.contains("TARGET ");
        });
    const auto fields = targetOutput.mid(targetOutput.indexOf("TARGET ") + 7).trimmed().split(' ');
    const auto number = fields.value(0).toULongLong();
    QTest::qWait(1000);
    const auto window = recordingWindowInfo(number);
    if (!check(ready && window.has_value(), QStringLiteral("CG 窗口 ID 可查询且目标非本进程")))
    {
        target.terminate();
        target.waitForFinished();
        return finish();
    }
    check(fields.size() == 5 &&
              window->logicalFrame == QRect(fields.value(1).toInt(), fields.value(2).toInt(),
                                            fields.value(3).toInt(), fields.value(4).toInt()),
          QStringLiteral("Qt frameGeometry 与 CG 左上坐标一致；Qt=%1,%2 %3×%4")
              .arg(fields.value(1).toInt())
              .arg(fields.value(2).toInt())
              .arg(fields.value(3).toInt())
              .arg(fields.value(4).toInt()));
    // SCWindow 与 CG 的 ID / 左上全局坐标核对；只记录几何，不记录标题。
    struct ContentState
    {
        bool done = false;
        SCShareableContent* content = nil;
        QString error;
    };
    auto content = std::make_shared<ContentState>();
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:NO
                                 completionHandler:^(SCShareableContent* loaded, NSError* error) {
                                   QMetaObject::invokeMethod(
                                       qApp,
                                       [content, loaded, error]
                                       {
                                           content->content = loaded;
                                           content->done = true;
                                           content->error = error ? QString::fromNSString(
                                                                        error.localizedDescription)
                                                                  : QString();
                                       },
                                       Qt::QueuedConnection);
                                 }];
    waitFor([&] { return content->done; });
    lines.append(QStringLiteral("INFO SCK 窗口数 %1，错误 %2")
                     .arg(content->content.windows.count)
                     .arg(content->error));
    bool coordinates = false;
    for (SCWindow* item in content->content.windows)
        if (item.windowID == number)
        {
            const CGRect frame = item.frame;
            lines.append(QStringLiteral("INFO SCK=%1,%2 %3×%4")
                             .arg(frame.origin.x)
                             .arg(frame.origin.y)
                             .arg(frame.size.width)
                             .arg(frame.size.height));
            coordinates =
                window->logicalFrame == QRect(qRound(frame.origin.x), qRound(frame.origin.y),
                                              qRound(frame.size.width), qRound(frame.size.height));
        }
    check(coordinates, QStringLiteral("CG / SCK windowID 与全局左上坐标一致：%1,%2 %3×%4")
                           .arg(window->logicalFrame.x())
                           .arg(window->logicalFrame.y())
                           .arg(window->logicalFrame.width())
                           .arg(window->logicalFrame.height()));
    auto displays = createDisplayTopology();
    auto capture = createCaptureProvider();
    for (const auto kind :
         {RecordingTargetKind::Region, RecordingTargetKind::Window, RecordingTargetKind::Display})
    {
        // 保持普通layer=0，才能进入真实窗口枚举；每轮重新激活目标，防止其他应用遮挡。
        NSRunningApplication* targetApp = [NSRunningApplication
            runningApplicationWithProcessIdentifier:pid_t(target.processId())];
        [NSApp yieldActivationToApplication:targetApp];
        [targetApp activateFromApplication:NSRunningApplication.currentApplication
                                   options:NSApplicationActivateAllWindows];
        QTest::qWait(300);
        RecordingDependencies d;
        d.cursorDisplay = [&]
        {
            auto result = displays->cursorDisplay();
            if (retina && result.error.isEmpty())
            {
                NSScreen* selected = NSScreen.screens.firstObject;
                for (NSScreen* candidate in NSScreen.screens)
                    if (candidate.backingScaleFactor > selected.backingScaleFactor)
                        selected = candidate;
                const CGFloat primaryTop = NSMaxY(NSScreen.screens.firstObject.frame);
                const NSRect frame = selected.frame;
                const NSRect available = selected.visibleFrame;
                result.target.id = [selected.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
                result.target.logicalGeometry =
                    QRect(qRound(frame.origin.x), qRound(primaryTop - NSMaxY(frame)),
                          qRound(frame.size.width), qRound(frame.size.height));
                result.target.devicePixelRatio = selected.backingScaleFactor;
                result.target.availableLogicalGeometry =
                    QRect(qRound(available.origin.x), qRound(primaryTop - NSMaxY(available)),
                          qRound(available.size.width), qRound(available.size.height));
            }
            return result;
        };
        d.stillMatches = [&](const DisplayTarget& display)
        { return displays->stillMatches(display); };
        d.freezeFrame = [&](const DisplayTarget& display, std::function<void(CaptureResult)> done)
        { capture->capture(display, std::move(done)); };
        RecordingTargetPicker* picker = nullptr;
        DisplayTarget selectedDisplay;
        d.pickerFactory = [&](CaptureFrame frame) -> std::unique_ptr<RecordingPicker>
        {
            selectedDisplay = frame.display;
            const auto windows = recordingWindows();
            bool listed = false;
            for (const auto& candidate : windows)
                listed |= candidate.id == number;
            lines.append(QStringLiteral("INFO 选择器枚举窗口数 %1，目标存在 %2")
                             .arg(windows.size())
                             .arg(listed));
            auto result = std::make_unique<RecordingTargetPicker>(std::move(frame), windows);
            picker = result.get();
            return result;
        };
        bool toasted = false;
        d.toastPresenter = [&](const RecordingEvent&, bool success) { toasted = success; };
        RecordingController controller(std::move(d));
        if (!settingsFile.isEmpty() && kind != RecordingTargetKind::Display)
            continue;
        RecordingPreferences preferences;
        if (settingsFile.isEmpty())
        {
            preferences.videoDirectory = directory;
            preferences.gifDirectory = directory;
            preferences.systemAudio = false;
            preferences.countdownSeconds = 3;
        }
        else
        {
            preferences = AppSettings(settingsFile).loadRecordingPreferences();
            gif = preferences.defaultOutput == RecordingOutput::Gif;
        }
        RecordingEvent result;
        QObject::connect(&controller, &RecordingController::finished, &controller,
                         [&](RecordingEvent event) { result = event; });
        QString error;
        QObject::connect(&controller, &RecordingController::failed, &controller,
                         [&](const QString& message) { error = message; });
        QElapsedTimer elapsed;
        elapsed.start();
        bool earlyQuit = false, quitReady = false;
        QObject::connect(&controller, &RecordingController::quitReady, &controller,
                         [&] { quitReady = true; });
        if (quitDuringStart)
            QObject::connect(
                &controller, &RecordingController::stateChanged, &controller,
                [&](RecordingController::State state)
                {
                    if (state == RecordingController::State::Preparing &&
                        !controller.request().tempPath.isEmpty())
                        QTimer::singleShot(
                            0, &controller,
                            [&]
                            {
                                if (controller.state() == RecordingController::State::Preparing)
                                {
                                    earlyQuit = true;
                                    controller.quitRequested(RecordingController::QuitChoice::Save);
                                }
                            });
                });
        controller.startSelection(preferences);
        if (!check(waitFor([&] { return picker || !error.isEmpty(); }) && picker,
                   QStringLiteral("模式 %1：真实冻结帧与选择器").arg(int(kind))))
            continue;
        if (gif && settingsFile.isEmpty())
            picker->findChild<QPushButton*>(QStringLiteral("recordingPickerOutputGif"))->click();
        picker->setMode(kind);
        if (kind == RecordingTargetKind::Window)
            QTest::qWait(100);
        if (kind == RecordingTargetKind::Region)
        {
            const QPoint top(120, 180), bottom(440, 400);
            QTest::mousePress(picker, Qt::LeftButton, {}, top);
            QTest::mouseRelease(picker, Qt::LeftButton, {}, bottom);
        }
        else if (kind == RecordingTargetKind::Window)
        {
            const auto latest = recordingWindowInfo(number);
            const auto local = (latest ? latest->logicalFrame : window->logicalFrame).center() -
                               selectedDisplay.logicalGeometry.topLeft();
            lines.append(QStringLiteral("INFO 显示器 %1,%2 %3×%4 DPR=%5，窗口点击 %6,%7")
                             .arg(selectedDisplay.logicalGeometry.x())
                             .arg(selectedDisplay.logicalGeometry.y())
                             .arg(selectedDisplay.logicalGeometry.width())
                             .arg(selectedDisplay.logicalGeometry.height())
                             .arg(selectedDisplay.devicePixelRatio)
                             .arg(local.x())
                             .arg(local.y()));
            QMouseEvent move(QEvent::MouseMove, QPointF(local),
                             QPointF(local + selectedDisplay.logicalGeometry.topLeft()),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(picker, &move);
            QTest::mouseClick(picker, Qt::LeftButton, {}, local);
            lines.append(QStringLiteral("INFO 命中 ID %1，目标 ID %2")
                             .arg(picker->selection().windowId)
                             .arg(number));
            check(picker->selection().windowId == number, QStringLiteral("窗口选择器命中受控目标"));
        }
        QTest::keyClick(picker, Qt::Key_Return);
        if (quitDuringStart)
        {
            check(waitFor([&] { return quitReady; }, 45000) && earlyQuit && !controller.active(),
                  QStringLiteral("Started 前请求保存退出，完成或明确丢弃退出：%1").arg(error));
            check(!QFileInfo::exists(controller.request().tempPath),
                  QStringLiteral("退出边界无临时文件"));
            continue;
        }
        const bool started = waitFor(
            [&] {
                return controller.state() == RecordingController::State::Recording ||
                       !error.isEmpty();
            });
        if (!check(started && error.isEmpty(),
                   QStringLiteral("模式 %1：倒计时后 Started（%2）").arg(int(kind)).arg(error)))
            continue;
        bool countdownExists = false;
        for (auto* widget : QApplication::topLevelWidgets())
            countdownExists |= widget->objectName() == QStringLiteral("recordingCountdown");
        check(elapsed.elapsed() >= preferences.countdownSeconds * 1000 && !countdownExists,
              QStringLiteral("倒计时结束后才采集"));
        if (!settingsFile.isEmpty())
        {
            const auto& request = controller.request();
            const QSize source(
                qRound(selectedDisplay.logicalGeometry.width() * selectedDisplay.devicePixelRatio),
                qRound(selectedDisplay.logicalGeometry.height() *
                       selectedDisplay.devicePixelRatio));
            check(request.outputPixels == recordingOutputSize(source, preferences.defaultOutput,
                                                              preferences.resolution,
                                                              preferences.gifMaxEdge) &&
                      request.fps == (gif ? preferences.gifFps : preferences.videoFps) &&
                      request.maxDurationMs == (gif ? preferences.gifMaxSeconds * 1000 : 0),
                  QStringLiteral("设置消费：%1×%2 / %3fps / 限时%4ms / 光标%5 / 增益%6,%7")
                      .arg(request.outputPixels.width())
                      .arg(request.outputPixels.height())
                      .arg(request.fps)
                      .arg(request.maxDurationMs)
                      .arg(request.showCursor)
                      .arg(request.systemGain)
                      .arg(request.microphoneGain));
        }
        const bool verifyLimit = gif && !settingsFile.isEmpty();
        if (verifyLimit)
        {
            check(waitFor([&] { return !controller.active(); },
                          preferences.gifMaxSeconds * 1000 + 15000) &&
                      result.autoStopped,
                  QStringLiteral("GIF 设置时长 %1 秒自动停止").arg(preferences.gifMaxSeconds));
        }
        else
        {
            QTest::qWait(1100);
            controller.pause();
            bool borderVisible = false;
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QStringLiteral("recordingBorder"))
                    borderVisible = widget->isVisible() &&
                                    widget->windowFlags().testFlag(Qt::WindowTransparentForInput);
            check(borderVisible, QStringLiteral("模式 %1：暂停边框可见且穿透").arg(int(kind)));
            QTest::qWait(500);
            controller.resume();
            QTest::qWait(1100);
            controller.stopAndSave();
        }
        check(waitFor([&] { return !controller.active(); }) && !result.path.isEmpty() &&
                  error.isEmpty() && toasted,
              QStringLiteral("模式 %1：控制器停止、保存、Toast 回调").arg(int(kind)));
        if (!result.path.isEmpty())
        {
            auto media = createRecordingMediaProbe()->probe(result.path);
            if (gif)
            {
                QImageReader reader(result.path);
                media.pixelSize = reader.size();
                media.durationMs = 0;
                media.valid = true;
                while (!reader.read().isNull())
                    media.durationMs += reader.nextImageDelay();
            }
            const bool durationMatches =
                verifyLimit ? qAbs(media.durationMs - preferences.gifMaxSeconds * 1000) <= 10
                            : (media.durationMs >= 1700 && media.durationMs < 3300);
            check(media.valid && durationMatches && media.pixelSize == result.pixelSize,
                  QStringLiteral("模式 %1：成片 %2 ms，%3×%4；时长与设置 / 暂停规则一致")
                      .arg(int(kind))
                      .arg(media.durationMs)
                      .arg(media.pixelSize.width())
                      .arg(media.pixelSize.height()));
            check(!QFileInfo::exists(controller.request().tempPath),
                  QStringLiteral("临时文件已收口"));
        }
    }
    target.terminate();
    target.waitForFinished(5000);
    return finish();
}
