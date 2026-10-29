#include "app/recording_controller.h"
#include "session/monotonic_clock.h"
#include "ui/recording_border_window.h"
#include "ui/recording_countdown_window.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QUuid>
namespace waibusnap
{
RecordingController::RecordingController(RecordingDependencies dependencies, QObject* parent)
    : QObject(parent), dependencies_(std::move(dependencies))
{
    if (!dependencies_.clock)
        dependencies_.clock = [] { return monotonicNs() / 1000000; };
    if (!dependencies_.availableBytes)
        dependencies_.availableBytes = [](const QString& path)
        {
            QStorageInfo storage(path);
            storage.refresh();
            return storage.isValid() && storage.isReady() ? storage.bytesAvailable() : qint64(-1);
        };
    if (!dependencies_.pickerFactory)
        dependencies_.pickerFactory = [](CaptureFrame frame)
        { return std::make_unique<RecordingTargetPicker>(std::move(frame)); };
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &RecordingController::poll);
}
RecordingController::~RecordingController() { reset(); }
void RecordingController::setState(State state)
{
    state_ = state;
    if (state != State::Paused && border_)
        border_->hide();
    emit stateChanged(state);
}
QString RecordingController::statusText() const
{
    switch (state_)
    {
    case State::Idle:
        return QStringLiteral("未在录制");
    case State::Preparing:
        return QStringLiteral("准备中…");
    case State::Selecting:
        return QStringLiteral("选择范围…");
    case State::Countdown:
        return QStringLiteral("%1 秒后开始").arg(countdownRemaining_);
    case State::Recording:
        return QStringLiteral("录制中 %1").arg(recordingDuration(recordedMs_));
    case State::Paused:
        return QStringLiteral("已暂停 %1").arg(recordingDuration(recordedMs_));
    case State::Finalizing:
        return QStringLiteral("正在保存…");
    }
    return {};
}
void RecordingController::startSelection(RecordingPreferences preferences)
{
    if (active())
        return;
    preferences_ = std::move(preferences);
    recordedMs_ = 0;
    lowWarned_ = diskFailureWarned_ = userPaused_ = autoPaused_ = false;
    invisibleCount_ = 0;
    autoStopReason_.clear();
    quitAfterRecording_ = false;
    request_ = {};
    const auto token = ++token_;
    setState(State::Preparing);
    if (!dependencies_.supported())
        return fail(QStringLiteral("Windows 原生录制将在后续版本接入。"));
    if (!dependencies_.hasPermission() && !dependencies_.requestPermission())
    {
        emit permissionRequired();
        return fail(QStringLiteral("尚未获得屏幕录制权限，请在系统设置 → 隐私与安全性 → "
                                   "屏幕与系统音频录制中允许 WaibuSnap，然后重试。"));
    }
    const auto display = dependencies_.cursorDisplay();
    if (!display.error.isEmpty())
        return fail(display.error);
    display_ = display.target;
    const QPointer<RecordingController> guard(this);
    dependencies_.freezeFrame(
        display_,
        [guard, token](CaptureResult result)
        {
            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard,
                [guard, token, result = std::move(result)]() mutable
                {
                    if (!guard || guard->token_ != token || guard->state_ != State::Preparing)
                        return;
                    if (result.error != CaptureError::None || result.frame.pixels.isNull())
                        return guard->fail(result.explanation.isEmpty()
                                               ? QStringLiteral("录制冻结帧获取失败。")
                                               : result.explanation);
                    if (!guard->dependencies_.stillMatches(guard->display_))
                        return guard->fail(
                            QStringLiteral("显示器布局已变化，请重新选择录制范围。"));
                    guard->picker_ = guard->dependencies_.pickerFactory(std::move(result.frame));
                    if (!guard->picker_)
                        return guard->fail(QStringLiteral("无法创建录制选择器。"));
                    guard->picker_->setDefaultOutput(guard->preferences_.defaultOutput);
                    guard->setState(State::Selecting);
                    guard->picker_->open(
                        [guard, token](RecordingSelection selection)
                        {
                            if (guard)
                                QMetaObject::invokeMethod(
                                    guard,
                                    [guard, token, selection]
                                    {
                                        if (guard)
                                            guard->selected(token, selection);
                                    },
                                    Qt::QueuedConnection);
                        },
                        [guard, token]
                        {
                            if (guard)
                                QMetaObject::invokeMethod(
                                    guard,
                                    [guard, token]
                                    {
                                        if (guard && guard->token_ == token)
                                            guard->cancel();
                                    },
                                    Qt::QueuedConnection);
                        });
                },
                Qt::QueuedConnection);
        });
}
bool RecordingController::validateDirectory()
{
    const auto directory = QFileInfo(request_.finalPath).absolutePath();
    // 首次使用只初始化内置默认目录；自定义路径失效时不擅自重建。
    const auto defaultDirectory = request_.output == RecordingOutput::Gif
                                      ? defaultRecordingGifDirectory()
                                      : defaultRecordingVideoDirectory();
    if (directory == defaultDirectory && !QFileInfo::exists(directory))
        QDir().mkpath(directory);
    const QFileInfo info(directory);
    QTemporaryFile probe(QDir(directory).filePath(QStringLiteral(".waibusnap-write-XXXXXX")));
    if (!info.isDir() || !info.isWritable() || !probe.open())
    {
        fail(QStringLiteral("录制目录不存在或不可写：%1。请重新配置录制目录。").arg(directory));
        return false;
    }
    return true;
}
void RecordingController::selected(quint64 token, RecordingSelection selection)
{
    if (token != token_ || state_ != State::Selecting)
        return;
    picker_->dismiss();
    picker_.reset();
    request_.output = selection.output;
    const bool gif = selection.output == RecordingOutput::Gif;
    request_.display = display_;
    request_.targetKind = selection.kind;
    request_.windowId = selection.windowId;
    targetFrame_ = selection.logicalFrame;
    const QSize displayPixels(
        qRound(display_.logicalGeometry.width() * display_.devicePixelRatio),
        qRound(display_.logicalGeometry.height() * display_.devicePixelRatio));
    QSize source;
    if (selection.kind == RecordingTargetKind::Window)
    {
        const auto window = dependencies_.windowInfo(selection.windowId);
        if (!window || !window->onScreen || window->logicalFrame.width() < 32 ||
            window->logicalFrame.height() < 32)
            return fail(QStringLiteral("请先让目标窗口可见，然后重新选择。"));
        targetFrame_ = window->logicalFrame;
        source = QSize(qRound(targetFrame_.width() * display_.devicePixelRatio),
                       qRound(targetFrame_.height() * display_.devicePixelRatio));
    }
    else
    {
        request_.sourcePixelRect = selection.kind == RecordingTargetKind::Display
                                       ? QRect(QPoint(), displayPixels)
                                       : selection.pixelRect;
        if (!QRect(QPoint(), displayPixels).contains(request_.sourcePixelRect) ||
            request_.sourcePixelRect.width() < 16 || request_.sourcePixelRect.height() < 16)
            return fail(QStringLiteral("录制区域须在显示器内且至少为 16×16 物理像素。"));
        source = request_.sourcePixelRect.size();
        const qreal dpr = display_.devicePixelRatio;
        targetFrame_ = QRectF(display_.logicalGeometry.x() + request_.sourcePixelRect.x() / dpr,
                              display_.logicalGeometry.y() + request_.sourcePixelRect.y() / dpr,
                              source.width() / dpr, source.height() / dpr)
                           .toAlignedRect();
    }
    request_.outputPixels = recordingOutputSize(source, selection.output, preferences_.resolution,
                                                preferences_.gifMaxEdge);
    request_.fps = gif ? preferences_.gifFps : preferences_.videoFps;
    if (!gif)
        request_.quality = preferences_.quality;
    request_.systemAudio = !gif && preferences_.systemAudio;
    request_.microphoneEnabled = !gif && preferences_.microphone;
    request_.microphoneDeviceId = gif ? QString() : preferences_.microphoneDeviceId;
    request_.systemGain = gif ? 0 : preferences_.systemVolumePercent / 100.0;
    request_.microphoneGain = gif ? 0 : preferences_.microphoneVolumePercent / 100.0;
    request_.showCursor = preferences_.showCursor;
    const auto directory = gif ? preferences_.gifDirectory : preferences_.videoDirectory;
    request_.maxDurationMs = gif ? preferences_.gifMaxSeconds * 1000 : 0;
    if ((!directory.isEmpty() && !QDir::isAbsolutePath(directory)) ||
        directory.contains(QChar::Null))
        return fail(QStringLiteral("录制目录必须是合法的绝对路径。"));
    request_.finalPath = suggestedRecordingPath(directory, selection.output, dependencies_.now());
    request_.tempPath =
        QDir(QFileInfo(request_.finalPath).absolutePath())
            .filePath(QStringLiteral(".waibusnap-recording-%1.%2.part")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces),
                               gif ? QStringLiteral("gif") : QStringLiteral("mp4")));
    if (!validateDirectory())
        return;
    const QString error = validateRecordingRequest(request_);
    if (!error.isEmpty())
        return fail(error);
    const int seconds = preferences_.countdownSeconds;
    if (seconds != 0 && seconds != 3 && seconds != 5)
        return fail(QStringLiteral("录制倒计时仅支持 0 / 3 / 5 秒。"));
    countdownRemaining_ = seconds;
    countdownEnd_ = dependencies_.clock() + seconds * 1000;
    setState(State::Countdown);
    timer_.start();
    if (seconds && dependencies_.showWindows)
    {
        countdown_ = std::make_unique<RecordingCountdownWindow>();
        connect(countdown_.get(), &RecordingCountdownWindow::cancelled, this,
                [this] { QTimer::singleShot(0, this, &RecordingController::cancel); });
        countdown_->present(targetFrame_, seconds);
    }
    if (!seconds)
        beginSession();
}
void RecordingController::beginSession()
{
    countdown_.reset();
    if (!dependencies_.stillMatches(display_))
        return fail(QStringLiteral("显示器布局已变化，请重新选择录制范围。"));
    if (request_.targetKind == RecordingTargetKind::Window)
    {
        const auto window = dependencies_.windowInfo(request_.windowId);
        if (!window || !window->onScreen)
            return fail(QStringLiteral("目标窗口已关闭或不可见，请重新选择。"));
        targetFrame_ = window->logicalFrame;
    }
    if (!validateDirectory())
        return;
    session_ = dependencies_.sessionFactory();
    if (!session_)
        return fail(QStringLiteral("当前平台暂不支持录制。"));
    setState(State::Preparing);
    const quint64 token = token_;
    const QPointer<RecordingController> guard(this);
    QString error;
    const bool started = session_->start(
        request_,
        [guard, token](RecordingEvent event)
        {
            if (guard)
                QMetaObject::invokeMethod(
                    guard,
                    [guard, token, event = std::move(event)]() mutable
                    {
                        if (guard)
                            guard->handleEvent(token, std::move(event));
                    },
                    Qt::QueuedConnection);
        },
        &error);
    if (!started)
    {
        // 同步 PermissionDenied 已入队；统一延后失败，保证 start 栈返回后再销毁会话。
        QMetaObject::invokeMethod(
            this,
            [this, token, error]
            {
                if (token == token_ && active())
                    fail(error.isEmpty() ? QStringLiteral("录制启动失败。") : error);
            },
            Qt::QueuedConnection);
        return;
    }
    environment_ = dependencies_.environmentFactory();
    if (environment_)
        environment_->start(
            [guard, token]
            {
                if (guard && guard->token_ == token)
                    guard->automaticStop(QStringLiteral("系统睡眠、锁屏或会话失活"));
            });
    nextDisk_ = dependencies_.clock();
    nextWindow_ = nextDisk_ + 1000;
    nextProgress_ = nextDisk_;
}
void RecordingController::handleEvent(quint64 token, RecordingEvent event)
{
    if (token != token_ || !active() || !session_)
        return;
    switch (event.type)
    {
    case RecordingEventType::Started:
        if (stopWhenStarted_)
        {
            stopWhenStarted_ = false;
            quitDeadline_ = dependencies_.clock() + 30000;
            session_->stop();
        }
        else if (state_ == State::Preparing)
            setState(State::Recording);
        break;
    case RecordingEventType::Progress:
        if (state_ != State::Finalizing)
        {
            recordedMs_ = qMax(recordedMs_, event.recordedMs);
            emit progressChanged(recordedMs_);
            updateBorder();
        }
        break;
    case RecordingEventType::Warning:
        emit noticeChanged(event.message);
        break;
    case RecordingEventType::AutoStop:
        automaticStop(event.message);
        break;
    case RecordingEventType::PermissionDenied:
        emit permissionRequired();
        fail(event.message);
        break;
    case RecordingEventType::Failed:
        fail(event.message, event.path);
        break;
    case RecordingEventType::Finished:
    {
        const bool quit = quitAfterRecording_;
        event.autoStopped |= !autoStopReason_.isEmpty();
        if (!autoStopReason_.isEmpty())
            event.message =
                autoStopReason_ +
                (event.message.isEmpty() ? QString() : QStringLiteral("\n") + event.message);
        reset();
        if (dependencies_.toastPresenter)
            dependencies_.toastPresenter(event, true);
        emit finished(event);
        if (quit)
            emit quitReady();
        break;
    }
    }
}
void RecordingController::pause()
{
    if (state_ != State::Recording && state_ != State::Paused)
        return;
    userPaused_ = true;
    if (state_ == State::Recording)
        session_->pause();
    setState(State::Paused);
    updateBorder();
}
void RecordingController::resume()
{
    if (state_ != State::Paused)
        return;
    userPaused_ = false;
    if (autoPaused_)
    {
        emit noticeChanged(QStringLiteral("目标窗口不可见，恢复可见后继续录制。"));
        return;
    }
    if (border_)
        border_->hide();
    session_->resume();
    setState(State::Recording);
}
void RecordingController::stopAndSave()
{
    if (!session_ || state_ == State::Finalizing || state_ == State::Idle)
        return;
    setState(State::Finalizing);
    session_->stop();
}
void RecordingController::automaticStop(const QString& reason)
{
    if (!active() || state_ == State::Finalizing)
        return;
    autoStopReason_ = reason.isEmpty() ? QStringLiteral("录制已达到停止条件") : reason;
    emit noticeChanged(autoStopReason_);
    if (session_)
        stopAndSave();
    else
        fail(autoStopReason_);
}
void RecordingController::cancel()
{
    if (state_ == State::Preparing || state_ == State::Selecting || state_ == State::Countdown)
        reset();
}
void RecordingController::discard()
{
    if (active())
        reset();
}
void RecordingController::quitRequested(QuitChoice choice)
{
    if (choice == QuitChoice::Cancel)
        return;
    if (choice == QuitChoice::Discard || !session_)
    {
        reset();
        emit quitReady();
        return;
    }
    quitAfterRecording_ = true;
    if (state_ == State::Preparing && !quitDuringStart_)
    {
        quitDuringStart_ = stopWhenStarted_ = true;
        quitDeadline_ = dependencies_.clock() + 10000;
        setState(State::Finalizing);
        emit noticeChanged(QStringLiteral("正在等待录制就绪，随后停止并保存后退出。"));
        return;
    }
    stopAndSave();
}
void RecordingController::reset()
{
    ++token_;
    timer_.stop();
    countdown_.reset();
    if (picker_)
        picker_->dismiss();
    picker_.reset();
    border_.reset();
    if (environment_)
        environment_->stop();
    environment_.reset();
    if (session_)
        session_->discard();
    session_.reset();
    quitAfterRecording_ = false;
    quitDuringStart_ = stopWhenStarted_ = false;
    quitDeadline_ = 0;
    countdownRemaining_ = 0;
    if (state_ != State::Idle)
        setState(State::Idle);
}
void RecordingController::fail(QString error, QString path)
{
    RecordingEvent event;
    event.type = RecordingEventType::Failed;
    event.message = error.isEmpty() ? QStringLiteral("录制失败，请重试。") : error;
    event.path = std::move(path);
    const bool hadSession = bool(session_);
    const bool quit = quitDuringStart_;
    const auto temporary = request_.tempPath;
    if (quit)
    {
        event.message += QStringLiteral("\n启动阶段保存退出未完成，已丢弃临时录制并退出。");
        event.path.clear();
    }
    reset();
    if (quit && QFileInfo::exists(temporary) && !QFile::remove(temporary))
        event.message += QStringLiteral("\n临时文件清理失败：%1").arg(temporary);
    emit noticeChanged(event.message);
    if (hadSession && dependencies_.toastPresenter)
        dependencies_.toastPresenter(event, false);
    emit failed(event.message);
    if (quit)
        emit quitReady();
}
void RecordingController::topologyChanged()
{
    if (active() && !dependencies_.stillMatches(display_))
        automaticStop(QStringLiteral("显示器布局变化"));
}
void RecordingController::updateBorder()
{
    if (state_ != State::Paused || !dependencies_.showWindows)
        return;
    if (!border_)
        border_ = std::make_unique<RecordingBorderWindow>();
    border_->present(targetFrame_, recordedMs_);
}
void RecordingController::poll()
{
    const auto now = dependencies_.clock();
    if (quitDuringStart_ && now >= quitDeadline_)
        return fail(stopWhenStarted_ ? QStringLiteral("等待录制启动超时（10 秒）。")
                                     : QStringLiteral("等待录制保存超时（30 秒）。"));
    if (state_ == State::Countdown)
    {
        const int remaining = int(qMax(qint64(0), countdownEnd_ - now + 999) / 1000);
        if (remaining != countdownRemaining_)
        {
            countdownRemaining_ = remaining;
            if (countdown_)
                countdown_->setSeconds(remaining);
            emit progressChanged(recordedMs_);
        }
        if (!remaining)
            beginSession();
        return;
    }
    if (state_ != State::Recording && state_ != State::Paused)
        return;
    topologyChanged();
    if (state_ == State::Finalizing)
        return;
    if (now >= nextDisk_)
    {
        nextDisk_ = now + 2000;
        const auto directory = QFileInfo(request_.finalPath).absolutePath();
        if (!QFileInfo(directory).isDir() || !QFileInfo(directory).isWritable())
            return automaticStop(QStringLiteral("录制目录已失效或不可写"));
        const auto bytes = dependencies_.availableBytes(directory);
        if (bytes < 0)
        {
            if (!diskFailureWarned_)
                emit noticeChanged(
                    QStringLiteral("无法检查录制目录的磁盘可用空间，请及时停止录制。"));
            diskFailureWarned_ = true;
        }
        else if (recordingDiskState(bytes) == RecordingDiskState::Critical)
            return automaticStop(QStringLiteral("磁盘可用空间不足 512 MiB"));
        else if (recordingDiskState(bytes) == RecordingDiskState::Low && !lowWarned_)
        {
            lowWarned_ = true;
            emit noticeChanged(QStringLiteral("磁盘可用空间不足 2 GiB，请及时停止或更换目录。"));
        }
    }
    if (request_.targetKind == RecordingTargetKind::Window && now >= nextWindow_)
    {
        nextWindow_ = now + 1000;
        const auto window = dependencies_.windowInfo(request_.windowId);
        if (!window)
            return automaticStop(QStringLiteral("目标窗口已关闭"));
        targetFrame_ = window->logicalFrame;
        if (!window->onScreen)
        {
            if (++invisibleCount_ >= 2 && !autoPaused_)
            {
                autoPaused_ = true;
                if (state_ == State::Recording)
                {
                    session_->pause();
                    setState(State::Paused);
                }
                emit noticeChanged(QStringLiteral("目标窗口不可见，已自动暂停。"));
            }
        }
        else
        {
            invisibleCount_ = 0;
            if (autoPaused_)
            {
                autoPaused_ = false;
                if (!userPaused_)
                    resume();
            }
        }
        updateBorder();
    }
    if (now >= nextProgress_)
    {
        nextProgress_ = now + 1000;
        emit progressChanged(recordedMs_);
        updateBorder();
    }
}
}
