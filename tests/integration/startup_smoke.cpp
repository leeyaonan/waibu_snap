#include "ui/selection_overlay.h"
#include <QApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QString>
#include <QTest>

namespace
{
// Windows 无控制台时 Qt 日志默认走调试器而不是 stderr；测试依赖子进程 stderr，
// 显式启用官方开关（QT_FORCE_STDERR_LOGGING），双端行为一致。
QProcessEnvironment childProcessEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    return environment;
}
} // namespace
class StartupSmokeTest final : public QObject
{
    Q_OBJECT
  private slots:
    void overlayCanPaintSelectAndCancel()
    {
        waibusnap::CaptureFrame frame;
        frame.display.logicalGeometry = QRect(0, 0, 320, 200);
        frame.display.devicePixelRatio = 2;
        frame.pixels = QImage(640, 400, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(Qt::red);
        frame.pixels.setDevicePixelRatio(2);
        waibusnap::SelectionOverlay overlay(frame);
        QSignalSpy painted(&overlay, &waibusnap::SelectionOverlay::firstPaintCompleted);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        QTRY_COMPARE(painted.count(), 1);
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(80, 60));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(20, 10));
        QCOMPARE(finished.count(), 0);
        QCOMPARE(overlay.selection(), QRect(40, 20, 120, 100));
        QVERIFY(overlay.isVisible());
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(50, 35));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(70, 55));
        QCOMPARE(overlay.selection(), QRect(80, 60, 120, 100));
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(100, 80));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(120, 100));
        QCOMPARE(overlay.selection(), QRect(80, 60, 160, 140));
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QVERIFY(!overlay.isVisible());
        waibusnap::SelectionOverlay cancelled(frame);
        QSignalSpy cancellation(&cancelled, &waibusnap::SelectionOverlay::finished);
        cancelled.show();
        QTest::keyClick(&cancelled, Qt::Key_Escape);
        QCOMPARE(cancellation.count(), 1);
        QVERIFY(!cancellation.first().at(1).toBool());
        QVERIFY(!cancelled.isVisible());
    }
    void applicationCanStartAndExit()
    {
        const QString executable = qEnvironmentVariable("WAIBUSNAP_TEST_APP");
        QVERIFY2(!executable.isEmpty(), "请通过 CTest 设置应用路径后运行。");
        QProcess process;
        process.setProgram(executable);
        process.setArguments({QStringLiteral("--smoke-test")});
        process.setProcessEnvironment(childProcessEnvironment());
        process.start();
        QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));
        QVERIFY2(process.waitForFinished(10000), qPrintable(process.errorString()));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        const QByteArray diagnostics = process.readAllStandardError();
        QVERIFY2(process.exitCode() == 0,
                 qPrintable(QStringLiteral("exitStatus=%1 exitCode=%2 stderr=%3")
                                .arg(process.exitStatus() == QProcess::NormalExit
                                         ? QStringLiteral("NormalExit")
                                         : QStringLiteral("CrashExit"))
                                .arg(process.exitCode())
                                .arg(QString::fromUtf8(diagnostics))));
        // 标记自带 ASCII 片段，避免 Windows 本地 8 位编码转换影响断言。
        QVERIFY(diagnostics.contains("tray-lifecycle-verified"));
    }
    void injectionRequiresExplicitTestMode()
    {
        QProcess process;
        process.setProcessEnvironment(childProcessEnvironment());
        process.start(qEnvironmentVariable("WAIBUSNAP_TEST_APP"), {"--test-count", "2"});
        QVERIFY2(process.waitForFinished(5000), qPrintable(process.errorString()));
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("--test-mode"));
    }
};
QTEST_MAIN(StartupSmokeTest)
#include "startup_smoke.moc"
