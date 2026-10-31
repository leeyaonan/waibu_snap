#include "interfaces/recording.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class RecordingMediaProbeTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        videoPath_ = directory_.filePath(QStringLiteral("probe-video.mp4"));
        const QString fixture = QFINDTESTDATA("../fixtures/recording/probe-video.mp4");
        if (fixture.isEmpty())
            QSKIP("缺少 tests/fixtures/recording/probe-video.mp4，请完整检出仓库测试夹具。");
        QFile source(fixture);
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.errorString()));
        QVERIFY2(source.size() > 0 && source.size() < 100 * 1024,
                 "媒体夹具必须非空且小于 100 KiB，请按 tests/README.md 重新生成。");
        QVERIFY(!source.readAll().isEmpty());
        QVERIFY2(QFile::copy(fixture, videoPath_), "无法复制媒体夹具到临时目录。");
        // 已存在的夹具损坏或被替换必须失败，不能以跳过掩盖探测回归。
        const auto probe = waibusnap::createRecordingMediaProbe();
        QVERIFY(probe);
        const auto info = probe->probe(videoPath_);
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY2(info.durationMs >= 900 && info.durationMs <= 1100,
                 qPrintable(QStringLiteral("夹具时长 %1 ms").arg(info.durationMs)));
        QCOMPARE(info.pixelSize, QSize(320, 240));
        @autoreleasepool
        {
            AVURLAsset* asset =
                [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:videoPath_.toNSString()]
                                    options:nil];
            QCOMPARE(asset.tracks.count, NSUInteger(1));
            AVAssetTrack* track = asset.tracks.firstObject;
            QVERIFY([track.mediaType isEqualToString:AVMediaTypeVideo]);
            QVERIFY(track.formatDescriptions.count > 0);
            const auto format =
                (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
            QCOMPARE(CMFormatDescriptionGetMediaSubType(format), kCMVideoCodecType_H264);
        }
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
    void rejectsTruncatedGif()
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
        QVERIFY2(info.error.contains(QStringLiteral("GIF")), qPrintable(info.error));
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
