#include "../fixtures/recording_control_fakes.h"
#include "app/application_controller.h"
#include "app/hotkey_rules.h"
#include "interfaces/tray_icon.h"
#include "ui/recording_border_window.h"
#include "ui/recording_countdown_window.h"
#include "ui/recording_toast.h"
#include "ui/settings_dialog.h"
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
using namespace waibusnap;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static RecordingController& recording(ApplicationController& c) { return *c.recording_; }
    static void start(ApplicationController& c) { c.startRecordingSelection(); }
    static void shutter(ApplicationController& c) { c.quickGunnerShutter(); }
};
}
class RecordingUiTest : public QObject
{
    Q_OBJECT
    static QAction* action(ApplicationController& c, const char* name)
    {
        return c.menu()->findChild<QAction*>(QString::fromLatin1(name));
    }
    static CaptureFrame frame()
    {
        CaptureFrame f;
        f.display = {5, QRect(-100, 10, 800, 600), 2, 1, QRect(-100, 10, 800, 570)};
        f.pixels = QImage(1600, 1200, QImage::Format_RGB32);
        f.pixels.fill(Qt::blue);
        return f;
    }
  private slots:
    void pickerModesMinimumAndKeys()
    {
        RecordingTargetPicker picker(
            frame(), {{123, QRect(40, 90, 240, 180), QStringLiteral("测试应用 — 窗口"), true}});
        int selected = 0, cancelled = 0;
        RecordingSelection result;
        picker.open(
            [&](RecordingSelection s)
            {
                ++selected;
                result = s;
            },
            [&] { ++cancelled; });
        for (const auto* name :
             {"recordingPickerAreaButton", "recordingPickerWindowButton",
              "recordingPickerDisplayButton", "recordingPickerOutputVideo", "recordingPickerStart",
              "recordingPickerCancel", "recordingPickerOutputGif"})
            QVERIFY(picker.findChild<QPushButton*>(QString::fromLatin1(name)));
        QTest::keyClick(&picker, Qt::Key_Return);
        QCOMPARE(selected, 0);
        QTest::mousePress(&picker, Qt::LeftButton, {}, QPoint(100, 100));
        QTest::mouseRelease(&picker, Qt::LeftButton, {}, QPoint(104, 104));
        QVERIFY(
            !picker.findChild<QPushButton*>(QStringLiteral("recordingPickerStart"))->isEnabled());
        QTest::mousePress(&picker, Qt::LeftButton, {}, QPoint(100, 100));
        QTest::mouseRelease(&picker, Qt::LeftButton, {}, QPoint(260, 220));
        QVERIFY(
            picker.findChild<QPushButton*>(QStringLiteral("recordingPickerStart"))->isEnabled());
        QTest::keyClick(&picker, Qt::Key_2);
        QTest::mouseMove(&picker, QPoint(200, 160));
        QTest::mouseClick(&picker, Qt::LeftButton, {}, QPoint(200, 160));
        QCOMPARE(picker.selection().windowId, quint64(123));
        auto* gif = picker.findChild<QPushButton*>(QStringLiteral("recordingPickerOutputGif"));
        auto* video = picker.findChild<QPushButton*>(QStringLiteral("recordingPickerOutputVideo"));
        QVERIFY(video->isChecked() && !gif->isChecked() && gif->isVisible());
        gif->click();
        QVERIFY(gif->isChecked() && !video->isChecked());
        QTest::keyClick(&picker, Qt::Key_V);
        QCOMPARE(picker.selection().output, RecordingOutput::Video);
        QTest::keyClick(&picker, Qt::Key_G);
        QTest::keyClick(&picker, Qt::Key_3);
        QCOMPARE(picker.selection().pixelRect, QRect(0, 0, 1600, 1200));
        QTest::keyClick(&picker, Qt::Key_Return);
        QCOMPARE(selected, 1);
        QCOMPARE(cancelled, 0);
        QCOMPARE(result.kind, RecordingTargetKind::Display);
        QCOMPARE(result.output, RecordingOutput::Gif);
        QVERIFY(!picker.isVisible());
        RecordingTargetPicker escape(frame(), {});
        escape.open({}, [&] { ++cancelled; });
        QTest::keyClick(&escape, Qt::Key_Escape);
        QCOMPARE(cancelled, 1);
    }
    void countdownAndBorder()
    {
        RecordingCountdownWindow countdown;
        QSignalSpy cancelled(&countdown, &RecordingCountdownWindow::cancelled);
        countdown.present(QRect(-100, 0, 800, 600), 0);
        QVERIFY(!countdown.isVisible());
        for (int seconds : {3, 5})
        {
            countdown.present(QRect(-100, 0, 800, 600), seconds);
            QVERIFY(countdown.isVisible());
            QCOMPARE(countdown.geometry().center(), QPoint(299, 299));
            QTest::keyClick(&countdown, Qt::Key_Escape);
            QVERIFY(!countdown.isVisible());
        }
        QCOMPARE(cancelled.size(), 2);
        RecordingBorderWindow border;
        border.present(QRect(-30, 70, 300, 200), 12000);
        QVERIFY(border.isVisible());
        QVERIFY(border.windowFlags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(border.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QCOMPARE(border.geometry(), QRect(-30, 70, 300, 200));
        border.present(QRect(-60, 90, 310, 220), 12000);
        QCOMPARE(border.geometry(), QRect(-60, 90, 310, 220));
        QVERIFY(!border.grab().isNull());
        border.hide();
        QVERIFY(!border.isVisible());
    }
    void toastVariantsAndFolder()
    {
        for (bool success : {false, true})
        {
            RecordingEvent event;
            event.path = QStringLiteral("/example/录制.mp4");
            event.pixelSize = QSize(1920, 1080);
            event.recordedMs = 12000;
            event.bytes = 123456;
            event.message = QStringLiteral("测试原因");
            QString revealed;
            RecordingToast toast(event, success, [&](const QString& p) { revealed = p; });
            toast.present();
            QVERIFY(toast.isVisible());
            QVERIFY(toast.testAttribute(Qt::WA_ShowWithoutActivating));
            auto* folder =
                toast.findChild<QPushButton*>(QStringLiteral("recordingToastOpenFolder"));
            QVERIFY(folder);
            folder->click();
            QCOMPARE(revealed, event.path);
            bool fullPath = false;
            for (auto* label : toast.findChildren<QLabel*>())
                fullPath |= label->toolTip() == event.path;
            QVERIFY(fullPath);
            QVERIFY(!toast.grab().isNull());
        }
    }
    void gifToastTitles()
    {
        for (bool automatic : {false, true})
        {
            RecordingEvent e;
            e.path = QStringLiteral("/example/test.gif");
            e.autoStopped = automatic;
            RecordingToast toast(e, true, {});
            bool found = false;
            for (auto* label : toast.findChildren<QLabel*>())
                found |= label->text() == (automatic ? QStringLiteral("GIF 已自动停止")
                                                     : QStringLiteral("GIF 已保存"));
            QVERIFY(found);
        }
    }
    void iconStatesHaveBothScales()
    {
        const auto idle = createTrayIcon();
        for (const auto state :
             {TrayIconState::Idle, TrayIconState::Recording, TrayIconState::Paused})
        {
            const auto icon = createTrayIcon(state);
            QVERIFY(!icon.isNull());
            for (int scale : {1, 2})
            {
                const auto sizes = icon.availableSizes();
                QVERIFY(!sizes.isEmpty());
                const int side = sizes.first().width();
                const auto pixmap = icon.pixmap(QSize(side, side), scale);
                QVERIFY(!pixmap.isNull());
                if (state != TrayIconState::Idle)
                {
                    QVERIFY(!icon.isMask());
                    QVERIFY(pixmap.toImage() != idle.pixmap(QSize(side, side), scale).toImage());
                }
            }
        }
    }
    void generalEditorsAndSevenWayConflict()
    {
        SettingsDialog dialog(QKeySequence(Qt::Key_F1),
                              [](const QKeySequence&) { return QString(); });
        auto* start =
            dialog.findChild<QKeySequenceEdit*>(QStringLiteral("recordingStartStopHotkey"));
        auto* pause =
            dialog.findChild<QKeySequenceEdit*>(QStringLiteral("recordingPauseResumeHotkey"));
        QVERIFY(start && pause);
        QCOMPARE(start->keySequence(), QKeySequence(Qt::Key_F3));
        QCOMPARE(pause->keySequence(), QKeySequence(Qt::Key_F4));
        std::array<QKeySequence, 7> sequences;
        for (size_t i = 0; i < sequences.size(); ++i)
            sequences[i] = defaultHotkey(hotkeyActions[i]);
        QVERIFY(hotkeyConflictError(sequences).isEmpty());
        for (size_t i = 0; i < sequences.size(); ++i)
        {
            auto duplicate = sequences;
            duplicate[6] = sequences[i == 6 ? 5 : i];
            QVERIFY(!hotkeyConflictError(duplicate).isEmpty());
        }
    }
    void quitDialogCancelAndFailedSave()
    {
        RecordingHarness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.directory.filePath(QStringLiteral("quit-settings.ini"));
        ApplicationController controller(*qApp, options, {}, {}, {}, {}, {}, {}, h.dependencies());
        auto& recording = ApplicationControllerTestAccess::recording(controller);
        h.start(recording);
        const auto choose = [](const QString& name)
        {
            QTimer::singleShot(
                0, qApp,
                [name]
                {
                    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    QVERIFY(box);
                    for (const auto* buttonName :
                         {"recordingQuitSave", "recordingQuitDiscard", "recordingQuitCancel"})
                        QVERIFY(box->findChild<QPushButton*>(QString::fromLatin1(buttonName)));
                    box->findChild<QPushButton*>(name)->click();
                });
        };
        choose(QStringLiteral("recordingQuitCancel"));
        controller.quit();
        QCOMPARE(recording.state(), RecordingController::State::Recording);
        choose(QStringLiteral("recordingQuitSave"));
        controller.quit();
        QCOMPARE(recording.state(), RecordingController::State::Finalizing);
        h.event(RecordingEventType::Failed);
        QCOMPARE(recording.state(), RecordingController::State::Idle);
        QVERIFY(action(controller, "settingsAction")->isEnabled());
        QVERIFY(!controller.active());
    }
    void trayMutualExclusionAndRestoration()
    {
        RecordingHarness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.directory.filePath(QStringLiteral("settings.ini"));
        ApplicationController controller(*qApp, options, {}, {}, {}, {}, {}, {}, h.dependencies());
        auto* menu = controller.menu()->findChild<QMenu*>(QStringLiteral("recordingMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 6);
        for (const auto* name :
             {"recordingStatusAction", "recordingStartAction", "recordingPauseAction",
              "recordingStopAction", "recordingDiscardAction", "recordingCancelAction"})
            QVERIFY(action(controller, name));
        QVERIFY(!action(controller, "recordingPauseAction")->isVisible());
        auto& recording = ApplicationControllerTestAccess::recording(controller);
        recording.startSelection(h.preferences());
        h.drain();
        h.select();
        h.event(RecordingEventType::Started);
        for (const auto* name : {"screenshotAction", "scrollCaptureAction", "quickGunnerModeAction",
                                 "quickGunnerFrameAction", "quickGunnerDirectoryAction",
                                 "settingsAction", "clipboardStickerAction"})
            QVERIFY2(!action(controller, name)->isEnabled(), name);
        QVERIFY(action(controller, "quickGunnerOpenAction")->isEnabled());
        controller.trigger(QStringLiteral("test"));
        ApplicationControllerTestAccess::shutter(controller);
        QVERIFY(!controller.active());
        QVERIFY(action(controller, "recordingPauseAction")->isEnabled());
        action(controller, "recordingPauseAction")->trigger();
        QCOMPARE(recording.state(), RecordingController::State::Paused);
        QCOMPARE(action(controller, "recordingPauseAction")->text(), QStringLiteral("继续"));
        h.event(RecordingEventType::Progress, 12000);
        QVERIFY(action(controller, "recordingStatusAction")
                    ->toolTip()
                    .contains(QStringLiteral("00:12")));
        action(controller, "recordingStopAction")->trigger();
        QCOMPARE(recording.state(), RecordingController::State::Finalizing);
        QVERIFY(!action(controller, "settingsAction")->isEnabled());
        h.event(RecordingEventType::Finished);
        QVERIFY(action(controller, "screenshotAction")->isEnabled());
        QVERIFY(action(controller, "settingsAction")->isEnabled());
        QVERIFY(action(controller, "clipboardStickerAction")->isEnabled());
    }
};
QTEST_MAIN(RecordingUiTest)
#include "recording_ui_test.moc"
