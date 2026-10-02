#include "ui/selection_overlay.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
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
    void cancelWithoutOutputs_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("escape") << 0;
        QTest::newRow("button") << 1;
        QTest::newRow("close") << 2;
    }
    void cancelWithoutOutputs()
    {
        QFETCH(int, action);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("原文件.png"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original"), qint64(8));
        file.close();
        const QStringList before =
            QDir(temporary.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        int copies = 0;
        int dialogs = 0;
        waibusnap::OverlayActions actions;
        actions.copyImage = [&](const QImage&)
        {
            ++copies;
            return waibusnap::ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++dialogs;
            return path;
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        if (action == 0)
            QTest::keyClick(&overlay, Qt::Key_Escape);
        else if (action == 1)
        {
            auto* button = overlay.findChild<QPushButton*>(QStringLiteral("cancelButton"));
            QVERIFY(button);
            QTest::mouseClick(button, Qt::LeftButton);
        }
        else
            overlay.close();
        QCOMPARE(copies, 0);
        QCOMPARE(dialogs, 0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 2);
        QVERIFY(!overlay.isVisible());
        QCOMPARE(QDir(temporary.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
    }
    void savingKeepsSessionAndMarksChangesDirty()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        int copies = 0;
        int dialogs = 0;
        waibusnap::OverlayActions actions;
        actions.copyImage = [&](const QImage&)
        {
            ++copies;
            return waibusnap::ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget* parent, const QString& suggestion)
        {
            ++dialogs;
            // 验证面板期间泄漏到覆盖层的 Esc 不结束截图，拖选也不能改变选区。
            QTest::keyClick(parent, Qt::Key_Escape);
            drag(*static_cast<waibusnap::SelectionOverlay*>(parent), {150, 100}, {200, 150});
            if (!suggestion.endsWith(QStringLiteral(".png")))
                return QString();
            return dialogs == 1 ? temporary.filePath(QStringLiteral("不存在/图.png"))
                                : temporary.filePath(QStringLiteral("中文 空格"));
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        auto* button = overlay.findChild<QPushButton*>(QStringLiteral("saveButton"));
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(button && status);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(dialogs, 1);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(overlay.selection(), QRect(40, 20, 120, 100));
        QVERIFY(!overlay.isSelectionSaved());
        QVERIFY(status->text().contains(QStringLiteral("保存失败")));
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(dialogs, 2);
        QCOMPARE(copies, 0);
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.isVisible());
        QVERIFY(overlay.isSelectionSaved());
        QVERIFY(status->text().contains(QStringLiteral("已保存")));
        const QString path = temporary.filePath(QStringLiteral("中文 空格.png"));
        QImage written(path);
        QCOMPARE(written.size(), QSize(120, 100));
        QCOMPARE(written.pixelColor(10, 10), QColor(Qt::green));
        QCOMPARE(written.pixelColor(0, 0), QColor(Qt::red));
        QTRY_VERIFY(overlay.hasFocus());
        drag(overlay, {50, 35}, {60, 45});
        QVERIFY(!overlay.isSelectionSaved());
        QVERIFY(!status->isVisible());
        QVERIFY(overlay.exportToPath(path));
        QVERIFY(overlay.isSelectionSaved());
        drag(overlay, {90, 70}, {100, 80});
        QVERIFY(!overlay.isSelectionSaved());
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 2);
        QVERIFY(QFileInfo::exists(path));
    }
    void saveCancelAndSuffixCollisionPreserveFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString bare = temporary.filePath(QStringLiteral("已有 图片"));
        const QString path = bare + QStringLiteral(".png");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original"), qint64(8));
        file.close();
        int dialogs = 0;
        QString repeatedSuggestion;
        waibusnap::OverlayActions actions;
        actions.chooseSavePath = [&](QWidget* parent, const QString& suggestion)
        {
            ++dialogs;
            QTest::keyClick(parent, Qt::Key_Escape);
            if (dialogs == 1)
                return bare;
            repeatedSuggestion = suggestion;
            return QString();
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        auto* button = overlay.findChild<QPushButton*>(QStringLiteral("saveButton"));
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(dialogs, 2);
        QCOMPARE(repeatedSuggestion, path);
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.isVisible());
        QVERIFY(!overlay.isSelectionSaved());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
        QCOMPARE(QDir(temporary.path()).entryList(QDir::Files).size(), 1);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
    }
    void outsideClickClearsAndNewDragReplaces()
    {
        waibusnap::SelectionOverlay overlay(sampleFrame());
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {300, 180});
        QVERIFY(overlay.selection().isEmpty());
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        QVERIFY(toolbar && !toolbar->isVisible());
        QCOMPARE(finished.count(), 0);
        drag(overlay, {80, 60}, {80, 60});
        QVERIFY(overlay.selection().isEmpty());
        drag(overlay, {20, 10}, {80, 60});
        drag(overlay, {200, 70}, {280, 130});
        QCOMPARE(overlay.selection(), QRect(400, 140, 160, 120));
        drag(overlay, {240, 100}, {400, 250});
        QCOMPARE(overlay.selection(), QRect(480, 280, 160, 120));
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
        QCOMPARE(finished.count(), 0);
    }
    void allResizeHandles_data()
    {
        QTest::addColumn<QPoint>("press");
        QTest::addColumn<QRect>("expected");
        QTest::addColumn<int>("cursor");
        QTest::newRow("left") << QPoint(80, 100) << QRect(200, 120, 200, 160)
                              << int(Qt::SizeHorCursor);
        QTest::newRow("top") << QPoint(140, 60) << QRect(160, 140, 240, 140)
                             << int(Qt::SizeVerCursor);
        QTest::newRow("right") << QPoint(200, 100) << QRect(160, 120, 280, 160)
                               << int(Qt::SizeHorCursor);
        QTest::newRow("bottom") << QPoint(140, 140) << QRect(160, 120, 240, 180)
                                << int(Qt::SizeVerCursor);
        QTest::newRow("top-left") << QPoint(80, 60) << QRect(200, 140, 200, 140)
                                  << int(Qt::SizeFDiagCursor);
        QTest::newRow("top-right")
            << QPoint(200, 60) << QRect(160, 140, 280, 140) << int(Qt::SizeBDiagCursor);
        QTest::newRow("bottom-left")
            << QPoint(80, 140) << QRect(200, 120, 200, 180) << int(Qt::SizeBDiagCursor);
        QTest::newRow("bottom-right")
            << QPoint(200, 140) << QRect(160, 120, 280, 180) << int(Qt::SizeFDiagCursor);
    }
    void allResizeHandles()
    {
        QFETCH(QPoint, press);
        QFETCH(QRect, expected);
        QFETCH(int, cursor);
        waibusnap::SelectionOverlay overlay(sampleFrame());
        overlay.show();
        drag(overlay, {80, 60}, {200, 140});
        QTest::mouseMove(&overlay, {140, 100});
        QCOMPARE(overlay.cursor().shape(), Qt::SizeAllCursor);
        QTest::mouseMove(&overlay, {300, 180});
        QCOMPARE(overlay.cursor().shape(), Qt::CrossCursor);
        QTest::mouseMove(&overlay, press);
        QCOMPARE(int(overlay.cursor().shape()), cursor);
        drag(overlay, press, press + QPoint(20, 10));
        QCOMPARE(overlay.selection(), expected);
    }
    void savedSelectionCanCopyLatestPixels()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage copied;
        int calls = 0;
        waibusnap::OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            ++calls;
            return waibusnap::ImageOutputResult{true, {}};
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        const QString path = temporary.filePath(QStringLiteral("之前保存.png"));
        QVERIFY(overlay.exportToPath(path));
        QCOMPARE(calls, 0);
        drag(overlay, {50, 35}, {60, 45});
        auto* button = overlay.findChild<QPushButton*>(QStringLiteral("copyButton"));
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(calls, 1);
        QCOMPARE(copied.size(), QSize(120, 100));
        QCOMPARE(copied.devicePixelRatio(), qreal(1));
        QCOMPARE(copied.pixelColor(10, 10), QColor(Qt::red));
        QCOMPARE(QImage(path).pixelColor(10, 10), QColor(Qt::green));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toRect(), QRect(60, 40, 120, 100));
        QCOMPARE(finished.first().at(1).toInt(), 10);
    }
    void closeDuringSaveDialogDoesNotExport()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("不能保存.png"));
        waibusnap::OverlayActions actions;
        actions.chooseSavePath = [&](QWidget* parent, const QString&)
        {
            parent->close();
            return path;
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        auto* button = overlay.findChild<QPushButton*>(QStringLiteral("saveButton"));
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 2);
        QVERIFY(!QFileInfo::exists(path));
    }
    void reverseResizeStopsAtOnePhysicalPixel()
    {
        waibusnap::SelectionOverlay overlay(sampleFrame());
        overlay.show();
        drag(overlay, {80, 60}, {200, 140});
        drag(overlay, {80, 60}, {300, 190});
        QCOMPARE(overlay.selection(), QRect(399, 279, 1, 1));
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
