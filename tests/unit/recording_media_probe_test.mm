#include "interfaces/recording.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>

namespace
{
bool writeTestVideo(const QString& path, int width, int height, int frames, int fps)
{
    @autoreleasepool
    {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        NSError* error = nil;
        AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                          fileType:AVFileTypeMPEG4
                                                             error:&error];
        if (!writer)
            return false;
        NSDictionary* settings = @{
            AVVideoCodecKey : AVVideoCodecTypeH264,
            AVVideoWidthKey : @(width),
            AVVideoHeightKey : @(height),
        };
        AVAssetWriterInput* input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                                   outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;
        NSDictionary* attributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey : @(width),
            (id)kCVPixelBufferHeightKey : @(height),
        };
        AVAssetWriterInputPixelBufferAdaptor* adaptor =
            [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:input
                                                       sourcePixelBufferAttributes:attributes];
        if (![writer canAddInput:input])
            return false;
        [writer addInput:input];
        if (![writer startWriting])
            return false;
        [writer startSessionAtSourceTime:kCMTimeZero];
        CVPixelBufferPoolRef pool = adaptor.pixelBufferPool;
        if (!pool)
            return false;
        for (int frame = 0; frame < frames; ++frame)
        {
            for (int attempt = 0; attempt < 5000 && !input.isReadyForMoreMediaData; ++attempt)
                ::usleep(1000);
            if (!input.isReadyForMoreMediaData)
                return false;
            CVPixelBufferRef buffer = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) !=
                kCVReturnSuccess)
                return false;
            CVPixelBufferLockBaseAddress(buffer, 0);
            auto* pixels = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    uint8_t* pixel = pixels + y * stride + x * 4;
                    pixel[0] = uint8_t(frame * 7 + x % 256);
                    pixel[1] = uint8_t(y % 256);
                    pixel[2] = uint8_t(180);
                    pixel[3] = 255;
                }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            const BOOL appended = [adaptor appendPixelBuffer:buffer
                                        withPresentationTime:CMTimeMake(frame, fps)];
            CVPixelBufferRelease(buffer);
            if (!appended)
                return false;
        }
        [input markAsFinished];
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
          dispatch_semaphore_signal(semaphore);
        }];
        if (dispatch_semaphore_wait(semaphore,
                                    dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0)
            return false;
        return writer.status == AVAssetWriterStatusCompleted;
    }
}
} // namespace

class RecordingMediaProbeTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        videoPath_ = directory_.filePath(QStringLiteral("probe-video.mp4"));
        QVERIFY(writeTestVideo(videoPath_, 320, 240, 30, 30));
        QVERIFY(QFileInfo::exists(videoPath_));
    }
    void probesDurationAndPixels()
    {
        const auto probe = waibusnap::createRecordingMediaProbe();
        QVERIFY(probe);
        const auto info = probe->probe(videoPath_);
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY2(info.durationMs >= 900 && info.durationMs <= 1100,
                 qPrintable(QStringLiteral("时长 %1 ms").arg(info.durationMs)));
        QCOMPARE(info.pixelSize, QSize(320, 240));
    }
    void thumbnailRespectsBounds()
    {
        const auto probe = waibusnap::createRecordingMediaProbe();
        QVERIFY(probe);
        const QImage thumbnail = probe->thumbnail(videoPath_, QSize(160, 160));
        QVERIFY(!thumbnail.isNull());
        QVERIFY(thumbnail.width() <= 160);
        QVERIFY(thumbnail.height() <= 160);
        QVERIFY(thumbnail.width() > 0 && thumbnail.height() > 0);
    }
    void rejectsMissingAndInvalidFiles()
    {
        const auto probe = waibusnap::createRecordingMediaProbe();
        QVERIFY(probe);
        const auto missing = probe->probe(directory_.filePath(QStringLiteral("missing.mp4")));
        QVERIFY(!missing.valid);
        QVERIFY(!missing.error.isEmpty());
        QVERIFY(
            probe->thumbnail(directory_.filePath(QStringLiteral("missing.mp4")), QSize(160, 160))
                .isNull());

        const QString textPath = directory_.filePath(QStringLiteral("not-video.mp4"));
        QFile text(textPath);
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("这不是视频文件。");
        text.close();
        const auto invalid = probe->probe(textPath);
        QVERIFY(!invalid.valid);
        QVERIFY(!invalid.error.isEmpty());
        QVERIFY(invalid.error.contains(QStringLiteral("无法读取")));
    }
    void defersGifProbe()
    {
        const auto probe = waibusnap::createRecordingMediaProbe();
        QVERIFY(probe);
        const QString gifPath = directory_.filePath(QStringLiteral("placeholder.gif"));
        QFile gif(gifPath);
        QVERIFY(gif.open(QIODevice::WriteOnly));
        gif.write("GIF89a");
        gif.close();
        const auto info = probe->probe(gifPath);
        QVERIFY(!info.valid);
        QVERIFY2(info.error.contains(QStringLiteral("R6")), qPrintable(info.error));
    }

  private:
    QTemporaryDir directory_;
    QString videoPath_;
};

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    RecordingMediaProbeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "recording_media_probe_test.moc"
