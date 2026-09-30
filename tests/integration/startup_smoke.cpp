#include "ui/startup_window.h"

#include <QProcess>
#include <QTest>

class StartupSmokeTest final : public QObject
{
    Q_OBJECT

  private slots:
    void windowCanOpenAndClose()
    {
        // 双端：验证实际窗口的可见性及关闭后的状态。
        waibusnap::StartupWindow window;
        window.show();
        QTRY_VERIFY(window.isVisible());
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
    }

    void applicationCanStartAndExit()
    {
        // 双端：启动真正的 .app 内可执行文件 / .exe，验证事件循环和动态库加载。
        const QString executable = qEnvironmentVariable("WAIBUSNAP_TEST_APP");
        QVERIFY2(!executable.isEmpty(), "请通过 CTest 设置应用路径后运行。");

        QProcess process;
        process.setProgram(executable);
        process.setArguments({QStringLiteral("--smoke-test")});
        process.start();
        QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));
        QVERIFY2(process.waitForFinished(10000), qPrintable(process.errorString()));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
    }
};

QTEST_MAIN(StartupSmokeTest)
#include "startup_smoke.moc"
