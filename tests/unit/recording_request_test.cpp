#include "interfaces/recording.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

namespace
{
waibusnap::RecordingRequest baseRequest(const QTemporaryDir& directory)
{
    waibusnap::RecordingRequest request;
    request.output = waibusnap::RecordingOutput::Video;
    request.targetKind = waibusnap::RecordingTargetKind::Display;
    request.display.id = 1;
    request.display.logicalGeometry = QRect(0, 0, 1920, 1080);
    request.display.devicePixelRatio = 2.0;
    request.sourcePixelRect = QRect(0, 0, 3840, 2160);
    request.outputPixels = QSize(1280, 720);
    request.fps = 30;
    request.quality = waibusnap::RecordingQuality::Low;
    request.systemAudio = false;
    request.tempPath = directory.filePath(QStringLiteral(".waibusnap-recording-test.mp4.part"));
    request.finalPath = directory.filePath(QStringLiteral("recording-test.mp4"));
    return request;
}
} // namespace

class RecordingRequestTest final : public QObject
{
    Q_OBJECT
  private slots:
    void capabilities()
    {
        const bool supported = waibusnap::recordingCaptureSupported();
        const bool permitted = waibusnap::hasScreenRecordingPermission();
        Q_UNUSED(permitted);
        QCOMPARE(bool(waibusnap::createRecordingSession()), supported);
        QVERIFY(waibusnap::createRecordingMediaProbe());
        if (!supported)
        {
            QCOMPARE(waibusnap::microphonePermission(),
                     waibusnap::MicrophonePermission::NotDetermined);
            QVERIFY(waibusnap::microphoneDevices().isEmpty());
            QVERIFY(waibusnap::defaultMicrophoneDeviceId().isEmpty());
            bool called = false;
            bool granted = true;
            waibusnap::requestMicrophonePermission(
                [&](bool value)
                {
                    called = true;
                    granted = value;
                });
            QVERIFY(!called);
            QTRY_VERIFY(called);
            QVERIFY(!granted);
        }
    }
    void rejectsInvalidRequests_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("expected");
        QTest::newRow("gif") << QStringLiteral("gif") << QStringLiteral("R5");
        QTest::newRow("microphone-nul")
            << QStringLiteral("microphone-nul") << QStringLiteral("非法字符");
        QTest::newRow("microphone-control")
            << QStringLiteral("microphone-control") << QStringLiteral("非法字符");
        QTest::newRow("zero-size") << QStringLiteral("zero-size") << QStringLiteral("正数");
        QTest::newRow("odd-size") << QStringLiteral("odd-size") << QStringLiteral("偶数");
        QTest::newRow("fps") << QStringLiteral("fps") << QStringLiteral("30 或 60");
        QTest::newRow("relative") << QStringLiteral("relative") << QStringLiteral("绝对路径");
        QTest::newRow("extension") << QStringLiteral("extension") << QStringLiteral(".part");
        QTest::newRow("same-path") << QStringLiteral("same-path") << QStringLiteral("不能与成品");
        QTest::newRow("display") << QStringLiteral("display") << QStringLiteral("显示器");
        QTest::newRow("region") << QStringLiteral("region") << QStringLiteral("超出");
        QTest::newRow("window") << QStringLiteral("window") << QStringLiteral("窗口");
        QTest::newRow("missing-directory")
            << QStringLiteral("missing-directory") << QStringLiteral("不存在");
    }
    void rejectsInvalidRequests()
    {
        QFETCH(QString, kind);
        QFETCH(QString, expected);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        waibusnap::RecordingRequest request = baseRequest(directory);
        if (kind == QStringLiteral("gif"))
            request.output = waibusnap::RecordingOutput::Gif;
        else if (kind == QStringLiteral("microphone-nul"))
            request.microphoneDeviceId = QStringLiteral("usb") + QChar(0) + QStringLiteral("mic");
        else if (kind == QStringLiteral("microphone-control"))
            request.microphoneDeviceId = QStringLiteral("usb\nmic");
        else if (kind == QStringLiteral("zero-size"))
            request.outputPixels = QSize(0, 720);
        else if (kind == QStringLiteral("odd-size"))
            request.outputPixels = QSize(1279, 719);
        else if (kind == QStringLiteral("fps"))
            request.fps = 12;
        else if (kind == QStringLiteral("relative"))
            request.tempPath = QStringLiteral("relative.mp4.part");
        else if (kind == QStringLiteral("extension"))
            request.tempPath = directory.filePath(QStringLiteral("recording.part.tmp"));
        else if (kind == QStringLiteral("same-path"))
            request.finalPath = request.tempPath;
        else if (kind == QStringLiteral("display"))
            request.display.id = 0;
        else if (kind == QStringLiteral("region"))
        {
            request.targetKind = waibusnap::RecordingTargetKind::Region;
            request.sourcePixelRect = QRect(3000, 1000, 1200, 1200);
        }
        else if (kind == QStringLiteral("window"))
            request.targetKind = waibusnap::RecordingTargetKind::Window;
        else if (kind == QStringLiteral("missing-directory"))
            request.tempPath = directory.filePath(QStringLiteral("missing/.waibusnap.mp4.part"));

        const auto session = waibusnap::createRecordingSession();
        const QString validation = waibusnap::validateRecordingRequest(request);
        QVERIFY2(validation.contains(expected), qPrintable(validation));
        if (session)
        {
            QString error;
            QVERIFY(!session->start(request, {}, &error));
            QCOMPARE(error, validation);
        }
        QVERIFY(!QFileInfo::exists(request.tempPath));
    }
    void acceptsMicrophoneWithoutStarting()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto request = baseRequest(directory);
        QVERIFY(!request.microphoneEnabled);
        request.microphoneEnabled = true;
        QVERIFY(request.microphoneDeviceId.isEmpty());
        QVERIFY(waibusnap::validateRecordingRequest(request).isEmpty());
        request.microphoneDeviceId = QStringLiteral("指定的离线设备");
        QVERIFY(waibusnap::validateRecordingRequest(request).isEmpty());
        for (double gain : {0.0, 1.0, 2.0})
        {
            request.systemGain = gain;
            request.microphoneGain = gain;
            QVERIFY(waibusnap::validateRecordingRequest(request).isEmpty());
        }
        QVERIFY(!QFileInfo::exists(request.tempPath));
    }
    void rejectsInvalidGains()
    {
        QTemporaryDir directory;
        for (double gain : {-0.01, 2.01, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()})
        {
            auto request = baseRequest(directory);
            request.systemGain = gain;
            QVERIFY(waibusnap::validateRecordingRequest(request).contains(QStringLiteral("增益")));
            request.systemGain = 1;
            request.microphoneGain = gain;
            QVERIFY(waibusnap::validateRecordingRequest(request).contains(QStringLiteral("增益")));
        }
    }
    void rejectsExistingPaths()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        {
            waibusnap::RecordingRequest request = baseRequest(directory);
            QFile finalFile(request.finalPath);
            QVERIFY(finalFile.open(QIODevice::WriteOnly));
            finalFile.write("existing");
            finalFile.close();
            const auto session = waibusnap::createRecordingSession();
            QString error;
            error = waibusnap::validateRecordingRequest(request);
            if (session)
                QVERIFY(!session->start(request, {}, &error));
            QVERIFY2(error.contains(QStringLiteral("已存在")), qPrintable(error));
            QVERIFY(!QFileInfo::exists(request.tempPath));
        }
        {
            waibusnap::RecordingRequest request = baseRequest(directory);
            request.finalPath = directory.filePath(QStringLiteral("second.mp4"));
            QFile temporary(request.tempPath);
            QVERIFY(temporary.open(QIODevice::WriteOnly));
            temporary.write("stale");
            temporary.close();
            const auto session = waibusnap::createRecordingSession();
            QString error;
            error = waibusnap::validateRecordingRequest(request);
            if (session)
                QVERIFY(!session->start(request, {}, &error));
            QVERIFY2(error.contains(QStringLiteral("临时文件已存在")), qPrintable(error));
            QVERIFY(QFileInfo::exists(request.tempPath));
        }
    }
    void ignoresStopBeforeStart()
    {
        const auto session = waibusnap::createRecordingSession();
        if (!session)
            return;
        session->pause();
        session->resume();
        session->stop();
        session->discard();
    }
};

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    RecordingRequestTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "recording_request_test.moc"
