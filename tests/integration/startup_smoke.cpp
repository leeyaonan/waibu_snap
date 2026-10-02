#include "ui/selection_overlay.h"
#include <QApplication>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QTest>

namespace
{
waibusnap::CaptureFrame sampleFrame()
{
    waibusnap::CaptureFrame frame;
    frame.display.logicalGeometry = QRect(0, 0, 320, 200);
    frame.display.devicePixelRatio = 2;
    frame.pixels = QImage(640, 400, QImage::Format_ARGB32_Premultiplied);
    frame.pixels.fill(Qt::red);
    frame.pixels.setPixelColor(50, 30, Qt::green);
    frame.pixels.setDevicePixelRatio(2);
    return frame;
}
void drag(waibusnap::SelectionOverlay& overlay, QPoint first, QPoint second)
{
    QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(&overlay, second);
    QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, second);
}
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
    void copyFailureCanRetryAndEndsOnlyOnSuccess()
    {
        int calls = 0;
        QImage received;
        waibusnap::OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            received = image;
            ++calls;
            return waibusnap::ImageOutputResult{calls > 1, QStringLiteral("测试剪贴板不可用")};
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        QCOMPARE(calls, 0);
        auto* button = overlay.findChild<QPushButton*>(QStringLiteral("copyButton"));
        QVERIFY(button && button->isVisible());
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(calls, 1);
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.isVisible());
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status && status->text().contains(QStringLiteral("复制失败")));
        QCOMPARE(received.size(), QSize(120, 100));
        QCOMPARE(received.devicePixelRatio(), qreal(1));
        QCOMPARE(received.pixelColor(10, 10), QColor(Qt::green));
        QCOMPARE(received.pixelColor(0, 0), QColor(Qt::red));
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(calls, 2);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toRect(), QRect(40, 20, 120, 100));
        QCOMPARE(finished.first().at(1).toInt(), 10);
        QVERIFY(!overlay.isVisible());
    }
    void cancelButtonDoesNotInvokeCopy()
    {
        int calls = 0;
        waibusnap::OverlayActions actions;
        actions.copyImage = [&](const QImage&)
        {
            ++calls;
            return waibusnap::ImageOutputResult{true, {}};
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        auto* button = overlay.findChild<QPushButton*>(QStringLiteral("cancelButton"));
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(calls, 0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 2);
        QVERIFY(!overlay.isVisible());
    }
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
        QCOMPARE(finished.first().at(1).toInt(), 2);
        QVERIFY(!overlay.isVisible());
        waibusnap::SelectionOverlay cancelled(frame);
        QSignalSpy cancellation(&cancelled, &waibusnap::SelectionOverlay::finished);
        cancelled.show();
        QTest::keyClick(&cancelled, Qt::Key_Escape);
        QCOMPARE(cancellation.count(), 1);
        QCOMPARE(cancellation.first().at(1).toInt(), 2);
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
