#include "interfaces/recording.h"
#include "session/monotonic_clock.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <dispatch/dispatch.h>
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

struct RecordingState
{
    RecordingRequest request;
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

void releaseCaptureObjects(const std::shared_ptr<RecordingState>& state)
{
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
    CMTime duration = CMSampleBufferGetDuration(sampleBuffer);
    if (!CMTIME_IS_NUMERIC(duration) || CMTimeCompare(duration, kCMTimeZero) <= 0)
        duration = CMTimeMake(CMSampleBufferGetNumSamples(sampleBuffer), kAudioSampleRate);
    CMSampleTimingInfo timing = {duration, adjusted, kCMTimeInvalid};
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

void handleScreenSample(const std::shared_ptr<RecordingState>& state,
                        CMSampleBufferRef sampleBuffer)
{
    if (!acceptingSamplesAfter(state))
        return;
    if (!CMSampleBufferGetImageBuffer(sampleBuffer))
        return;
    if (!appendVideoBuffer(state, sampleBuffer))
        return;
    drainPendingAudio(state);
}

void handleAudioSample(const std::shared_ptr<RecordingState>& state, CMSampleBufferRef sampleBuffer)
{
    if (!acceptingSamplesAfter(state))
        return;
    appendAudioBuffer(state, sampleBuffer);
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
    if (status != AVAssetWriterStatusCompleted)
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
    if (request.systemAudio)
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
    if (audioAvailable && request.systemAudio)
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
            emitWarning(state, QStringLiteral("系统声音编码参数不受支持，已降级为仅画面录制。"));
        }
    }
    state->writer = writer;
    state->videoInput = videoInput;
    state->audioInput = audioInput;
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
    bool audioAvailable = state->request.systemAudio;
    if (audioAvailable)
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
            audioAvailable = false;
            emitWarning(state, QStringLiteral("系统声音初始化失败（错误 %1），已降级为仅画面录制。")
                                   .arg(audioError ? audioError.code : -1));
        }
        else
        {
            state->audioOutput = audioOutput;
        }
    }
    if (!createWriter(state, audioAvailable))
        return;
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

QString validateTarget(const RecordingRequest& request)
{
    if (request.targetKind == RecordingTargetKind::Window)
    {
        if (request.windowId == 0)
            return QStringLiteral("窗口录制缺少有效的窗口 ID。");
        return {};
    }
    if (request.display.id == 0)
        return QStringLiteral("录制目标显示器无效，请重新选择。");
    const double scale =
        request.display.devicePixelRatio > 0 ? request.display.devicePixelRatio : 1.0;
    const QSize displayPixels(qRound(request.display.logicalGeometry.width() * scale),
                              qRound(request.display.logicalGeometry.height() * scale));
    if (request.targetKind == RecordingTargetKind::Display)
        return {};
    const QRect& rectangle = request.sourcePixelRect;
    if (rectangle.width() <= 0 || rectangle.height() <= 0)
        return QStringLiteral("区域录制缺少有效的选区尺寸。");
    if (!QRect(QPoint(0, 0), displayPixels).contains(rectangle))
        return QStringLiteral("录制区域超出目标显示器范围，请重新框选。");
    return {};
}

QString validateRequest(const RecordingRequest& request)
{
    if (request.output != RecordingOutput::Video)
        return QStringLiteral("GIF 录制将在 R5 接入；当前仅支持 MP4 视频。");
    if (!request.microphoneDeviceId.isEmpty())
        return QStringLiteral("麦克风将在 R3 接入；当前请关闭麦克风后重试。");
    if (request.outputPixels.width() <= 0 || request.outputPixels.height() <= 0)
        return QStringLiteral("输出尺寸必须为正数。");
    if (request.outputPixels.width() % 2 != 0 || request.outputPixels.height() % 2 != 0)
        return QStringLiteral("视频输出宽高必须为偶数。");
    if (request.fps != 30 && request.fps != 60)
        return QStringLiteral("视频帧率只支持 30 或 60。");
    if (request.maxDurationMs < 0)
        return QStringLiteral("最长录制时长不能为负数。");
    if (request.tempPath.isEmpty() || request.finalPath.isEmpty())
        return QStringLiteral("录制临时路径与成品路径不能为空。");
    if (!QDir::isAbsolutePath(request.tempPath) || !QDir::isAbsolutePath(request.finalPath))
        return QStringLiteral("录制路径必须是绝对路径。");
    if (QDir::cleanPath(request.tempPath) == QDir::cleanPath(request.finalPath))
        return QStringLiteral("录制临时路径不能与成品路径相同。");
    if (!request.tempPath.endsWith(QStringLiteral(".part"), Qt::CaseInsensitive))
        return QStringLiteral("录制临时文件必须以 .part 结尾。");
    if (QFileInfo::exists(request.tempPath))
        return QStringLiteral("录制临时文件已存在，请重新选择路径。");
    if (QFileInfo::exists(request.finalPath))
        return QStringLiteral("成品路径已存在，请重新选择文件名。");
    const QFileInfo temporary(request.tempPath);
    const QFileInfo finalInfo(request.finalPath);
    if (!temporary.absoluteDir().exists() || !finalInfo.absoluteDir().exists())
        return QStringLiteral("录制目录不存在，请先在设置中重新选择目录。");
    if (!QFileInfo(temporary.absolutePath()).isWritable() ||
        !QFileInfo(finalInfo.absolutePath()).isWritable())
        return QStringLiteral("录制目录不可写，请检查目录权限。");
    return validateTarget(request);
}

class MacRecordingSession final : public RecordingSession
{
  public:
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
        const QString validation = validateRequest(request);
        if (!validation.isEmpty())
        {
            setError(validation);
            return false;
        }
        if (!hasScreenRecordingPermission())
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
              beginSetup(state);
          }
        });
        return true;
    }

    void pause() override
    {
        const auto state = state_;
        if (!state || state->terminal.load(std::memory_order_acquire) ||
            state->stopRequested.load(std::memory_order_acquire) ||
            state->discardRequested.load(std::memory_order_acquire))
            return;
        if (!state->paused.exchange(true, std::memory_order_acq_rel))
            state->pauseStartNs.store(monotonicNs(), std::memory_order_release);
    }

    void resume() override
    {
        const auto state = state_;
        if (!state || state->terminal.load(std::memory_order_acquire) ||
            state->stopRequested.load(std::memory_order_acquire) ||
            state->discardRequested.load(std::memory_order_acquire))
            return;
        if (state->paused.exchange(false, std::memory_order_acq_rel))
        {
            const qint64 start = state->pauseStartNs.exchange(0, std::memory_order_acq_rel);
            const qint64 now = monotonicNs();
            if (start > 0 && now > start)
                state->pausedTotalNs.fetch_add(now - start, std::memory_order_acq_rel);
        }
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
        {
            info.error = QStringLiteral("GIF 媒体探测在 R6 接入。");
            return info;
        }
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
        if (!file.exists() || !file.isFile() || isGifPath(path))
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
    return std::make_unique<MacRecordingSession>();
}

bool recordingCaptureSupported() { return true; }

bool hasScreenRecordingPermission() { return CGPreflightScreenCaptureAccess(); }

bool requestScreenRecordingPermission() { return CGRequestScreenCaptureAccess(); }

std::unique_ptr<RecordingMediaProbe> createRecordingMediaProbe()
{
    return std::make_unique<MacRecordingMediaProbe>();
}

} // namespace waibusnap
