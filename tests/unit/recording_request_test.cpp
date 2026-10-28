#include "interfaces/recording.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

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
        QCOMPARE(waibusnap::recordingCaptureSupported(), true);
        const bool permitted = waibusnap::hasScreenRecordingPermission();
        Q_UNUSED(permitted);
        QVERIFY(waibusnap::createRecordingSession());
        QVERIFY(waibusnap::createRecordingMediaProbe());
    }
    void rejectsInvalidRequests_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("expected");
        QTest::newRow("gif") << QStringLiteral("gif") << QStringLiteral("R5");
        QTest::newRow("microphone") << QStringLiteral("microphone") << QStringLiteral("R3");
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
        else if (kind == QStringLiteral("microphone"))
            request.microphoneDeviceId = QStringLiteral("usb-mic");
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
        QVERIFY(session);
        QString error;
        const bool started = session->start(request, {}, &error);
        QVERIFY(!started);
        QVERIFY2(error.contains(expected), qPrintable(error));
        QVERIFY(!QFileInfo::exists(request.tempPath));
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
            QVERIFY(!session->start(request, {}, &error));
            QVERIFY2(error.contains(QStringLiteral("临时文件已存在")), qPrintable(error));
            QVERIFY(QFileInfo::exists(request.tempPath));
        }
    }
    void ignoresStopBeforeStart()
    {
        const auto session = waibusnap::createRecordingSession();
        QVERIFY(session);
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
