#include "platform/macos/recording_services.h"
#include "core/gif_encoder.h"
#include "interfaces/recording.h"
#include "platform/macos/recording_audio_pipeline.h"
#include "session/monotonic_clock.h"
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <dispatch/dispatch.h>
#include <limits>
#include <memory>
#include <unistd.h>

// SCStream 的 sampleHandlerQueue 只接受实现了 SCStreamOutput 的对象；画面与声音各用一个。
@interface WaibuSnapRecordingStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, copy) void (^handler)(CMSampleBufferRef sampleBuffer);
@end

@implementation WaibuSnapRecordingStreamOutput
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    (void)type;
    // 回调期间即使会话释放了 stream / output，也要保证 self 活到方法返回。
    WaibuSnapRecordingStreamOutput* keepAlive = self;
    @autoreleasepool
    {
        if (self.handler)
            self.handler(sampleBuffer);
    }
    (void)keepAlive;
}
@end

// delegate 与停止队列并发时，用局部强引用保证会话 / handler 活到本次回调结束。
@interface WaibuSnapRecordingMicrophoneOutput
    : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
@property(nonatomic, weak) AVCaptureSession* session;
@property(nonatomic, copy) void (^handler)(CMSampleBufferRef sample, CMTime hostPts);
@end
@implementation WaibuSnapRecordingMicrophoneOutput
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection
{
    (void)output;
    (void)connection;
    __attribute__((objc_precise_lifetime)) WaibuSnapRecordingMicrophoneOutput* keepAlive = self;
    AVCaptureSession* session = keepAlive.session;
    const auto handler = keepAlive.handler;
    if (!session || !handler)
        return;
    @autoreleasepool
    {
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        CMClockRef clock = session.synchronizationClock;
        if (clock)
            pts = CMSyncConvertTime(pts, clock, CMClockGetHostTimeClock());
        handler(sample, pts);
    }
}
@end

namespace waibusnap
{
namespace
{
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioBitRate = 128000;
constexpr size_t kMaxPendingAudioBuffers = 32;
constexpr char kRecordingQueueName[] = "local.waibusnap.recording";

QString permissionExplanation()
{
    return QStringLiteral("尚未获得屏幕录制权限，请在系统设置 → 隐私与安全性 → "
                          "屏幕与系统音频录制中允许 WaibuSnap，然后重试；系统要求时请重启应用。") +
           QStringLiteral("若已授权仍失败：macOS 27 起请从访达或用 open 命令启动应用"
                          "（从终端直接执行不会继承授权）。");
}

struct MicrophoneCapture
{
    std::atomic<bool> cancelled{false};
    dispatch_queue_t __strong queue =
        dispatch_queue_create("local.waibusnap.recording.microphone", DISPATCH_QUEUE_SERIAL);
    // 以下对象只在 queue 上访问；delegate 的样本交付到 RecordingState::workQueue。
    AVCaptureSession* __strong session = nil;
    AVCaptureAudioDataOutput* __strong output = nil;
    WaibuSnapRecordingMicrophoneOutput* __strong delegate = nil;
    id __strong runtimeObserver = nil;
    id __strong disconnectObserver = nil;
};

struct GifPipeline
{
    dispatch_queue_t __strong queue =
        dispatch_queue_create("local.waibusnap.recording.gif", DISPATCH_QUEUE_SERIAL);
    GifEncoder encoder;
};
struct RecordingState
{
    RecordingRequest request;
    std::shared_ptr<GifPipeline> gif;
    QImage latestGifFrame;
    bool gifBusy = false;
    bool gifLimitReached = false;
    RecordingSession::EventCallback callback;
    std::atomic<bool> destroyed{false};
    std::atomic<bool> terminal{false};
    std::atomic<bool> acceptingSamples{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> discardRequested{false};
    std::atomic<qint64> pauseStartNs{0};
    std::atomic<qint64> pausedTotalNs{0};
    std::atomic<int> droppedFrames{0};

    dispatch_queue_t __strong workQueue = nil;
    dispatch_source_t __strong frameTimer = nil;
    SCStream* __strong stream = nil;
    SCContentFilter* __strong filter = nil;
    std::shared_ptr<MicrophoneCapture> microphone;
    RecordingAudioConverter systemConverter;
    RecordingAudioConverter microphoneConverter;
    RecordingAudioMixer mixer;
    bool systemAvailable = false;
    bool microphoneAvailable = false;
    bool microphoneWarningSent = false;
    bool systemWarningSent = false;
    bool audioOverflowWarningSent = false;
    CMTime lastMixedEnd = kCMTimeInvalid;
    CMTime minimumSamplePts = kCMTimeInvalid;
    AVAssetWriter* __strong writer = nil;
    AVAssetWriterInput* __strong videoInput = nil;
    AVAssetWriterInput* __strong audioInput = nil;
    WaibuSnapRecordingStreamOutput* __strong screenOutput = nil;
    WaibuSnapRecordingStreamOutput* __strong audioOutput = nil;

    // 以下字段只在 workQueue 上读写。
    bool firstPtsSet = false;
    CMTime firstPts = kCMTimeZero;
    CMTime lastVideoPts = kCMTimeInvalid;
    qint64 pausedBaseNs = 0;
    qint64 timelineStartHostNs = 0;
    qint64 lastRealFrameHostNs = 0;
    qint64 lastRecordedMs = 0;
    qint64 lastProgressMs = 0;
    bool writingStarted = false;
    CMSampleBufferRef lastVideoSample = nullptr;
    std::deque<CMSampleBufferRef> pendingAudio;

    ~RecordingState()
    {
        if (lastVideoSample)
            CFRelease(lastVideoSample);
        for (CMSampleBufferRef buffer : pendingAudio)
            CFRelease(buffer);
        pendingAudio.clear();
    }
};

void postEvent(const std::shared_ptr<RecordingState>& state, RecordingEvent event)
{
    if (!QCoreApplication::instance())
        return;
    const std::weak_ptr<RecordingState> weak = state;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [weak, event = std::move(event)]() mutable
        {
            const auto locked = weak.lock();
            if (!locked || locked->destroyed.load(std::memory_order_acquire))
                return;
            if (locked->callback)
                locked->callback(std::move(event));
        },
        Qt::QueuedConnection);
}

qint64 fileSize(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() ? info.size() : 0;
}

bool flushFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    (void)file.flush();
    const int descriptor = file.handle();
    if (descriptor >= 0)
        (void)::fsync(descriptor);
    file.close();
    return true;
}

void stopMicrophone(const std::shared_ptr<RecordingState>& state)
{
    state->microphoneAvailable = false;
    state->microphoneConverter.reset();
    state->mixer.setSources(state->systemAvailable, false);
    const auto capture = std::move(state->microphone);
    if (!capture)
        return;
    capture->cancelled.store(true, std::memory_order_release);
    dispatch_async(capture->queue, ^{
      @autoreleasepool
      {
          NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
          if (capture->runtimeObserver)
              [center removeObserver:capture->runtimeObserver];
          if (capture->disconnectObserver)
              [center removeObserver:capture->disconnectObserver];
          capture->runtimeObserver = nil;
          capture->disconnectObserver = nil;
          [capture->output setSampleBufferDelegate:nil queue:nullptr];
          [capture->session stopRunning];
          capture->delegate = nil;
          capture->output = nil;
          capture->session = nil;
      }
    });
}

void releaseCaptureObjects(const std::shared_ptr<RecordingState>& state)
{
    if (state->gif)
    {
        const auto gif = std::move(state->gif);
        dispatch_async(gif->queue, ^{
          gif->encoder.cancel();
        });
    }
    state->latestGifFrame = {};
    stopMicrophone(state);
    state->mixer.reset();
    state->systemConverter.reset();
    for (CMSampleBufferRef buffer : state->pendingAudio)
        CFRelease(buffer);
    state->pendingAudio.clear();
    if (state->frameTimer)
    {
        dispatch_source_cancel(state->frameTimer);
        state->frameTimer = nil;
    }
    if (state->lastVideoSample)
    {
        CFRelease(state->lastVideoSample);
        state->lastVideoSample = nullptr;
    }
    if (state->stream)
    {
        [state->stream stopCaptureWithCompletionHandler:nil];
        state->stream = nil;
    }
    state->filter = nil;
    state->screenOutput = nil;
    state->audioOutput = nil;
    state->videoInput = nil;
    state->audioInput = nil;
    state->writer = nil;
}

void failSession(const std::shared_ptr<RecordingState>& state, const QString& message,
                 bool keepTemp = false)
{
    if (state->terminal.exchange(true))
        return;
    state->acceptingSamples.store(false, std::memory_order_release);
    // 可能由 SCStream 输出回调触发；把释放对象推迟到当前回调返回之后。
    const auto cleanupState = state;
    const QString tempToRemove = keepTemp ? QString() : state->request.tempPath;
    dispatch_async(state->workQueue, ^{
      @autoreleasepool
      {
          if (cleanupState->writer && cleanupState->writer.status == AVAssetWriterStatusWriting)
              [cleanupState->writer cancelWriting];
          releaseCaptureObjects(cleanupState);
          if (!tempToRemove.isEmpty())
              QFile::remove(tempToRemove);
      }
    });
    RecordingEvent event;
    event.type = RecordingEventType::Failed;
    event.recordedMs = state->lastRecordedMs;
    event.path = keepTemp ? state->request.tempPath : QString();
    event.pixelSize = state->request.outputPixels;
    event.droppedFrames = state->droppedFrames.load(std::memory_order_relaxed);
    event.message = message;
    postEvent(state, std::move(event));
}

void discardSession(const std::shared_ptr<RecordingState>& state)
{
    if (state->terminal.exchange(true))
        return;
    state->acceptingSamples.store(false, std::memory_order_release);
    if (state->writer && state->writer.status == AVAssetWriterStatusWriting)
        [state->writer cancelWriting];
    releaseCaptureObjects(state);
    QFile::remove(state->request.tempPath);
}

void emitWarning(const std::shared_ptr<RecordingState>& state, const QString& message)
{
    RecordingEvent event;
    event.type = RecordingEventType::Warning;
    event.pixelSize = state->request.outputPixels;
    event.message = message;
    postEvent(state, std::move(event));
}

void maybeEmitProgress(const std::shared_ptr<RecordingState>& state)
{
    if (state->lastRecordedMs - state->lastProgressMs < 1000)
        return;
    state->lastProgressMs = state->lastRecordedMs;
    RecordingEvent event;
    event.type = RecordingEventType::Progress;
    event.recordedMs = state->lastRecordedMs;
    event.bytes = fileSize(state->request.tempPath);
    event.pixelSize = state->request.outputPixels;
    event.droppedFrames = state->droppedFrames.load(std::memory_order_relaxed);
    postEvent(state, std::move(event));
}

void tickStaticFrame(const std::shared_ptr<RecordingState>& state);

void startStaticFrameTimer(const std::shared_ptr<RecordingState>& state)
{
    if (state->frameTimer)
        return;
    dispatch_source_t timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, state->workQueue);
    if (!timer)
        return;
    const uint64_t interval = uint64_t(NSEC_PER_SEC) / uint64_t(std::max(1, state->request.fps));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, interval), interval,
                              interval / 2);
    const auto timerState = state;
    dispatch_source_set_event_handler(timer, ^{
      @autoreleasepool
      {
          tickStaticFrame(timerState);
      }
    });
    state->frameTimer = timer;
    dispatch_resume(timer);
}

bool ensureWriterStarted(const std::shared_ptr<RecordingState>& state)
{
    if (state->writingStarted)
        return true;
    if (!state->writer || ![state->writer startWriting])
    {
        failSession(state, QStringLiteral("视频写入器启动失败，未生成文件。"));
        return false;
    }
    [state->writer startSessionAtSourceTime:kCMTimeZero];
    state->writingStarted = true;
    startStaticFrameTimer(state);
    return true;
}

bool appendVideoBuffer(const std::shared_ptr<RecordingState>& state, CMSampleBufferRef sampleBuffer)
{
    CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (!state->firstPtsSet)
    {
        state->firstPts = presentation;
        state->firstPtsSet = true;
        state->timelineStartHostNs = monotonicNs();
        // 首次采集前若发生过暂停，不应从时间轴里扣掉那段暂停。
        state->pausedBaseNs = state->pausedTotalNs.load(std::memory_order_acquire);
    }
    const qint64 pausedNs = std::max<qint64>(
        0, state->pausedTotalNs.load(std::memory_order_acquire) - state->pausedBaseNs);
    CMTime adjusted = CMTimeSubtract(presentation, state->firstPts);
    adjusted = CMTimeSubtract(adjusted, CMTimeMake(pausedNs, 1000000000));
    if (!CMTIME_IS_NUMERIC(adjusted) || CMTimeCompare(adjusted, kCMTimeZero) < 0)
        return true;
    if (CMTIME_IS_VALID(state->lastVideoPts) && CMTimeCompare(adjusted, state->lastVideoPts) <= 0)
    {
        // 静止补帧可能已把时间轴推到真实帧之前；保留真实内容并顺延一帧，不能丢掉变化后的第一帧。
        adjusted = CMTimeAdd(state->lastVideoPts, CMTimeMake(1, state->request.fps));
    }
    if (!ensureWriterStarted(state))
        return false;
    if (!state->videoInput.isReadyForMoreMediaData)
    {
        state->droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    CMSampleTimingInfo timing = {CMTimeMake(1, state->request.fps), adjusted, kCMTimeInvalid};
    CMSampleBufferRef copy = nullptr;
    if (CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sampleBuffer, 1, &timing,
                                              &copy) != noErr ||
        !copy)
    {
        failSession(state, QStringLiteral("画面时间戳重写失败，录制已停止。"));
        return false;
    }
    const bool appended = [state->videoInput appendSampleBuffer:copy];
    CFRelease(copy);
    if (!appended)
    {
        failSession(state, QStringLiteral("视频写入失败，录制已停止；临时文件已清理。"));
        return false;
    }
    if (state->lastVideoSample)
        CFRelease(state->lastVideoSample);
    state->lastVideoSample = sampleBuffer;
    CFRetain(state->lastVideoSample);
    state->lastVideoPts = adjusted;
    state->lastRealFrameHostNs = monotonicNs();
    const qint64 frameMs =
        qRound64(CMTimeGetSeconds(adjusted) * 1000.0) + 1000 / state->request.fps;
    state->lastRecordedMs = std::max(state->lastRecordedMs, frameMs);
    maybeEmitProgress(state);
    return true;
}

void drainMixedAudio(const std::shared_ptr<RecordingState>& state, bool force = false);
void drainPendingAudio(const std::shared_ptr<RecordingState>& state);
bool acceptingSamplesAfter(const std::shared_ptr<RecordingState>& state);

void tickStaticFrame(const std::shared_ptr<RecordingState>& state)
{
    if (!acceptingSamplesAfter(state) || !state->writingStarted || !state->lastVideoSample ||
        !CMTIME_IS_VALID(state->lastVideoPts))
        return;
    const qint64 frameNs = qint64(1000000000) / std::max(1, state->request.fps);
    const qint64 now = monotonicNs();
    // 有真实帧持续到达时不补帧；只有画面静止超过一帧才按时间轴复制上一帧。
    if (now - state->lastRealFrameHostNs < frameNs * 3 / 2)
        return;
    const qint64 pausedNs = std::max<qint64>(
        0, state->pausedTotalNs.load(std::memory_order_acquire) - state->pausedBaseNs);
    const qint64 elapsedNs = now - state->timelineStartHostNs - pausedNs;
    if (elapsedNs <= 0)
        return;
    const CMTime adjusted = CMTimeMake(elapsedNs, 1000000000);
    if (CMTimeCompare(adjusted, state->lastVideoPts) <= 0)
        return;
    if (!state->videoInput.isReadyForMoreMediaData)
        return;
    CMSampleTimingInfo timing = {CMTimeMake(1, state->request.fps), adjusted, kCMTimeInvalid};
    CMSampleBufferRef copy = nullptr;
    if (CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, state->lastVideoSample, 1,
                                              &timing, &copy) != noErr ||
        !copy)
        return;
    const bool appended = [state->videoInput appendSampleBuffer:copy];
    CFRelease(copy);
    if (!appended)
    {
        failSession(state, QStringLiteral("静止画面补帧失败，录制已停止；临时文件已清理。"));
        return;
    }
    state->lastVideoPts = adjusted;
    const qint64 frameMs =
        qRound64(CMTimeGetSeconds(adjusted) * 1000.0) + 1000 / state->request.fps;
    state->lastRecordedMs = std::max(state->lastRecordedMs, frameMs);
    maybeEmitProgress(state);
    drainPendingAudio(state);
}

void drainPendingAudio(const std::shared_ptr<RecordingState>& state)
{
    while (!state->pendingAudio.empty() && state->audioInput &&
           state->audioInput.isReadyForMoreMediaData)
    {
        CMSampleBufferRef buffer = state->pendingAudio.front();
        state->pendingAudio.pop_front();
        if (![state->audioInput appendSampleBuffer:buffer])
        {
            CFRelease(buffer);
            failSession(state, QStringLiteral("音频写入失败，录制已停止；临时文件已清理。"));
            return;
        }
        CFRelease(buffer);
    }
}

void appendAudioBuffer(const std::shared_ptr<RecordingState>& state, CMSampleBufferRef sampleBuffer)
{
    if (!state->audioInput || !state->firstPtsSet)
        return;
    const qint64 pausedNs = std::max<qint64>(
        0, state->pausedTotalNs.load(std::memory_order_acquire) - state->pausedBaseNs);
    CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    CMTime adjusted = CMTimeSubtract(presentation, state->firstPts);
    adjusted = CMTimeSubtract(adjusted, CMTimeMake(pausedNs, 1000000000));
    if (!CMTIME_IS_NUMERIC(adjusted) || CMTimeCompare(adjusted, kCMTimeZero) < 0)
        return;
    // PCM 的 timing.duration 是单个采样帧时长，不能填整个缓冲时长。
    CMSampleTimingInfo timing = {CMTimeMake(1, kAudioSampleRate), adjusted, kCMTimeInvalid};
    CMSampleBufferRef copy = nullptr;
    if (CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sampleBuffer, 1, &timing,
                                              &copy) != noErr ||
        !copy)
        return;
    if (state->pendingAudio.empty() && state->audioInput.isReadyForMoreMediaData)
    {
        const bool appended = [state->audioInput appendSampleBuffer:copy];
        CFRelease(copy);
        if (!appended)
            failSession(state, QStringLiteral("音频写入失败，录制已停止；临时文件已清理。"));
        return;
    }
    state->pendingAudio.push_back(copy);
    while (state->pendingAudio.size() > kMaxPendingAudioBuffers)
    {
        CFRelease(state->pendingAudio.front());
        state->pendingAudio.pop_front();
    }
    drainPendingAudio(state);
}

bool acceptingSamplesAfter(const std::shared_ptr<RecordingState>& state)
{
    return state->acceptingSamples.load(std::memory_order_acquire) &&
           !state->terminal.load(std::memory_order_acquire) &&
           !state->paused.load(std::memory_order_acquire);
}

QImage imageFromCGImage(CGImageRef image);
void tickGif(std::shared_ptr<RecordingState> state)
{
    if (!acceptingSamplesAfter(state) || state->gifLimitReached || state->latestGifFrame.isNull())
        return;
    if (state->gifBusy)
    {
        state->droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state->gifBusy = true;
    const QImage frame = state->latestGifFrame;
    const auto gif = state->gif;
    dispatch_async(gif->queue, ^{
      @autoreleasepool
      {
          QString error;
          const bool ok = !state->terminal.load(std::memory_order_acquire) &&
                          gif->encoder.addFrame(frame, &error);
          const qint64 frames = gif->encoder.frameCount();
          dispatch_async(state->workQueue, ^{
            state->gifBusy = false;
            if (state->terminal.load(std::memory_order_acquire))
                return;
            if (!ok)
            {
                failSession(state, error);
                return;
            }
            state->lastRecordedMs = frames * 1000 / state->request.fps;
            maybeEmitProgress(state);
            if (!state->gifLimitReached &&
                frames * 1000 >= qint64(state->request.maxDurationMs) * state->request.fps)
            {
                state->gifLimitReached = true;
                RecordingEvent event;
                event.type = RecordingEventType::AutoStop;
                event.recordedMs = state->lastRecordedMs;
                event.autoStopped = true;
                event.message = QStringLiteral("GIF 已达到最长时长");
                postEvent(state, std::move(event));
            }
          });
      }
    });
}
void startGifTimer(std::shared_ptr<RecordingState> state)
{
    if (state->frameTimer)
        return;
    auto timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, state->workQueue);
    if (!timer)
    {
        failSession(state, QStringLiteral("无法创建GIF帧定时器。"));
        return;
    }
    const uint64_t interval = uint64_t(NSEC_PER_SEC) / uint64_t(state->request.fps);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, interval), interval,
                              interval / 20);
    dispatch_source_set_event_handler(timer, ^{
      tickGif(state);
    });
    state->frameTimer = timer;
    dispatch_resume(timer);
}

void handleScreenSample(const std::shared_ptr<RecordingState>& state,
                        CMSampleBufferRef sampleBuffer)
{
    if (!acceptingSamplesAfter(state))
        return;
    if (!CMSampleBufferGetImageBuffer(sampleBuffer))
        return;
    if (CMTIME_IS_NUMERIC(state->minimumSamplePts) &&
        CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                      state->minimumSamplePts) < 0)
        return;
    if (state->request.output == RecordingOutput::Gif)
    {
        CGImageRef image = nullptr;
        const OSStatus status = VTCreateCGImageFromCVPixelBuffer(
            CMSampleBufferGetImageBuffer(sampleBuffer), nullptr, &image);
        if (status != noErr || !image)
        {
            failSession(state, QStringLiteral("GIF画面转换失败，录制已停止。"));
            return;
        }
        state->latestGifFrame = imageFromCGImage(image);
        CGImageRelease(image);
        if (state->latestGifFrame.isNull() ||
            state->latestGifFrame.size() != state->request.outputPixels)
        {
            failSession(state, QStringLiteral("GIF画面为空或尺寸不一致。"));
            return;
        }
        startGifTimer(state);
        return;
    }
    if (!appendVideoBuffer(state, sampleBuffer))
        return;
    drainPendingAudio(state);
}

void disableMicrophone(const std::shared_ptr<RecordingState>& state, const QString& reason)
{
    stopMicrophone(state);
    if (!state->microphoneWarningSent)
    {
        state->microphoneWarningSent = true;
        emitWarning(state, reason + QStringLiteral("，本次录制将继续使用系统声音或静音。"));
    }
}

void disableSystemAudio(const std::shared_ptr<RecordingState>& state)
{
    state->systemAvailable = false;
    state->systemConverter.reset();
    state->mixer.setSources(false, state->microphoneAvailable);
    if (!state->systemWarningSent)
    {
        state->systemWarningSent = true;
        emitWarning(state,
                    QStringLiteral("系统声音采集或格式转换失败，本次录制将继续使用麦克风或静音。"));
    }
}

void drainMixedAudio(const std::shared_ptr<RecordingState>& state, bool force)
{
    while (!state->terminal.load(std::memory_order_acquire))
    {
        auto chunk = state->mixer.take(force);
        if (!chunk)
            break;
        // 主时钟切换时裁掉已经写过的部分，防止重复或倒退。
        if (CMTIME_IS_NUMERIC(state->lastMixedEnd) &&
            CMTimeCompare(chunk->pts, state->lastMixedEnd) < 0)
        {
            const qsizetype skip = std::min(
                chunk->frames(),
                qRound64(CMTimeGetSeconds(CMTimeSubtract(state->lastMixedEnd, chunk->pts)) *
                         kAudioSampleRate));
            chunk->samples.erase(chunk->samples.begin(),
                                 chunk->samples.begin() + skip * kAudioChannels);
            chunk->pts = CMTimeAdd(chunk->pts, CMTimeMake(skip, kAudioSampleRate));
        }
        if (chunk->frames() == 0)
            continue;
        CMSampleBufferRef sample = createRecordingPcmSample(*chunk);
        if (!sample)
        {
            stopMicrophone(state);
            state->systemAvailable = false;
            state->systemConverter.reset();
            state->mixer.setSources(false, false);
            emitWarning(state, QStringLiteral("音频混音缓冲创建失败，已降级为静音画面录制。"));
            break;
        }
        state->lastMixedEnd = CMTimeAdd(chunk->pts, CMTimeMake(chunk->frames(), kAudioSampleRate));
        appendAudioBuffer(state, sample);
        CFRelease(sample);
    }
    if (state->mixer.takeOverflowed() && !state->audioOverflowWarningSent)
    {
        state->audioOverflowWarningSent = true;
        emitWarning(state, QStringLiteral("音频缓冲积压，已丢弃最旧样本并重新对齐；录制继续。"));
    }
}

void handleAudioSample(const std::shared_ptr<RecordingState>& state, CMSampleBufferRef sampleBuffer)
{
    if (!acceptingSamplesAfter(state) || !state->systemAvailable || !state->audioInput)
        return;
    if (CMTIME_IS_NUMERIC(state->minimumSamplePts) &&
        CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                      state->minimumSamplePts) < 0)
        return;
    RecordingPcmChunk chunk;
    if (!state->systemConverter.convert(
            sampleBuffer, CMSampleBufferGetPresentationTimeStamp(sampleBuffer), &chunk))
        disableSystemAudio(state);
    else
        state->mixer.pushSystem(std::move(chunk));
    drainMixedAudio(state);
}

void handleMicrophoneSample(const std::shared_ptr<RecordingState>& state,
                            CMSampleBufferRef sampleBuffer, CMTime pts)
{
    if (!acceptingSamplesAfter(state) || !state->microphoneAvailable || !state->audioInput)
        return;
    if (CMTIME_IS_NUMERIC(state->minimumSamplePts) &&
        CMTimeCompare(pts, state->minimumSamplePts) < 0)
        return;
    RecordingPcmChunk chunk;
    if (!state->microphoneConverter.convert(sampleBuffer, pts, &chunk))
        disableMicrophone(state, QStringLiteral("麦克风格式转换失败"));
    else
        state->mixer.pushMicrophone(std::move(chunk));
    drainMixedAudio(state);
}

AVCaptureDeviceDiscoverySession* audioDiscovery()
{
    return [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[
            AVCaptureDeviceTypeMicrophone, AVCaptureDeviceTypeExternal
        ]
                              mediaType:AVMediaTypeAudio
                               position:AVCaptureDevicePositionUnspecified];
}

AVCaptureDevice* selectedMicrophone(const QString& id)
{
    if (id.isEmpty())
        return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    for (AVCaptureDevice* device in audioDiscovery().devices)
        if (QString::fromNSString(device.uniqueID) == id)
            return device;
    return nil;
}

bool sessionEnding(const std::shared_ptr<RecordingState>& state)
{
    return state->terminal.load(std::memory_order_acquire) ||
           state->stopRequested.load(std::memory_order_acquire) ||
           state->discardRequested.load(std::memory_order_acquire);
}

void startMicrophone(const std::shared_ptr<RecordingState>& state, AVCaptureDevice* device)
{
    if (sessionEnding(state))
        return;
    const auto capture = std::make_shared<MicrophoneCapture>();
    state->microphone = capture;
    const std::weak_ptr<RecordingState> weak = state;
    const auto workQueue = state->workQueue;
    dispatch_async(capture->queue, ^{
      @autoreleasepool
      {
          if (capture->cancelled.load(std::memory_order_acquire))
              return;
          AVCaptureSession* session = [[AVCaptureSession alloc] init];
          NSError* error = nil;
          AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                                              error:&error];
          AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
          WaibuSnapRecordingMicrophoneOutput* delegate =
              [[WaibuSnapRecordingMicrophoneOutput alloc] init];
          delegate.session = session;
          delegate.handler = ^(CMSampleBufferRef sample, CMTime pts) {
            const auto locked = weak.lock();
            if (locked)
                handleMicrophoneSample(locked, sample, pts);
          };
          [session beginConfiguration];
          const bool configured =
              input && !error && [session canAddInput:input] && [session canAddOutput:output];
          if (configured)
          {
              [session addInput:input];
              [session addOutput:output];
              [output setSampleBufferDelegate:delegate queue:workQueue];
          }
          [session commitConfiguration];
          capture->session = session;
          capture->output = output;
          capture->delegate = delegate;
          NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
          const auto notifyFailure = ^(NSNotification* notification) {
            (void)notification;
            const auto locked = weak.lock();
            if (!locked)
                return;
            dispatch_async(workQueue, ^{
              if (!sessionEnding(locked) && locked->microphoneAvailable)
                  disableMicrophone(locked, QStringLiteral("麦克风已断开或采集异常，麦克风已停用"));
            });
          };
          capture->runtimeObserver =
              [center addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                  object:session
                                   queue:nil
                              usingBlock:notifyFailure];
          capture->disconnectObserver =
              [center addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                                  object:device
                                   queue:nil
                              usingBlock:notifyFailure];
          if (configured && !capture->cancelled.load(std::memory_order_acquire))
              [session startRunning];
          const bool running = configured && session.isRunning;
          dispatch_async(workQueue, ^{
            const auto locked = weak.lock();
            if (locked && !sessionEnding(locked) && !running)
                disableMicrophone(locked, QStringLiteral("麦克风初始化失败"));
          });
      }
    });
}

bool prepareMicrophone(const std::shared_ptr<RecordingState>& state)
{
    if (!state->request.microphoneEnabled)
        return false;
    AVCaptureDevice* device = selectedMicrophone(state->request.microphoneDeviceId);
    if (!device)
    {
        disableMicrophone(state, QStringLiteral("所选麦克风不可用或已断开"));
        return false;
    }
    const auto permission = microphonePermission();
    if (permission == MicrophonePermission::Denied)
    {
        disableMicrophone(state, QStringLiteral("麦克风未授权"));
        return false;
    }
    state->microphoneAvailable = true;
    if (permission == MicrophonePermission::Authorized)
        startMicrophone(state, device);
    else
    {
        // 只预留一个 AAC 输入，视频立即开始；拒绝 / 超时没有音频样本时不会建空轨。
        const std::weak_ptr<RecordingState> weak = state;
        requestMicrophonePermission(
            [weak, device](bool granted)
            {
                const auto locked = weak.lock();
                if (!locked)
                    return;
                dispatch_async(locked->workQueue, ^{
                  if (sessionEnding(locked))
                      return;
                  if (!granted)
                      disableMicrophone(locked, QStringLiteral("麦克风未授权或等待授权超时"));
                  else if (locked->microphoneAvailable)
                      startMicrophone(locked, device);
                });
            });
    }
    return true;
}

void resetAudioForPause(const std::shared_ptr<RecordingState>& state)
{
    state->mixer.reset();
    state->systemConverter.reset();
    state->microphoneConverter.reset();
    for (CMSampleBufferRef buffer : state->pendingAudio)
        CFRelease(buffer);
    state->pendingAudio.clear();
}

void drainPendingAudioBeforeFinish(const std::shared_ptr<RecordingState>& state)
{
    // 写入器可能刚好在切换 ready 状态；有限等待，避免无界缓冲。
    for (int attempt = 0; attempt < 100 && !state->pendingAudio.empty(); ++attempt)
    {
        drainPendingAudio(state);
        if (state->pendingAudio.empty() || state->terminal.load(std::memory_order_acquire))
            return;
        ::usleep(2000);
    }
}

void finalizeWriter(const std::shared_ptr<RecordingState>& state)
{
    if (state->terminal.load(std::memory_order_acquire))
    {
        if (state->discardRequested.load(std::memory_order_acquire))
            QFile::remove(state->request.tempPath);
        return;
    }
    const AVAssetWriterStatus status =
        state->writer ? state->writer.status : AVAssetWriterStatusFailed;
    if (state->request.output != RecordingOutput::Gif && status != AVAssetWriterStatusCompleted)
    {
        failSession(state,
                    QStringLiteral("视频写入未完成（状态 %1），未生成文件。").arg(int(status)));
        return;
    }
    releaseCaptureObjects(state);
    if (state->discardRequested.load(std::memory_order_acquire))
    {
        state->terminal.store(true, std::memory_order_release);
        QFile::remove(state->request.tempPath);
        return;
    }
    const QString tempPath = state->request.tempPath;
    const QString finalPath = state->request.finalPath;
    if (!flushFile(tempPath))
    {
        failSession(state, QStringLiteral("临时文件刷新失败，已保留可恢复文件：%1").arg(tempPath),
                    true);
        return;
    }
    if (QFileInfo::exists(finalPath))
    {
        failSession(state,
                    QStringLiteral("成品路径已存在，未覆盖；临时文件已保留：%1").arg(tempPath),
                    true);
        return;
    }
    if (!QFile::rename(tempPath, finalPath))
    {
        failSession(state, QStringLiteral("成品改名失败，临时文件已保留：%1").arg(tempPath), true);
        return;
    }
    state->terminal.store(true, std::memory_order_release);
    RecordingEvent event;
    event.type = RecordingEventType::Finished;
    event.autoStopped = state->gifLimitReached;
    event.recordedMs = state->lastRecordedMs;
    event.bytes = fileSize(finalPath);
    event.path = finalPath;
    event.pixelSize = state->request.outputPixels;
    event.droppedFrames = state->droppedFrames.load(std::memory_order_relaxed);
    event.message = QStringLiteral("录制完成。");
    postEvent(state, std::move(event));
}

void finishWriter(const std::shared_ptr<RecordingState>& state)
{
    if (state->terminal.load(std::memory_order_acquire))
        return;
    state->acceptingSamples.store(false, std::memory_order_release);
    if (!state->writingStarted || !CMTIME_IS_VALID(state->lastVideoPts))
    {
        failSession(state, QStringLiteral("没有采集到任何画面，未生成文件。"));
        return;
    }
    drainMixedAudio(state, true);
    stopMicrophone(state);
    drainPendingAudioBeforeFinish(state);
    if (state->terminal.load(std::memory_order_acquire))
        return;
    [state->videoInput markAsFinished];
    if (state->audioInput)
        [state->audioInput markAsFinished];
    const auto keepAlive = state;
    [state->writer finishWritingWithCompletionHandler:^{
      const auto inner = keepAlive;
      dispatch_async(inner->workQueue, ^{
        @autoreleasepool
        {
            finalizeWriter(inner);
        }
      });
    }];
}

void beginStop(const std::shared_ptr<RecordingState>& state)
{
    if (state->terminal.load(std::memory_order_acquire))
        return;
    if (state->request.output == RecordingOutput::Gif)
    {
        const auto gifState = state;
        gifState->acceptingSamples.store(false, std::memory_order_release);
        if (gifState->frameTimer)
        {
            dispatch_source_cancel(gifState->frameTimer);
            gifState->frameTimer = nil;
        }
        [gifState->stream stopCaptureWithCompletionHandler:nil];
        gifState->latestGifFrame = {};
        const auto gif = gifState->gif;
        if (!gif)
        {
            failSession(gifState, QStringLiteral("GIF录制尚未开始。"));
            return;
        }
        dispatch_async(gif->queue, ^{
          QString error;
          const bool hasFrames = gif->encoder.frameCount() > 0;
          const bool ok = gif->encoder.finish(&error);
          dispatch_async(gifState->workQueue, ^{
            if (gifState->terminal.load(std::memory_order_acquire))
                return;
            if (!ok)
            {
                failSession(gifState, error, hasFrames);
                return;
            }
            finalizeWriter(gifState);
          });
        });
        return;
    }
    if (!state->stream || !state->writer)
    {
        failSession(state, QStringLiteral("录制尚未开始，已结束。"));
        return;
    }
    state->acceptingSamples.store(false, std::memory_order_release);
    // 先拒绝后续样本，再请求停止采集；写入器不依赖 stopCapture 的完成回调。
    [state->stream stopCaptureWithCompletionHandler:nil];
    finishWriter(state);
}

void configureAudio(SCStreamConfiguration* configuration)
{
    configuration.capturesAudio = YES;
    configuration.sampleRate = kAudioSampleRate;
    configuration.channelCount = kAudioChannels;
    configuration.excludesCurrentProcessAudio = YES;
}

SCContentFilter* makeContentFilter(SCShareableContent* content, const RecordingRequest& request,
                                   QString* error)
{
    if (request.targetKind == RecordingTargetKind::Window)
    {
        SCWindow* targetWindow = nil;
        for (SCWindow* window in content.windows)
            if (quint64(window.windowID) == request.windowId)
            {
                targetWindow = window;
                break;
            }
        if (!targetWindow)
        {
            *error = QStringLiteral("目标窗口已关闭，无法开始录制。");
            return nil;
        }
        return [[SCContentFilter alloc] initWithDesktopIndependentWindow:targetWindow];
    }
    SCDisplay* targetDisplay = nil;
    for (SCDisplay* display in content.displays)
        if (quint64(display.displayID) == quint64(request.display.id))
        {
            targetDisplay = display;
            break;
        }
    if (!targetDisplay)
    {
        *error = QStringLiteral("目标显示器已断开，无法开始录制。");
        return nil;
    }
    NSMutableArray<SCRunningApplication*>* excluded = [NSMutableArray array];
    for (SCRunningApplication* application in content.applications)
        if (application.processID == getpid())
            [excluded addObject:application];
    return [[SCContentFilter alloc] initWithDisplay:targetDisplay
                              excludingApplications:excluded
                                   exceptingWindows:@[]];
}

void configureStream(SCStreamConfiguration* configuration, const RecordingRequest& request)
{
    configuration.width = size_t(request.outputPixels.width());
    configuration.height = size_t(request.outputPixels.height());
    configuration.minimumFrameInterval = CMTimeMake(1, request.fps);
    configuration.pixelFormat = kCVPixelFormatType_32BGRA;
    configuration.colorSpaceName = kCGColorSpaceSRGB;
    configuration.showsCursor = request.showCursor;
    configuration.scalesToFit = YES;
    configuration.queueDepth = 5;
    if (request.targetKind != RecordingTargetKind::Window)
    {
        if (request.targetKind == RecordingTargetKind::Display)
        {
            configuration.sourceRect = CGRectMake(0, 0, request.display.logicalGeometry.width(),
                                                  request.display.logicalGeometry.height());
        }
        else
        {
            const double scale =
                request.display.devicePixelRatio > 0 ? request.display.devicePixelRatio : 1.0;
            const QRect& rectangle = request.sourcePixelRect;
            configuration.sourceRect =
                CGRectMake(rectangle.x() / scale, rectangle.y() / scale, rectangle.width() / scale,
                           rectangle.height() / scale);
        }
    }
    if (request.output == RecordingOutput::Gif)
        configuration.capturesAudio = NO;
    else if (request.systemAudio)
        configureAudio(configuration);
}

bool createWriter(const std::shared_ptr<RecordingState>& state, bool audioAvailable)
{
    const RecordingRequest& request = state->request;
    NSURL* url = [NSURL fileURLWithPath:request.tempPath.toNSString()];
    NSError* error = nil;
    AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                      fileType:AVFileTypeMPEG4
                                                         error:&error];
    if (!writer)
    {
        failSession(state,
                    QStringLiteral("无法创建视频写入器（错误 %1），请检查目标目录与磁盘空间。")
                        .arg(error ? error.code : -1));
        return false;
    }
    NSDictionary* videoSettings = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(request.outputPixels.width()),
        AVVideoHeightKey : @(request.outputPixels.height()),
        AVVideoCompressionPropertiesKey : @{
            AVVideoAverageBitRateKey :
                @(estimatedVideoBitrate(request.outputPixels, request.fps, request.quality)),
            AVVideoExpectedSourceFrameRateKey : @(request.fps),
            AVVideoMaxKeyFrameIntervalDurationKey : @(2),
            AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
        },
    };
    AVAssetWriterInput* videoInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                                    outputSettings:videoSettings];
    videoInput.expectsMediaDataInRealTime = YES;
    if (![writer canAddInput:videoInput])
    {
        failSession(state, QStringLiteral("视频编码参数不受支持，无法开始录制。"));
        return false;
    }
    [writer addInput:videoInput];
    AVAssetWriterInput* audioInput = nil;
    if (audioAvailable)
    {
        NSDictionary* audioSettings = @{
            AVFormatIDKey : @(kAudioFormatMPEG4AAC),
            AVSampleRateKey : @(kAudioSampleRate),
            AVNumberOfChannelsKey : @(kAudioChannels),
            AVEncoderBitRateKey : @(kAudioBitRate),
        };
        audioInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                                    outputSettings:audioSettings];
        audioInput.expectsMediaDataInRealTime = YES;
        if ([writer canAddInput:audioInput])
            [writer addInput:audioInput];
        else
        {
            audioInput = nil;
            emitWarning(state, QStringLiteral("音频编码参数不受支持，已降级为仅画面录制。"));
        }
    }
    state->writer = writer;
    state->videoInput = videoInput;
    state->audioInput = audioInput;
    return true;
}

void startCapture(const std::shared_ptr<RecordingState>& state, SCStream* stream);

bool retryWithoutSystemAudio(const std::shared_ptr<RecordingState>& state)
{
    if (!state->systemAvailable)
        return false;
    disableSystemAudio(state);
    [state->stream stopCaptureWithCompletionHandler:nil];
    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    configureStream(configuration, state->request);
    configuration.capturesAudio = NO;
    SCStream* replacement = [[SCStream alloc] initWithFilter:state->filter
                                               configuration:configuration
                                                    delegate:nil];
    NSError* error = nil;
    if (![replacement addStreamOutput:state->screenOutput
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:state->workQueue
                                error:&error])
        return false;
    state->audioOutput = nil;
    startCapture(state, replacement);
    return true;
}

void startCapture(const std::shared_ptr<RecordingState>& state, SCStream* stream)
{
    state->stream = stream;
    state->acceptingSamples.store(true, std::memory_order_release);
    const auto keepAlive = state;
    [stream startCaptureWithCompletionHandler:^(NSError* error) {
      const auto inner = keepAlive;
      dispatch_async(inner->workQueue, ^{
        @autoreleasepool
        {
            if (inner->terminal.load(std::memory_order_acquire) ||
                inner->stopRequested.load(std::memory_order_acquire) ||
                inner->discardRequested.load(std::memory_order_acquire))
                return;
            if (error)
            {
                if (retryWithoutSystemAudio(inner))
                    return;
                failSession(inner,
                            QStringLiteral("屏幕采集启动失败（错误 %1），请检查屏幕录制权限。")
                                .arg(error.code));
                return;
            }
            RecordingEvent event;
            event.type = RecordingEventType::Started;
            event.pixelSize = inner->request.outputPixels;
            event.message = QStringLiteral("录制已开始。");
            postEvent(inner, std::move(event));
        }
      });
    }];
}

void configureAndStart(const std::shared_ptr<RecordingState>& state, SCShareableContent* content,
                       NSError* contentError)
{
    if (state->terminal.load(std::memory_order_acquire) ||
        state->stopRequested.load(std::memory_order_acquire) ||
        state->discardRequested.load(std::memory_order_acquire))
        return;
    if (contentError || !content)
    {
        failSession(state,
                    QStringLiteral("无法读取屏幕采集信息（错误 %1），请检查权限与显示器连接。")
                        .arg(contentError ? contentError.code : -1));
        return;
    }
    QString filterError;
    SCContentFilter* filter = makeContentFilter(content, state->request, &filterError);
    if (!filter)
    {
        failSession(state, filterError);
        return;
    }
    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    configureStream(configuration, state->request);
    SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:configuration
                                               delegate:nil];
    WaibuSnapRecordingStreamOutput* screenOutput = [[WaibuSnapRecordingStreamOutput alloc] init];
    const auto screenState = state;
    screenOutput.handler = ^(CMSampleBufferRef sampleBuffer) {
      handleScreenSample(screenState, sampleBuffer);
    };
    NSError* addError = nil;
    if (![stream addStreamOutput:screenOutput
                            type:SCStreamOutputTypeScreen
              sampleHandlerQueue:state->workQueue
                           error:&addError])
    {
        failSession(state, QStringLiteral("无法添加画面输出（错误 %1），录制未开始。")
                               .arg(addError ? addError.code : -1));
        return;
    }
    state->screenOutput = screenOutput;
    state->systemAvailable = state->request.systemAudio;
    if (state->systemAvailable)
    {
        WaibuSnapRecordingStreamOutput* audioOutput = [[WaibuSnapRecordingStreamOutput alloc] init];
        const auto audioState = state;
        audioOutput.handler = ^(CMSampleBufferRef sampleBuffer) {
          handleAudioSample(audioState, sampleBuffer);
        };
        NSError* audioError = nil;
        if (![stream addStreamOutput:audioOutput
                                type:SCStreamOutputTypeAudio
                  sampleHandlerQueue:state->workQueue
                               error:&audioError])
        {
            disableSystemAudio(state);
            // SCStream 复制配置；重建为纯视频，避免失败的系统音频配置阻塞画面启动。
            configuration.capturesAudio = NO;
            stream = [[SCStream alloc] initWithFilter:filter
                                        configuration:configuration
                                             delegate:nil];
            if (![stream addStreamOutput:screenOutput
                                    type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:state->workQueue
                                   error:&addError])
            {
                failSession(state, QStringLiteral("无法添加画面输出，录制未开始。"));
                return;
            }
        }
        else
        {
            state->audioOutput = audioOutput;
        }
    }
    if (state->request.output == RecordingOutput::Gif)
    {
        const auto gifState = state;
        gifState->filter = filter;
        gifState->gif = std::make_shared<GifPipeline>();
        const auto gif = gifState->gif;
        dispatch_async(gif->queue, ^{
          QString error;
          const bool ok =
              gif->encoder.start(gifState->request.tempPath, gifState->request.outputPixels,
                                 gifState->request.fps, &error);
          dispatch_async(gifState->workQueue, ^{
            if (sessionEnding(gifState))
                return;
            if (!ok)
            {
                failSession(gifState, error);
                return;
            }
            startCapture(gifState, stream);
          });
        });
        return;
    }
    const bool microphoneAvailable = prepareMicrophone(state);
    state->mixer.setSources(state->systemAvailable, microphoneAvailable);
    state->mixer.setGains(float(state->request.systemGain), float(state->request.microphoneGain));
    state->filter = filter;
    if (!createWriter(state, state->systemAvailable || microphoneAvailable))
        return;
    if (!state->audioInput)
        stopMicrophone(state);
    startCapture(state, stream);
}

void beginSetup(const std::shared_ptr<RecordingState>& state)
{
    const auto keepAlive = state;
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:NO
                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                   const auto inner = keepAlive;
                                   dispatch_async(inner->workQueue, ^{
                                     @autoreleasepool
                                     {
                                         configureAndStart(inner, content, error);
                                     }
                                   });
                                 }];
}

class MacRecordingSession final : public RecordingSession
{
  public:
    explicit MacRecordingSession(std::function<bool()> encoderAvailable)
        : encoderAvailable_(std::move(encoderAvailable))
    {
    }
    ~MacRecordingSession() override
    {
        const auto state = std::move(state_);
        if (!state)
            return;
        state->destroyed.store(true, std::memory_order_release);
        state->acceptingSamples.store(false, std::memory_order_release);
        state->terminal.store(true, std::memory_order_release);
        if (!state->workQueue)
            return;
        dispatch_async(state->workQueue, ^{
          @autoreleasepool
          {
              if (state->writer && state->writer.status == AVAssetWriterStatusWriting)
                  [state->writer cancelWriting];
              releaseCaptureObjects(state);
              QFile::remove(state->request.tempPath);
          }
        });
    }

    bool start(const RecordingRequest& request, EventCallback callback, QString* error) override
    {
        const auto setError = [error](const QString& message)
        {
            if (error)
                *error = message;
        };
        if (state_)
        {
            setError(QStringLiteral("录制会话已经启动过，请创建新的会话。"));
            return false;
        }
        const QString validation = validateRecordingRequest(request);
        if (!validation.isEmpty())
        {
            setError(validation);
            return false;
        }
        // 在创建写入器、查询采集内容与申请权限之前拒绝无编码器环境。
        const bool encoderAvailable =
            request.output != RecordingOutput::Video || encoderAvailable_();
        if (encoderAvailable && !hasScreenRecordingPermission())
        {
            const QString explanation = permissionExplanation();
            setError(explanation);
            // start 返回 false 时调用方可能立刻释放会话，权限拒绝同步交付才能保证可见。
            if (callback)
            {
                RecordingEvent event;
                event.type = RecordingEventType::PermissionDenied;
                event.message = explanation;
                callback(event);
            }
            return false;
        }
        auto state = std::make_shared<RecordingState>();
        state->request = request;
        state->callback = std::move(callback);
        state->workQueue = dispatch_queue_create(kRecordingQueueName, DISPATCH_QUEUE_SERIAL);
        if (!state->workQueue)
        {
            setError(QStringLiteral("无法创建录制工作队列，请重试。"));
            return false;
        }
        state_ = state;
        dispatch_async(state->workQueue, ^{
          @autoreleasepool
          {
              if (!encoderAvailable)
              {
                  failSession(state, QStringLiteral("当前环境不支持 H.264 编码，无法开始录制"));
                  return;
              }
              beginSetup(state);
          }
        });
        return true;
    }

    void pause() override
    {
        const auto state = state_;
        if (!state || sessionEnding(state))
            return;
        const qint64 commandNs = monotonicNs();
        dispatch_async(state->workQueue, ^{
          if (sessionEnding(state) || state->paused.exchange(true, std::memory_order_acq_rel))
              return;
          state->pauseStartNs.store(commandNs, std::memory_order_release);
          if (state->request.output == RecordingOutput::Gif)
              state->latestGifFrame = {};
          resetAudioForPause(state);
        });
    }

    void resume() override
    {
        const auto state = state_;
        if (!state || sessionEnding(state))
            return;
        const qint64 commandNs = monotonicNs();
        const CMTime resumePts = CMClockGetTime(CMClockGetHostTimeClock());
        dispatch_async(state->workQueue, ^{
          if (sessionEnding(state) || !state->paused.load(std::memory_order_acquire))
              return;
          if (state->request.output == RecordingOutput::Gif)
              state->latestGifFrame = {};
          resetAudioForPause(state);
          state->minimumSamplePts = resumePts;
          const qint64 start = state->pauseStartNs.exchange(0, std::memory_order_acq_rel);
          if (start > 0 && commandNs > start)
              state->pausedTotalNs.fetch_add(commandNs - start, std::memory_order_acq_rel);
          state->paused.store(false, std::memory_order_release);
        });
    }

    void stop() override
    {
        const auto state = state_;
        if (!state || state->terminal.load(std::memory_order_acquire))
            return;
        if (state->stopRequested.exchange(true, std::memory_order_acq_rel))
            return;
        state->acceptingSamples.store(false, std::memory_order_release);
        dispatch_async(state->workQueue, ^{
          @autoreleasepool
          {
              beginStop(state);
          }
        });
    }

    void discard() override
    {
        const auto state = state_;
        if (!state || state->terminal.load(std::memory_order_acquire))
            return;
        state->discardRequested.store(true, std::memory_order_release);
        state->acceptingSamples.store(false, std::memory_order_release);
        dispatch_async(state->workQueue, ^{
          @autoreleasepool
          {
              discardSession(state);
          }
        });
    }

  private:
    std::function<bool()> encoderAvailable_;
    std::shared_ptr<RecordingState> state_;
};

QImage imageFromCGImage(CGImageRef image)
{
    if (!image)
        return {};
    const int width = int(CGImageGetWidth(image));
    const int height = int(CGImageGetHeight(image));
    if (width <= 0 || height <= 0)
        return {};
    QImage pixels(width, height, QImage::Format_ARGB32_Premultiplied);
    if (pixels.isNull())
        return {};
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(pixels.bits(), width, height, 8, size_t(pixels.bytesPerLine()), space,
                              kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    if (!context)
        return {};
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    pixels.setColorSpace(QColorSpace::SRgb);
    return pixels;
}

NSArray<AVAssetTrack*>* loadVideoTracks(AVAsset* asset)
{
    struct TrackLoadResult
    {
        NSArray<AVAssetTrack*>* __strong tracks = nil;
    };
    const auto result = std::make_shared<TrackLoadResult>();
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack*>* loaded, NSError* error) {
                   (void)error;
                   result->tracks = loaded;
                   dispatch_semaphore_signal(semaphore);
                 }];
    if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0)
        return nil;
    return result->tracks;
}

bool isGifPath(const QString& path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
}

class MacRecordingMediaProbe final : public RecordingMediaProbe
{
  public:
    RecordingMediaInfo probe(const QString& path) const override
    {
        RecordingMediaInfo info;
        const QFileInfo file(path);
        if (!file.exists() || !file.isFile())
        {
            info.error = QStringLiteral("文件不存在或不是普通文件。");
            return info;
        }
        if (isGifPath(path))
            return probeRecordingGif(path);
        @autoreleasepool
        {
            AVURLAsset* asset =
                [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path.toNSString()]
                                    options:@{
                                        AVURLAssetPreferPreciseDurationAndTimingKey : @YES
                                    }];
            NSArray<AVAssetTrack*>* tracks = loadVideoTracks(asset);
            if (!tracks || tracks.count == 0)
            {
                info.error = QStringLiteral("无法读取媒体信息：不是可读取的视频文件。");
                return info;
            }
            const CMTime duration = asset.duration;
            if (!CMTIME_IS_NUMERIC(duration) || CMTimeCompare(duration, kCMTimeZero) < 0)
            {
                info.error = QStringLiteral("无法读取视频时长。");
                return info;
            }
            AVAssetTrack* track = tracks.firstObject;
            const CGSize natural = track.naturalSize;
            const CGAffineTransform transform = track.preferredTransform;
            const CGRect displayRect = CGRectApplyAffineTransform(
                CGRectMake(0, 0, natural.width, natural.height), transform);
            const QSize pixels(qRound(std::fabs(displayRect.size.width)),
                               qRound(std::fabs(displayRect.size.height)));
            if (pixels.width() <= 0 || pixels.height() <= 0)
            {
                info.error = QStringLiteral("无法读取视频分辨率。");
                return info;
            }
            info.valid = true;
            info.durationMs = qRound64(CMTimeGetSeconds(duration) * 1000.0);
            info.pixelSize = pixels;
        }
        return info;
    }

    QImage thumbnail(const QString& path, QSize bounds) const override
    {
        const QFileInfo file(path);
        if (isGifPath(path))
            return recordingGifThumbnail(path, bounds);
        if (!file.exists() || !file.isFile())
            return {};
        @autoreleasepool
        {
            AVURLAsset* asset =
                [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path.toNSString()]
                                    options:@{
                                        AVURLAssetPreferPreciseDurationAndTimingKey : @YES
                                    }];
            AVAssetImageGenerator* generator = [[AVAssetImageGenerator alloc] initWithAsset:asset];
            generator.appliesPreferredTrackTransform = YES;
            if (bounds.width() > 0 && bounds.height() > 0)
                generator.maximumSize = CGSizeMake(bounds.width(), bounds.height());
            const auto output = std::make_shared<QImage>();
            dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
            const auto keepOutput = output;
            [generator generateCGImageAsynchronouslyForTime:kCMTimeZero
                                          completionHandler:^(CGImageRef image, CMTime actualTime,
                                                              NSError* error) {
                                            (void)actualTime;
                                            (void)error;
                                            @autoreleasepool
                                            {
                                                if (image)
                                                    *keepOutput = imageFromCGImage(image);
                                                dispatch_semaphore_signal(semaphore);
                                            }
                                          }];
            if (dispatch_semaphore_wait(semaphore,
                                        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0)
                return {};
            return *output;
        }
    }
};

} // namespace

std::unique_ptr<RecordingSession> createRecordingSession()
{
    return createMacRecordingSession(macH264EncoderAvailable);
}

std::unique_ptr<RecordingSession>
createMacRecordingSession(std::function<bool()> h264EncoderAvailable)
{
    return std::make_unique<MacRecordingSession>(std::move(h264EncoderAvailable));
}

bool macH264EncoderAvailable()
{
    static const bool available = []
    {
        @autoreleasepool
        {
            CFArrayRef list = nullptr;
            const OSStatus status = VTCopyVideoEncoderList(nullptr, &list);
            NSArray* encoders = CFBridgingRelease(list);
            bool found = false;
            if (status == noErr)
                for (NSDictionary* encoder in encoders)
                {
                    NSNumber* codec = encoder[(__bridge NSString*)kVTVideoEncoderList_CodecType];
                    if (codec.unsignedIntValue == kCMVideoCodecType_H264)
                    {
                        found = true;
                        break;
                    }
                }
            qInfo() << "H.264 编码能力查询：状态" << status << "，可用" << found;
            return found;
        }
    }();
    return available;
}

bool recordingCaptureSupported() { return true; }

bool hasScreenRecordingPermission() { return CGPreflightScreenCaptureAccess(); }

bool requestScreenRecordingPermission() { return CGRequestScreenCaptureAccess(); }

MicrophonePermission microphonePermission()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
    {
    case AVAuthorizationStatusAuthorized:
        return MicrophonePermission::Authorized;
    case AVAuthorizationStatusNotDetermined:
        return MicrophonePermission::NotDetermined;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
    default:
        return MicrophonePermission::Denied;
    }
}

void requestMicrophonePermission(std::function<void(bool)> callback)
{
    if (!QCoreApplication::instance())
        return;
    // 所有完成路径都排队到 GUI；done 保证系统回调与超时竞争时只交付一次。
    struct PermissionRequest
    {
        std::atomic<bool> done{false};
        std::function<void(bool)> callback;
    };
    const auto pending = std::make_shared<PermissionRequest>();
    pending->callback = std::move(callback);
    const auto deliver = [pending](bool authorized)
    {
        if (!QCoreApplication::instance())
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [pending, authorized]
            {
                if (!pending->done.exchange(true, std::memory_order_acq_rel) && pending->callback)
                {
                    auto callback = std::move(pending->callback);
                    callback(authorized);
                }
            },
            Qt::QueuedConnection);
    };
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [pending, deliver]
        {
            const auto permission = microphonePermission();
            if (permission != MicrophonePermission::NotDetermined)
            {
                deliver(permission == MicrophonePermission::Authorized);
                return;
            }
            QTimer::singleShot(30000, QCoreApplication::instance(), [deliver] { deliver(false); });
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                                       deliver(granted);
                                     }];
        },
        Qt::QueuedConnection);
}

QString microphonePermissionExplanation()
{
    return QStringLiteral(
        "麦克风权限仅用于您开启的麦克风录制；请在系统设置 → 隐私与安全性 → "
        "麦克风中允许 WaibuSnap，然后重新开始录制。声音只写入本机文件，不会上传。");
}

QString defaultMicrophoneDeviceId()
{
    @autoreleasepool
    {
        AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
        return device ? QString::fromNSString(device.uniqueID) : QString();
    }
}

QVector<MicrophoneDevice> microphoneDevices()
{
    @autoreleasepool
    {
        QVector<MicrophoneDevice> result;
        const QString defaultId = defaultMicrophoneDeviceId();
        for (AVCaptureDevice* device in audioDiscovery().devices)
        {
            MicrophoneDevice item;
            item.id = QString::fromNSString(device.uniqueID);
            item.name = QString::fromNSString(device.localizedName);
            item.isDefault = item.id == defaultId;
            if (!item.id.isEmpty() && std::none_of(result.cbegin(), result.cend(),
                                                   [&item](const MicrophoneDevice& existing)
                                                   { return existing.id == item.id; }))
                result.push_back(item);
        }
        std::stable_sort(result.begin(), result.end(),
                         [](const MicrophoneDevice& left, const MicrophoneDevice& right)
                         {
                             const int order =
                                 QString::compare(left.name, right.name, Qt::CaseSensitive);
                             return order == 0 ? left.id < right.id : order < 0;
                         });
        return result;
    }
}

std::unique_ptr<RecordingMediaProbe> createRecordingMediaProbe()
{
    return std::make_unique<MacRecordingMediaProbe>();
}

} // namespace waibusnap

namespace waibusnap
{
namespace
{
std::optional<RecordingWindowInfo> readRecordingWindow(NSDictionary* dictionary, bool selectable)
{
    const auto id = [dictionary[(__bridge NSString*)kCGWindowNumber] unsignedLongLongValue];
    CGRect frame = CGRectZero;
    if (!id ||
        !CGRectMakeWithDictionaryRepresentation(
            (__bridge CFDictionaryRef)dictionary[(__bridge NSString*)kCGWindowBounds], &frame))
        return std::nullopt;
    const auto pid = [dictionary[(__bridge NSString*)kCGWindowOwnerPID] intValue];
    const auto layer = [dictionary[(__bridge NSString*)kCGWindowLayer] intValue];
    const auto alpha = [dictionary[(__bridge NSString*)kCGWindowAlpha] doubleValue];
    NSString* app = dictionary[(__bridge NSString*)kCGWindowOwnerName];
    if (selectable && (pid == getpid() || layer != 0 || alpha <= 0 || frame.size.width < 32 ||
                       frame.size.height < 32 || [app isEqualToString:@"Dock"] ||
                       [app isEqualToString:@"Window Server"]))
        return std::nullopt;
    NSString* title = dictionary[(__bridge NSString*)kCGWindowName];
    QString label = app.length ? QString::fromNSString(app) : QStringLiteral("应用窗口");
    if (title.length)
        label += QStringLiteral(" — ") + QString::fromNSString(title);
    // 标题只用于选择器，不写日志；限制标签内存与展示长度。
    label = label.left(160);
    return RecordingWindowInfo{
        id,
        QRect(qRound(frame.origin.x), qRound(frame.origin.y), qRound(frame.size.width),
              qRound(frame.size.height)),
        label, [dictionary[(__bridge NSString*)kCGWindowIsOnscreen] boolValue] != NO};
}
class MacRecordingEnvironmentObserver final : public RecordingEnvironmentObserver
{
  public:
    ~MacRecordingEnvironmentObserver() override { stop(); }
    void start(std::function<void()> suspend) override
    {
        stop();
        context_ = std::make_unique<QObject>();
        const QPointer<QObject> context(context_.get());
        NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
        for (NSNotificationName name in @[
                 NSWorkspaceWillSleepNotification, NSWorkspaceScreensDidSleepNotification,
                 NSWorkspaceSessionDidResignActiveNotification
             ])
        {
            id observer = [center
                addObserverForName:name
                            object:nil
                             queue:NSOperationQueue.mainQueue
                        usingBlock:^(NSNotification*) {
                          if (context)
                              QMetaObject::invokeMethod(context, suspend, Qt::QueuedConnection);
                        }];
            [workspaceObservers_ addObject:observer];
        }
        id lockObserver = [NSDistributedNotificationCenter.defaultCenter
            addObserverForName:@"com.apple.screenIsLocked"
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                      if (context)
                          QMetaObject::invokeMethod(context, suspend, Qt::QueuedConnection);
                    }];
        lockObserver_ = lockObserver;
    }
    void stop() override
    {
        context_.reset();
        for (id observer in workspaceObservers_)
            [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer];
        [workspaceObservers_ removeAllObjects];
        if (lockObserver_)
            [NSDistributedNotificationCenter.defaultCenter removeObserver:lockObserver_];
        lockObserver_ = nil;
    }

  private:
    std::unique_ptr<QObject> context_;
    NSMutableArray* workspaceObservers_ = [NSMutableArray array];
    id lockObserver_ = nil;
};
}
QVector<RecordingWindowInfo> recordingWindows()
{
    QVector<RecordingWindowInfo> result;
    NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    for (NSDictionary* dictionary in windows)
        if (const auto window = readRecordingWindow(dictionary, true); window && window->onScreen)
            result.append(*window);
    return result;
}
std::optional<RecordingWindowInfo> recordingWindowInfo(quint64 id)
{
    if (!id || id > std::numeric_limits<CGWindowID>::max())
        return std::nullopt;
    NSArray* windows = CFBridgingRelease(
        CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, CGWindowID(id)));
    for (NSDictionary* dictionary in windows)
        if (const auto window = readRecordingWindow(dictionary, false); window && window->id == id)
            return window;
    return std::nullopt;
}
std::unique_ptr<RecordingEnvironmentObserver> createRecordingEnvironmentObserver()
{
    return std::make_unique<MacRecordingEnvironmentObserver>();
}
}
