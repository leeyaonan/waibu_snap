#include "app/app_settings.h"
#include "core/recording.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
namespace
{
const QDateTime fixedTimestamp(QDate(2026, 10, 28), QTime(12, 34, 56, 789));
QByteArray fileBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}
class RecordingCoreTest final : public QObject
{
    Q_OBJECT
  private slots:
    void directoryDraftKeepsDefaultsAndExplicitInvalidValues()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        QVERIFY(settings.loadRecordingPreferences(nullptr, false).videoDirectory.isEmpty());
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("recording/videoDirectory"),
                         QStringLiteral("relative/old"));
            ini.setValue(QStringLiteral("recording/gifDirectory"), defaultRecordingGifDirectory());
            ini.sync();
        }
        const auto before = fileBytes(path);
        const auto draft = settings.loadRecordingPreferences(nullptr, false);
        QCOMPARE(draft.videoDirectory, QStringLiteral("relative/old"));
        QCOMPARE(draft.gifDirectory, defaultRecordingGifDirectory());
        QVERIFY(!settings.saveRecordingPreferences(draft).isEmpty());
        QCOMPARE(fileBytes(path), before);
        QCOMPARE(settings.loadRecordingPreferences().videoDirectory,
                 defaultRecordingVideoDirectory());
    }
    void defaultDirectoriesUseExpectedSuffixes()
    {
        const QString video = defaultRecordingVideoDirectory();
        QVERIFY(QDir::isAbsolutePath(video));
        QCOMPARE(QFileInfo(video).fileName(), QStringLiteral("WaibuSnap"));
        const QString gif = defaultRecordingGifDirectory();
        QVERIFY(QDir::isAbsolutePath(gif));
        QCOMPARE(QFileInfo(gif).fileName(), QStringLiteral("GIF"));
        QCOMPARE(QFileInfo(QFileInfo(gif).absolutePath()).fileName(), QStringLiteral("WaibuSnap"));
    }
    void suggestedPathUsesFixedTimestampAndExtensions()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString video =
            suggestedRecordingPath(temporary.path(), RecordingOutput::Video, fixedTimestamp);
        QCOMPARE(QFileInfo(video).fileName(),
                 QStringLiteral("WaibuSnap_2026-10-28_12-34-56-789.mp4"));
        const QString gif =
            suggestedRecordingPath(temporary.path(), RecordingOutput::Gif, fixedTimestamp);
        QCOMPARE(QFileInfo(gif).fileName(),
                 QStringLiteral("WaibuSnap_2026-10-28_12-34-56-789.gif"));
        QVERIFY(!QFileInfo::exists(video));
        QVERIFY(!QFileInfo::exists(gif));
    }
    void suggestedPathFallsBackForEmptyOrRelativeDirectory()
    {
        const QString video =
            suggestedRecordingPath(QString(), RecordingOutput::Video, fixedTimestamp);
        QCOMPARE(video, QDir(defaultRecordingVideoDirectory())
                            .filePath(QStringLiteral("WaibuSnap_2026-10-28_12-34-56-789.mp4")));
        const QString relative = suggestedRecordingPath(QStringLiteral("relative/path"),
                                                        RecordingOutput::Video, fixedTimestamp);
        QCOMPARE(relative, video);
        const QString gif = suggestedRecordingPath(QStringLiteral("relative/path"),
                                                   RecordingOutput::Gif, fixedTimestamp);
        QCOMPARE(gif, QDir(defaultRecordingGifDirectory())
                          .filePath(QStringLiteral("WaibuSnap_2026-10-28_12-34-56-789.gif")));
    }
    void suggestedPathAvoidsCollisionsWithoutOverwriting()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir folder(temporary.path());
        const QString base = QStringLiteral("WaibuSnap_2026-10-28_12-34-56-789");
        QFile first(folder.filePath(base + QStringLiteral(".mp4")));
        QVERIFY(first.open(QIODevice::WriteOnly));
        QVERIFY(first.write("original") == qint64(8));
        first.close();
        QCOMPARE(QFileInfo(suggestedRecordingPath(temporary.path(), RecordingOutput::Video,
                                                  fixedTimestamp))
                     .fileName(),
                 base + QStringLiteral("_2.mp4"));
        QFile second(folder.filePath(base + QStringLiteral("_2.mp4")));
        QVERIFY(second.open(QIODevice::WriteOnly));
        second.close();
        QCOMPARE(QFileInfo(suggestedRecordingPath(temporary.path(), RecordingOutput::Video,
                                                  fixedTimestamp))
                     .fileName(),
                 base + QStringLiteral("_3.mp4"));
        QCOMPARE(fileBytes(folder.filePath(base + QStringLiteral(".mp4"))),
                 QByteArrayLiteral("original"));
    }
    void outputSize_data()
    {
        QTest::addColumn<QSize>("source");
        QTest::addColumn<int>("output");
        QTest::addColumn<int>("resolution");
        QTest::addColumn<int>("gifMaxEdge");
        QTest::addColumn<QSize>("expected");
        const int video = int(RecordingOutput::Video);
        const int gif = int(RecordingOutput::Gif);
        const int native = int(RecordingResolution::Native);
        const int p1080 = int(RecordingResolution::P1080);
        const int p720 = int(RecordingResolution::P720);
        QTest::newRow("video-native-1080p")
            << QSize(1920, 1080) << video << native << 1280 << QSize(1920, 1080);
        QTest::newRow("video-native-4k")
            << QSize(3840, 2160) << video << native << 1280 << QSize(3840, 2160);
        QTest::newRow("video-native-even-floor")
            << QSize(1367, 769) << video << native << 1280 << QSize(1366, 768);
        QTest::newRow("video-1080p-4k")
            << QSize(3840, 2160) << video << p1080 << 1280 << QSize(1920, 1080);
        QTest::newRow("video-1080p-16-10")
            << QSize(2560, 1600) << video << p1080 << 1280 << QSize(1728, 1080);
        QTest::newRow("video-1080p-3-2")
            << QSize(2880, 1920) << video << p1080 << 1280 << QSize(1620, 1080);
        QTest::newRow("video-720p-square")
            << QSize(2000, 2000) << video << p720 << 1280 << QSize(720, 720);
        QTest::newRow("video-1080p-no-upscale")
            << QSize(1280, 720) << video << p1080 << 1280 << QSize(1280, 720);
        QTest::newRow("video-720p-no-upscale")
            << QSize(960, 540) << video << p720 << 1280 << QSize(960, 540);
        QTest::newRow("video-minimum") << QSize(1, 1) << video << native << 1280 << QSize(2, 2);
        QTest::newRow("video-minimum-one-axis")
            << QSize(3, 100) << video << native << 1280 << QSize(2, 100);
        QTest::newRow("gif-1280-4k")
            << QSize(3840, 2160) << gif << native << 1280 << QSize(1280, 720);
        QTest::newRow("gif-960-3-2")
            << QSize(2880, 1920) << gif << native << 960 << QSize(960, 640);
        QTest::newRow("gif-1280-no-upscale")
            << QSize(640, 480) << gif << native << 1280 << QSize(640, 480);
        QTest::newRow("gif-native-keeps-odd")
            << QSize(1367, 769) << gif << native << 0 << QSize(1367, 769);
        QTest::newRow("gif-1280-square")
            << QSize(4000, 4000) << gif << native << 1280 << QSize(1280, 1280);
        QTest::newRow("gif-minimum") << QSize(1, 1) << gif << native << 1280 << QSize(2, 2);
        QTest::newRow("invalid-width") << QSize(0, 1080) << video << native << 1280 << QSize();
        QTest::newRow("invalid-negative") << QSize(-5, 10) << gif << native << 1280 << QSize();
    }
    void outputSize()
    {
        QFETCH(QSize, source);
        QFETCH(int, output);
        QFETCH(int, resolution);
        QFETCH(int, gifMaxEdge);
        QFETCH(QSize, expected);
        QCOMPARE(recordingOutputSize(source, RecordingOutput(output),
                                     RecordingResolution(resolution), gifMaxEdge),
                 expected);
    }
    void videoBitrate_data()
    {
        QTest::addColumn<QSize>("pixels");
        QTest::addColumn<int>("fps");
        QTest::addColumn<int>("quality");
        QTest::addColumn<qint64>("expected");
        const int high = int(RecordingQuality::High);
        const int medium = int(RecordingQuality::Medium);
        const int low = int(RecordingQuality::Low);
        QTest::newRow("1080p30-high") << QSize(1920, 1080) << 30 << high << qint64(11197440);
        QTest::newRow("4k60-high-cap") << QSize(3840, 2160) << 60 << high << qint64(40000000);
        QTest::newRow("720p30-low-floor") << QSize(1280, 720) << 30 << low << qint64(2500000);
        QTest::newRow("1080p30-medium") << QSize(1920, 1080) << 30 << medium << qint64(7464960);
        QTest::newRow("4k60-medium-cap") << QSize(3840, 2160) << 60 << medium << qint64(24000000);
        QTest::newRow("small-high-floor") << QSize(640, 480) << 30 << high << qint64(6000000);
        QTest::newRow("invalid-pixels") << QSize() << 30 << high << qint64(6000000);
        QTest::newRow("invalid-fps") << QSize(1920, 1080) << 0 << high << qint64(6000000);
    }
    void videoBitrate()
    {
        QFETCH(QSize, pixels);
        QFETCH(int, fps);
        QFETCH(int, quality);
        QFETCH(qint64, expected);
        QCOMPARE(estimatedVideoBitrate(pixels, fps, RecordingQuality(quality)), expected);
    }
    void gifEstimateIsPositiveAndMonotonic()
    {
        const QSize pixels(1280, 720);
        const qint64 base = estimatedGifBytes(pixels, 12, 30);
        QCOMPARE(base, qint64(39814144));
        QVERIFY(base >= 39000000 && base <= 40000000);
        QVERIFY(base > 0);
        QVERIFY(estimatedGifBytes(pixels, 12, 60) > base);
        QVERIFY(estimatedGifBytes(pixels, 15, 30) > base);
        QVERIFY(estimatedGifBytes(QSize(1920, 1080), 12, 30) > base);
        QVERIFY(estimatedGifBytes(pixels, 0, 30) > 0);
        QCOMPARE(estimatedGifBytes(QSize(), 12, 30), qint64(1024));
        QCOMPARE(estimatedGifBytes(pixels, -1, 30), qint64(1024));
        QCOMPARE(estimatedGifBytes(pixels, 12, 0), qint64(1024));
    }
    void diskStateBoundaries()
    {
        constexpr qint64 mebibyte = 1024LL * 1024LL;
        QCOMPARE(recordingDiskState(2048 * mebibyte), RecordingDiskState::Normal);
        QCOMPARE(recordingDiskState(2048 * mebibyte - 1), RecordingDiskState::Low);
        QCOMPARE(recordingDiskState(512 * mebibyte), RecordingDiskState::Low);
        QCOMPARE(recordingDiskState(512 * mebibyte - 1), RecordingDiskState::Critical);
        QCOMPARE(recordingDiskState(0), RecordingDiskState::Critical);
        QCOMPARE(recordingDiskState(-1), RecordingDiskState::Critical);
    }
    void recordingPreferencesDefaultWithoutFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("设置 空格/settings.ini"));
        AppSettings settings(path);
        QString notice = QStringLiteral("旧提示");
        const auto loaded = settings.loadRecordingPreferences(&notice);
        QVERIFY(notice.isEmpty());
        QCOMPARE(loaded.videoDirectory, defaultRecordingVideoDirectory());
        QCOMPARE(loaded.gifDirectory, defaultRecordingGifDirectory());
        QCOMPARE(loaded.defaultOutput, RecordingOutput::Video);
        QCOMPARE(loaded.videoFps, 30);
        QCOMPARE(loaded.resolution, RecordingResolution::Native);
        QCOMPARE(loaded.quality, RecordingQuality::High);
        QCOMPARE(loaded.gifMaxEdge, 1280);
        QCOMPARE(loaded.gifFps, 12);
        QCOMPARE(loaded.gifMaxSeconds, 30);
        QCOMPARE(loaded.countdownSeconds, 3);
        QVERIFY(loaded.showCursor);
        QVERIFY(loaded.systemAudio);
        QVERIFY(!loaded.microphone);
        QVERIFY(loaded.microphoneDeviceId.isEmpty());
        QCOMPARE(loaded.systemVolumePercent, 100);
        QCOMPARE(loaded.microphoneVolumePercent, 100);
        QVERIFY(!QFile::exists(path));
    }
    void recordingPreferencesRoundTripCreatesDirectoriesAndKeepsOtherKeys()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("设置 空格/settings.ini"));
        AppSettings settings(path);
        QVERIFY(settings.saveHotkey(QKeySequence(Qt::Key_F5)).isEmpty());
        RecordingPreferences custom;
        custom.videoDirectory = temporary.filePath(QStringLiteral("视频 输出"));
        custom.gifDirectory = temporary.filePath(QStringLiteral("GIF 输出/深层"));
        custom.defaultOutput = RecordingOutput::Gif;
        custom.videoFps = 60;
        custom.resolution = RecordingResolution::P720;
        custom.quality = RecordingQuality::Low;
        custom.gifMaxEdge = 960;
        custom.gifFps = 15;
        custom.gifMaxSeconds = 60;
        custom.countdownSeconds = 5;
        custom.showCursor = false;
        custom.systemAudio = false;
        custom.microphone = true;
        custom.microphoneDeviceId = QStringLiteral("usb-mic-42");
        custom.systemVolumePercent = 37;
        custom.microphoneVolumePercent = 0;
        QVERIFY(!QFileInfo::exists(custom.videoDirectory));
        QVERIFY(settings.saveRecordingPreferences(custom).isEmpty());
        QVERIFY(QFileInfo(custom.videoDirectory).isDir());
        QVERIFY(QFileInfo(custom.gifDirectory).isDir());
        QString notice = QStringLiteral("旧提示");
        const auto loaded = AppSettings(path).loadRecordingPreferences(&notice);
        QVERIFY(notice.isEmpty());
        QCOMPARE(loaded.videoDirectory, QDir::cleanPath(custom.videoDirectory));
        QCOMPARE(loaded.gifDirectory, QDir::cleanPath(custom.gifDirectory));
        QCOMPARE(loaded.defaultOutput, RecordingOutput::Gif);
        QCOMPARE(loaded.videoFps, 60);
        QCOMPARE(loaded.resolution, RecordingResolution::P720);
        QCOMPARE(loaded.quality, RecordingQuality::Low);
        QCOMPARE(loaded.gifMaxEdge, 960);
        QCOMPARE(loaded.gifFps, 15);
        QCOMPARE(loaded.gifMaxSeconds, 60);
        QCOMPARE(loaded.countdownSeconds, 5);
        QVERIFY(!loaded.showCursor);
        QVERIFY(!loaded.systemAudio);
        QVERIFY(loaded.microphone);
        QCOMPARE(loaded.microphoneDeviceId, QStringLiteral("usb-mic-42"));
        QCOMPARE(loaded.systemVolumePercent, 37);
        QCOMPARE(loaded.microphoneVolumePercent, 0);
        QCOMPARE(settings.loadHotkey().sequence, QKeySequence(Qt::Key_F5));
        {
            QSettings ini(path, QSettings::IniFormat);
            QCOMPARE(ini.value(QStringLiteral("recording/output")).toString(),
                     QStringLiteral("gif"));
            QCOMPARE(ini.value(QStringLiteral("recording/resolution")).toString(),
                     QStringLiteral("720p"));
            QCOMPARE(ini.value(QStringLiteral("recording/quality")).toString(),
                     QStringLiteral("low"));
            QCOMPARE(ini.value(QStringLiteral("recording/gifMaxEdge")).toString(),
                     QStringLiteral("960"));
            QCOMPARE(ini.allKeys().size(), 17);
        }
        custom.gifMaxEdge = 0;
        QVERIFY(settings.saveRecordingPreferences(custom).isEmpty());
        {
            QSettings ini(path, QSettings::IniFormat);
            QCOMPARE(ini.value(QStringLiteral("recording/gifMaxEdge")).toString(),
                     QStringLiteral("native"));
        }
        QCOMPARE(AppSettings(path).loadRecordingPreferences().gifMaxEdge, 0);
        QCOMPARE(settings.loadHotkey().sequence, QKeySequence(Qt::Key_F5));
    }
    void recordingPreferencesInvalidValuesFallBackTogether()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("recording/videoDirectory"),
                         QStringLiteral("relative/video"));
            ini.setValue(QStringLiteral("recording/gifDirectory"), QStringLiteral("relative/gif"));
            ini.setValue(QStringLiteral("recording/output"), QStringLiteral("webm"));
            ini.setValue(QStringLiteral("recording/videoFps"), 45);
            ini.setValue(QStringLiteral("recording/resolution"), QStringLiteral("4k"));
            ini.setValue(QStringLiteral("recording/quality"), QStringLiteral("ultra"));
            ini.setValue(QStringLiteral("recording/gifMaxEdge"), 42);
            ini.setValue(QStringLiteral("recording/gifFps"), 30);
            ini.setValue(QStringLiteral("recording/gifMaxSeconds"), 10);
            ini.setValue(QStringLiteral("recording/countdownSeconds"), 1);
            ini.setValue(QStringLiteral("recording/showCursor"), QStringLiteral("yes"));
            ini.setValue(QStringLiteral("recording/systemAudio"), QStringLiteral("0"));
            ini.setValue(QStringLiteral("recording/microphone"), QStringLiteral("maybe"));
            ini.setValue(QStringLiteral("recording/systemVolumePercent"), 250);
            ini.setValue(QStringLiteral("recording/microphoneVolumePercent"), -10);
            ini.sync();
        }
        const QByteArray before = fileBytes(path);
        QString notice;
        const auto loaded = AppSettings(path).loadRecordingPreferences(&notice);
        QVERIFY(!notice.isEmpty());
        QVERIFY(notice.contains(QStringLiteral("视频目录")));
        QVERIFY(notice.contains(QStringLiteral("默认输出")));
        QVERIFY(notice.contains(QStringLiteral("系统声音音量已修正")));
        QCOMPARE(fileBytes(path), before);
        QCOMPARE(loaded.videoDirectory, defaultRecordingVideoDirectory());
        QCOMPARE(loaded.gifDirectory, defaultRecordingGifDirectory());
        QCOMPARE(loaded.defaultOutput, RecordingOutput::Video);
        QCOMPARE(loaded.videoFps, 30);
        QCOMPARE(loaded.resolution, RecordingResolution::Native);
        QCOMPARE(loaded.quality, RecordingQuality::High);
        QCOMPARE(loaded.gifMaxEdge, 1280);
        QCOMPARE(loaded.gifFps, 12);
        QCOMPARE(loaded.gifMaxSeconds, 30);
        QCOMPARE(loaded.countdownSeconds, 3);
        QVERIFY(loaded.showCursor);
        QVERIFY(loaded.systemAudio);
        QVERIFY(!loaded.microphone);
        QCOMPARE(loaded.systemVolumePercent, 200);
        QCOMPARE(loaded.microphoneVolumePercent, 0);
    }
    void recordingPreferencesMalformedFilePreservesBytes()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray malformed("[recording\noutput=gif\n");
        QVERIFY(file.write(malformed) == qint64(malformed.size()));
        file.close();
        QString notice;
        const auto loaded = AppSettings(path).loadRecordingPreferences(&notice);
        QVERIFY(!notice.isEmpty());
        QVERIFY(notice.contains(QStringLiteral("损坏")));
        QCOMPARE(loaded.defaultOutput, RecordingOutput::Video);
        QCOMPARE(fileBytes(path), malformed);
    }
    void recordingPreferencesUnreadablePathFallsBack()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        QVERIFY(QDir().mkpath(path));
        AppSettings settings(path);
        QString notice;
        const auto loaded = settings.loadRecordingPreferences(&notice);
        QVERIFY(!notice.isEmpty());
        QCOMPARE(loaded.videoDirectory, defaultRecordingVideoDirectory());
        QVERIFY(!settings.saveRecordingPreferences(RecordingPreferences{}).isEmpty());
    }
    void recordingPreferencesSaveValidation()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        RecordingPreferences valid;
        valid.videoDirectory = temporary.filePath(QStringLiteral("video"));
        valid.gifDirectory = temporary.filePath(QStringLiteral("gif"));
        valid.defaultOutput = RecordingOutput::Gif;
        valid.videoFps = 60;
        valid.systemVolumePercent = 200;
        QVERIFY(settings.saveRecordingPreferences(valid).isEmpty());
        const auto assertPreserved = [&]
        {
            const auto loaded = AppSettings(path).loadRecordingPreferences();
            QCOMPARE(loaded.videoDirectory, QDir::cleanPath(valid.videoDirectory));
            QCOMPARE(loaded.gifDirectory, QDir::cleanPath(valid.gifDirectory));
            QCOMPARE(loaded.defaultOutput, RecordingOutput::Gif);
            QCOMPARE(loaded.videoFps, 60);
            QCOMPARE(loaded.systemVolumePercent, 200);
        };
        auto invalid = valid;
        invalid.videoDirectory = QStringLiteral("relative");
        QVERIFY(settings.saveRecordingPreferences(invalid).contains(QStringLiteral("绝对路径")));
        assertPreserved();
        invalid = valid;
        invalid.defaultOutput = static_cast<RecordingOutput>(99);
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.videoFps = 45;
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.resolution = static_cast<RecordingResolution>(99);
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.quality = static_cast<RecordingQuality>(99);
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.gifMaxEdge = 42;
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.gifFps = 30;
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.gifMaxSeconds = 10;
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.countdownSeconds = 1;
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.systemVolumePercent = 201;
        QVERIFY(settings.saveRecordingPreferences(invalid).contains(QStringLiteral("0–200")));
        assertPreserved();
        invalid = valid;
        invalid.microphoneDeviceId = QStringLiteral("bad");
        invalid.microphoneDeviceId.append(QChar(QChar::Null));
        invalid.microphoneDeviceId.append(QStringLiteral("id"));
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
        invalid = valid;
        invalid.videoDirectory = path;
        QVERIFY(!settings.saveRecordingPreferences(invalid).isEmpty());
        assertPreserved();
    }
};
QTEST_GUILESS_MAIN(RecordingCoreTest)
#include "recording_core_test.moc"
