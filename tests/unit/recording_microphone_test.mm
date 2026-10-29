#include "interfaces/recording.h"
#include <QCoreApplication>
#include <QSet>
#include <QThread>
#include <QtTest>
class RecordingMicrophoneTest final : public QObject
{
    Q_OBJECT
  private slots:
    void readOnlyDiscovery()
    {
        const auto before = waibusnap::microphonePermission();
        QVERIFY(before == waibusnap::MicrophonePermission::Authorized ||
                before == waibusnap::MicrophonePermission::Denied ||
                before == waibusnap::MicrophonePermission::NotDetermined);
        const auto devices = waibusnap::microphoneDevices();
        const QString defaultId = waibusnap::defaultMicrophoneDeviceId();
        QSet<QString> ids;
        int defaults = 0;
        QString previousName;
        for (const auto& device : devices)
        {
            QVERIFY(!device.id.isEmpty());
            QVERIFY(!ids.contains(device.id));
            ids.insert(device.id);
            if (device.isDefault)
            {
                ++defaults;
                QCOMPARE(device.id, defaultId);
            }
            QVERIFY(previousName <= device.name);
            previousName = device.name;
        }
        QVERIFY(defaults <= 1);
        if (!defaultId.isEmpty())
        {
            QVERIFY(ids.contains(defaultId));
            QCOMPARE(defaults, 1);
        }
        // 只查询，不调用 requestAccess；CI 没有设备时空列表同样合法。
        QCOMPARE(waibusnap::microphonePermission(), before);
        QVERIFY(!waibusnap::microphonePermissionExplanation().isEmpty());
    }
    void determinedPermissionCompletesAsynchronously()
    {
        const auto permission = waibusnap::microphonePermission();
        if (permission == waibusnap::MicrophonePermission::NotDetermined)
            QSKIP("未决定权限时不请求授权，避免 CI 弹窗。", QTest::SkipSingle);
        bool called = false;
        bool granted = false;
        QThread* callbackThread = nullptr;
        waibusnap::requestMicrophonePermission(
            [&](bool allowed)
            {
                called = true;
                granted = allowed;
                callbackThread = QThread::currentThread();
            });
        QVERIFY(!called);
        QTRY_VERIFY(called);
        QCOMPARE(granted, permission == waibusnap::MicrophonePermission::Authorized);
        QCOMPARE(callbackThread, QCoreApplication::instance()->thread());
    }
};
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    RecordingMicrophoneTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "recording_microphone_test.moc"
