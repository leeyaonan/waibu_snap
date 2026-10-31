#include "interfaces/recording.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextStream>
#include <unistd.h>

namespace
{
constexpr int kWidth = 320, kHeight = 240, kFrames = 30, kFps = 30;

void diagnose(const char* stage, AVAssetWriter* writer, AVAssetWriterInput* input,
              NSError* error = nil)
{
    QTextStream out(stderr);
    out << "阶段=" << stage << " writer=" << bool(writer)
        << " status=" << (writer ? int(writer.status) : -1)
        << " error=" << QString::fromNSString((writer.error ? writer.error : error).description)
        << " input=" << bool(input) << " ready=" << bool(input.isReadyForMoreMediaData) << '\n';
    out.flush();
}

bool writeTestVideo(const QString& path)
{
    @autoreleasepool
    {
        AVAssetWriter* writer = nil;
        AVAssetWriterInput* input = nil;
        const auto fail = [&](const char* stage, NSError* error = nil)
        {
            diagnose(stage, writer, input, error);
            if (writer.status == AVAssetWriterStatusWriting)
                [writer cancelWriting];
            return false;
        };
        @try
        {
            NSError* error = nil;
            diagnose("创建 AVAssetWriter 前", writer, input);
            writer = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:path.toNSString()]
                                               fileType:AVFileTypeMPEG4
                                                  error:&error];
            if (!writer)
                return fail("创建 AVAssetWriter 失败", error);
            NSDictionary* settings = @{
                AVVideoCodecKey : AVVideoCodecTypeH264,
                AVVideoWidthKey : @(kWidth),
                AVVideoHeightKey : @(kHeight),
            };
            input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                   outputSettings:settings];
            if (!input)
                return fail("创建 AVAssetWriterInput 失败");
            input.expectsMediaDataInRealTime = NO;
            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey : @(kWidth),
                (id)kCVPixelBufferHeightKey : @(kHeight),
            };
            AVAssetWriterInputPixelBufferAdaptor* adaptor =
                [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:input
                                                           sourcePixelBufferAttributes:attributes];
            if (!adaptor)
                return fail("创建 adaptor 失败");
            if (![writer canAddInput:input])
                return fail("canAddInput 失败");
            [writer addInput:input];
            diagnose("startWriting 前", writer, input);
            if (writer.status != AVAssetWriterStatusUnknown || ![writer startWriting])
                return fail("startWriting 失败");
            [writer startSessionAtSourceTime:kCMTimeZero];
            CVPixelBufferPoolRef pool = adaptor.pixelBufferPool;
            if (!pool)
                return fail("pixelBufferPool 为空");
            // getter 不转移所有权；跨异步编码保留池，不能依赖其内部替换时机。
            CVPixelBufferPoolRetain(pool);
            const auto releasePool = qScopeGuard([pool] { CVPixelBufferPoolRelease(pool); });
            for (int frame = 0; frame < kFrames; ++frame)
            {
                for (int attempt = 0; attempt < 5000 && !input.isReadyForMoreMediaData &&
                                      writer.status == AVAssetWriterStatusWriting;
                     ++attempt)
                    ::usleep(1000);
                if (!input.isReadyForMoreMediaData || writer.status != AVAssetWriterStatusWriting)
                    return fail("等待 input 就绪失败");
                CVPixelBufferRef buffer = nullptr;
                if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) !=
                        kCVReturnSuccess ||
                    !buffer)
                    return fail("创建 pixelBuffer 失败");
                const CVReturn locked = CVPixelBufferLockBaseAddress(buffer, 0);
                auto* pixels = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
                const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
                if (locked != kCVReturnSuccess || !pixels || stride < size_t(kWidth * 4) ||
                    CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
                    CVPixelBufferIsPlanar(buffer) || CVPixelBufferGetWidth(buffer) != kWidth ||
                    CVPixelBufferGetHeight(buffer) != kHeight)
                {
                    QTextStream(stderr)
                        << "像素缓冲诊断：frame=" << frame << " lock=" << locked
                        << " base=" << bool(pixels) << " stride=" << stride
                        << " width=" << CVPixelBufferGetWidth(buffer)
                        << " height=" << CVPixelBufferGetHeight(buffer)
                        << " format=" << CVPixelBufferGetPixelFormatType(buffer) << '\n';
                    if (locked == kCVReturnSuccess)
                        CVPixelBufferUnlockBaseAddress(buffer, 0);
                    CVPixelBufferRelease(buffer);
                    return fail("锁定 pixelBuffer 失败");
                }
                for (int y = 0; y < kHeight; ++y)
                    for (int x = 0; x < kWidth; ++x)
                    {
                        uint8_t* pixel = pixels + y * stride + x * 4;
                        pixel[0] = uint8_t(frame * 7 + x % 256);
                        pixel[1] = uint8_t(y % 256);
                        pixel[2] = 180;
                        pixel[3] = 255;
                    }
                CVPixelBufferUnlockBaseAddress(buffer, 0);
                diagnose("appendPixelBuffer 前", writer, input);
                const BOOL appended = [adaptor appendPixelBuffer:buffer
                                            withPresentationTime:CMTimeMake(frame, kFps)];
                CVPixelBufferRelease(buffer);
                if (!appended)
                    return fail("appendPixelBuffer 失败");
            }
            [input markAsFinished];
            dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
            if (!semaphore)
                return fail("创建完成信号量失败");
            diagnose("finishWriting 前", writer, input);
            [writer finishWritingWithCompletionHandler:^{
              dispatch_semaphore_signal(semaphore);
            }];
            if (dispatch_semaphore_wait(semaphore,
                                        dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0)
                return fail("finishWriting 超时");
            if (writer.status != AVAssetWriterStatusCompleted)
                return fail("finishWriting 失败");
            diagnose("完成", writer, input);
            return true;
        }
        @catch (NSException* exception)
        {
            QTextStream(stderr) << "原生异常=" << QString::fromNSString(exception.description)
                                << '\n';
            return fail("捕获 Objective-C 异常");
        }
    }
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 3 && arguments[1] == QStringLiteral("--worker"))
        return writeTestVideo(arguments[2]) ? 0 : 1;
    if (arguments.size() != 2 || QFileInfo::exists(arguments[1]))
    {
        QTextStream(stderr)
            << "用法：make_recording_fixture <尚不存在的输出 MP4 路径>；不覆盖已有文件。\n";
        return 1;
    }
    const QString destination = QFileInfo(arguments[1]).absoluteFilePath();
    QTemporaryDir directory(
        QDir(QFileInfo(destination).absolutePath()).filePath(QStringLiteral(".fixture-XXXXXX")));
    if (!directory.isValid())
    {
        QTextStream(stderr) << "阶段=准备临时目录失败\n";
        return 1;
    }
    const QString temporary = directory.filePath(QStringLiteral("probe-video.mp4"));
    // nil / 状态检查不能拦截驱动 SIGSEGV：子进程崩溃时由父进程正常报告非零退出。
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(app.applicationFilePath(), {QStringLiteral("--worker"), temporary});
    if (!child.waitForStarted() || !child.waitForFinished(30000))
    {
        child.kill();
        child.waitForFinished(5000);
        QTextStream(stderr) << "阶段=生成子进程启动失败或超时：" << child.errorString() << '\n';
        return 1;
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0)
    {
        QTextStream(stderr) << "阶段=生成子进程失败，退出码=" << child.exitCode()
                            << "，异常终止=" << (child.exitStatus() == QProcess::CrashExit) << '\n';
        return 1;
    }
    const auto info = waibusnap::createRecordingMediaProbe()->probe(temporary);
    const auto bytes = QFileInfo(temporary).size();
    if (!info.valid || info.pixelSize != QSize(kWidth, kHeight) || info.durationMs < 900 ||
        info.durationMs > 1100 || bytes <= 0 || bytes >= 100 * 1024)
    {
        QTextStream(stderr) << "阶段=成品规格校验失败：" << info.error
                            << " 时长=" << info.durationMs << " 尺寸=" << info.pixelSize.width()
                            << 'x' << info.pixelSize.height() << " 字节=" << bytes << '\n';
        return 1;
    }
    if (!QFile::copy(temporary, destination))
    {
        QTextStream(stderr) << "阶段=发布夹具失败，未覆盖已有文件\n";
        return 1;
    }
    QTextStream(stdout) << "生成成功：" << destination << "，" << bytes << " 字节，"
                        << info.durationMs << " ms，320x240 H.264 / 仅视频轨\n";
    return 0;
}
