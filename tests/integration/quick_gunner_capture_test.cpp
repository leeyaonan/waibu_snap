#include "app/application_controller.h"
#include "app/quick_gunner.h"
#include "app/quick_gunner_save_queue.h"
#include "ui/quick_gunner_flash_window.h"
#include "ui/quick_gunner_region_picker.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static void guard(ApplicationController& controller, int which, bool enabled)
    {
        if (which == 0)
            controller.active_ = enabled;
        if (which == 1)
            controller.settingsOpen_ = enabled;
        if (which == 2)
            controller.quitting_ = enabled;
        if (which == 3)
            controller.resolvingQuit_ = enabled;
        controller.refreshQuickGunnerActions();
    }
    static void installForFramingTest(ApplicationController& controller,
                                      std::unique_ptr<QuickGunnerController> quickGunner,
                                      std::unique_ptr<DisplayTopology> displays,
                                      std::unique_ptr<CaptureProvider> capture)
    {
        controller.quickGunner_ = std::move(quickGunner);
        controller.displays_ = std::move(displays);
        controller.capture_ = std::move(capture);
        controller.connectQuickGunner();
    }
    static QuickGunnerController& quickGunner(ApplicationController& controller)
    {
        return *controller.quickGunner_;
    }
    static void initializeHotkeys(ApplicationController& c)
    {
        c.quickGunnerHotkey_->load();
        c.hotkeySettings_.initialize();
        for (auto& key : c.historyHotkeys_)
            key->initialize();
    }
    static bool registered(ApplicationController& c) { return c.quickGunnerHotkey_->registered(); }
    static bool quitting(ApplicationController& c) { return c.quitting_; }
    static void stopScreenshot(ApplicationController& c)
    {
        // 仅结束 fake 会话，避免受控运行的自动退出计时器污染后续模态用例。
        c.options_.testMode = false;
        c.finish({}, 2);
        c.options_.testMode = true;
    }
    static bool chooseDirectory(ApplicationController& c) { return c.chooseQuickGunnerDirectory(); }
    static QList<QWidget*> pausedUi(ApplicationController& c, CaptureFrame frame)
    {
        // 此处只注入暂停界面，不启动受控截图计时器，避免退出测试影响下一行模态。
        c.options_.testMode = false;
        c.active_ = true;
        c.scrollFlowActive_ = true;
        c.overlay_ = new SelectionOverlay(frame);
        c.scrollBorder_ = new ScrollBorderWindow({20, 20, 100, 80});
        c.scrollBar_ = new ScrollControlBar({20, 20, 100, 80}, {0, 0, 800, 600});
        QList<QWidget*> windows{c.overlay_, c.scrollBorder_, c.scrollBar_};
        for (auto* window : windows)
            window->show();
        return windows;
    }
    static quint64 token(ApplicationController& controller) { return controller.token_; }
    static QImage watermark(ApplicationController& controller, const QImage& image)
    {
        return controller.quickGunnerWatermark(image);
    }
    static WatermarkService& watermarks(ApplicationController& controller)
    {
        return controller.watermarks_;
    }
    static HistoryWindow* browser(ApplicationController& c) { return c.historyWindow_; }
    static HistoryManager& history(ApplicationController& c) { return c.history_; }
    static QuickGunnerResourceSource& source(ApplicationController& c)
    {
        return *c.quickGunnerSource_;
    }
    static bool warned(ApplicationController& controller)
    {
        return controller.quickGunnerWatermarkWarned_;
    }
};
}
using namespace waibusnap;
namespace
{
class Displays final : public DisplayTopology
{
  public:
    DisplayTarget target{42, {-400, 20, 400, 300}, 2, 123, {}};
    bool matches = true;
    DisplayResult cursorDisplay() const override { return {target, {}}; }
    bool stillMatches(const DisplayTarget&) const override { return matches; }
};
class Capture final : public CaptureProvider
{
  public:
    bool permission = true;
    bool hasPermission() const override { return permission; }
    bool requestPermission() override { return permission; }
    QVector<std::function<void(CaptureResult)>> callbacks;
    void capture(const DisplayTarget&, std::function<void(CaptureResult)> callback) override
    {
        callbacks.append(std::move(callback));
    }
};
class Hotkeys final : public GlobalHotkey
{
  public:
    bool rejectShutter = false;
    std::array<std::function<void()>, hotkeyActions.size()> handlers;
    HotkeyRegistration registerHotkey(HotkeyAction action, const QKeySequence&,
                                      std::function<void()> handler) override
    {
        if (rejectShutter && action == HotkeyAction::QuickGunnerShutter)
            return {false, QStringLiteral("测试系统占用")};
        handlers[size_t(action)] = std::move(handler);
        return {true, {}};
    }
    void unregister(HotkeyAction action) override { handlers[size_t(action)] = {}; }
    void fire(HotkeyAction action)
    {
        const auto handler = handlers[size_t(action)];
        if (handler)
            handler();
    }
};
struct Calls
{
    QVector<RegionCaptureRequest> requests;
    QVector<std::function<void(QImage, QString)>> callbacks;
};
class Once final : public RegionCaptureOnce
{
  public:
    explicit Once(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}
    void capture(const RegionCaptureRequest& request,
                 std::function<void(QImage, QString)> callback) override
    {
        calls_->requests << request;
        calls_->callbacks << std::move(callback);
    }

  private:
    std::shared_ptr<Calls> calls_;
};
struct Harness
{
    QTemporaryDir temp;
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    Displays displays;
    Capture capture;
    std::shared_ptr<Calls> calls = std::make_shared<Calls>();
    QuickGunnerSaveQueue* saves = nullptr;
    int watermarks = 0;
    bool watermarkOnGui = false;
    std::unique_ptr<QuickGunnerController> controller;
    Harness(bool directory = true, bool region = true,
            std::function<QImage(const QImage&)> watermark = {})
    {
        QuickGunnerPreferences preferences;
        if (directory)
            preferences.directory = temp.filePath(QStringLiteral("shots"));
        if (region)
            preferences.region = QuickGunnerRegion{displays.target, {20, 40, 100, 80}};
        settings.saveQuickGunnerPreferences(preferences);
        if (!preferences.directory.isEmpty())
            QDir().mkdir(preferences.directory);
        QuickGunnerDependencies dependencies;
        dependencies.captureFactory = [calls = calls] { return std::make_unique<Once>(calls); };
        dependencies.watermark = [this](const QImage& image)
        {
            ++watermarks;
            watermarkOnGui = QThread::currentThread() == qApp->thread();
            QImage result = image;
            result.setPixelColor(0, 0, Qt::blue);
            return result;
        };
        QuickGunnerSaveActions writes;
        // 固定毫秒时间戳，连拍集成同时核对真实文件冲突后缀，不能靠时钟避开冲突。
        writes.timestamp = [] { return QDateTime::fromMSecsSinceEpoch(1700000000123LL); };
        if (watermark)
            dependencies.watermark = std::move(watermark);
        auto queue = std::make_unique<QuickGunnerSaveQueue>(settings, writes);
        saves = queue.get();
        dependencies.saveQueue = std::move(queue);
        controller = std::make_unique<QuickGunnerController>(
            settings, displays, capture, RegionPickerFactory{}, std::move(dependencies));
    }
    void complete(int index, QString error = {})
    {
        QImage image(100, 80, QImage::Format_RGB32);
        image.fill(Qt::red);
        calls->callbacks.at(index)(error.isEmpty() ? image : QImage(), error);
    }
};
}
class QuickGunnerCaptureTest : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs); }
    void preflight()
    {
        Harness noDirectory(false);
        QVERIFY(noDirectory.controller->shutter().contains(QStringLiteral("目录")));
        Harness noRegion(true, false);
        QVERIFY(noRegion.controller->shutter().contains(QStringLiteral("机位")));
        Harness h;
        h.displays.matches = false;
        QVERIFY(h.controller->shutter().contains(QStringLiteral("布局")));
        h.displays.matches = true;
        h.capture.permission = false;
        QVERIFY(!h.controller->shutter().isEmpty());
        h.capture.permission = true;
        QVERIFY(h.controller->beginFraming().isEmpty());
        QVERIFY(!h.controller->shutter().isEmpty());
        QVERIFY(h.calls->requests.isEmpty());
    }
    void serialSuccessFailureAndBounds()
    {
        Harness h;
        QSignalSpy dropped(h.controller.get(), &QuickGunnerController::shotsDropped);
        QSignalSpy captured(h.controller.get(), &QuickGunnerController::shotCaptured);
        QSignalSpy failed(h.controller.get(), &QuickGunnerController::shotFailed);
        QSignalSpy saved(h.saves, &QuickGunnerSaveQueue::saved);
        for (int i = 0; i < 11; ++i)
            QVERIFY(h.controller->shutter().isEmpty());
        QVERIFY(h.controller->capturing());
        QCOMPARE(h.controller->pendingCaptureCount(), 8);
        QCOMPARE(h.calls->requests.size(), 1);
        QCOMPARE(dropped.size(), 2);
        const auto request = h.calls->requests.first();
        QCOMPARE(request.pixelRect, QRect(20, 40, 100, 80));
        QCOMPARE(request.display.id, quint32(42));
        auto* flash = h.controller->flashWindow();
        QVERIFY(flash);
        const auto id = regionCaptureWindowId(flash->windowHandle());
        QCOMPARE(request.excludedWindowIds, id ? QVector<quint64>{id} : QVector<quint64>{});
        h.complete(0);
        QTRY_COMPARE(h.calls->requests.size(), 2);
        QCOMPARE(captured.size(), 1);
        QCOMPARE(captured[0][0].toRect(), QRect(-390, 40, 50, 40));
        QCOMPARE(h.watermarks, 1);
        QCOMPARE(h.controller->state(), QuickGunnerController::State::Idle);
        QVERIFY(!flash->isVisible());
        h.complete(1, QStringLiteral("测试失败"));
        QTRY_COMPARE(failed.size(), 1);
        for (int i = 2; i < 9; ++i)
        {
            QTRY_COMPARE(h.calls->requests.size(), i + 1);
            h.complete(i);
        }
        QTRY_VERIFY(!h.controller->capturing());
        QCOMPARE(h.controller->pendingCaptureCount(), 0);
        QTRY_COMPARE(saved.size(), 8);
        QCOMPARE(QImage(saved[0][0].toString()).pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(h.controller->pendingSaveCount(), 0);
        QCOMPARE(h.watermarks, 8);
        QVERIFY(h.watermarkOnGui);
    }
    void deactivateDropsWaitingAndSavesInFlight()
    {
        Harness h;
        QVERIFY(h.controller->activate({}).isEmpty());
        QSignalSpy dropped(h.controller.get(), &QuickGunnerController::shotsDropped);
        QSignalSpy saved(h.saves, &QuickGunnerSaveQueue::saved);
        for (int i = 0; i < 4; ++i)
            QVERIFY(h.controller->shutter().isEmpty());
        QCOMPARE(h.controller->pendingCaptureCount(), 3);
        h.controller->deactivate();
        QCOMPARE(h.controller->pendingCaptureCount(), 0);
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(dropped[0][0].toInt(), 3);
        QVERIFY(h.controller->capturing());
        h.complete(0);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(!h.controller->capturing());
        QCOMPARE(h.calls->requests.size(), 1);
        QVERIFY(!h.controller->flashWindow()->isVisible());
        h.controller->deactivate();
        QCOMPARE(dropped.size(), 1);
        QVERIFY(h.controller->activate({}).isEmpty());
        QCoreApplication::processEvents();
        QCOMPARE(h.calls->requests.size(), 1);
    }
    void ordinaryScreenshotCannotRaceInFlightCapture()
    {
        Harness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("application.ini"));
        options.metricsFile = h.temp.filePath(QStringLiteral("metrics.jsonl"));
        auto keys = std::make_unique<Hotkeys>();
        auto* raw = keys.get();
        ApplicationController c(*qApp, options, {}, {}, {}, {}, {}, std::move(keys));
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        // 直接激活 fake 控制器，Windows 也能验证共享层互斥，不调用原生采集。
        QVERIFY(gunner.activate({}).isEmpty());
        raw->fire(HotkeyAction::QuickGunnerShutter);
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QCOMPARE(h.calls->requests.size(), 1);
        for (int i = 0; i < 2; ++i)
        {
            const auto token = ApplicationControllerTestAccess::token(c);
            raw->fire(HotkeyAction::Screenshot);
            QVERIFY(!c.active());
            QCOMPARE(ApplicationControllerTestAccess::token(c), token);
            h.complete(i);
            if (i == 0)
                QTRY_COMPARE(h.calls->requests.size(), 2);
        }
        QTRY_VERIFY(!gunner.capturing());
        raw->fire(HotkeyAction::Screenshot);
        QVERIFY(c.active());
        QVERIFY(!gunner.flashWindow()->isVisible());
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QCOMPARE(h.calls->requests.size(), 2);
        ApplicationControllerTestAccess::stopScreenshot(c);
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QCOMPARE(h.calls->requests.size(), 3);
        h.complete(2);
        QTRY_VERIFY(!gunner.capturing());
    }
    void framingDisablesReentrantTrayActionsAndKeepsEscape()
    {
        Harness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("application.ini"));
        ApplicationController c(*qApp, options, {}, {}, {}, {}, {}, std::make_unique<Hotkeys>());
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        QVERIFY(gunner.beginFraming().isEmpty());
        for (const auto* name :
             {"quickGunnerModeAction", "quickGunnerFrameAction", "quickGunnerDirectoryAction"})
        {
            auto* action = c.menu()->findChild<QAction*>(QLatin1String(name));
            QVERIFY(action);
            QVERIFY(!action->isEnabled());
        }
        QVERIFY(!ApplicationControllerTestAccess::chooseDirectory(c));
        QCOMPARE(h.capture.callbacks.size(), 1);
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::green);
        h.capture.callbacks.last()({{h.displays.target, image, 0}, CaptureError::None, {}});
        auto* picker = qApp->findChild<QuickGunnerRegionPicker*>();
        for (auto* window : QApplication::topLevelWidgets())
            if (auto* candidate = qobject_cast<QuickGunnerRegionPicker*>(window))
                picker = candidate;
        QVERIFY(picker);
        QTest::keyClick(picker, Qt::Key_Escape);
        QCOMPARE(gunner.state(), QuickGunnerController::State::Idle);
        auto* directory =
            c.menu()->findChild<QAction*>(QStringLiteral("quickGunnerDirectoryAction"));
        QVERIFY(directory->isEnabled());
    }
    void invalidDirectoryStillCapturesAndRecovers()
    {
        Harness h;
        const auto directory = h.controller->preferences().directory;
        QVERIFY(QDir(directory).removeRecursively());
        QSignalSpy saved(h.saves, &QuickGunnerSaveQueue::saved);
        QSignalSpy failed(h.controller.get(), &QuickGunnerController::shotFailed);
        QVERIFY(h.controller->shutter().isEmpty());
        h.complete(0);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(h.controller->pendingSaveCount(), 1);
        QVERIFY(QDir().mkdir(directory));
        QVERIFY(h.controller->setDirectory(directory, {}).isEmpty());
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(h.controller->pendingSaveCount(), 0);
    }
    void lateAndDuplicateCompletion()
    {
        Harness h;
        QSignalSpy captured(h.controller.get(), &QuickGunnerController::shotCaptured);
        QVERIFY(h.controller->shutter().isEmpty());
        h.complete(0);
        h.complete(0);
        QTRY_COMPARE(captured.size(), 1);
        QVERIFY(h.controller->shutter().isEmpty());
        h.controller.reset();
        h.complete(1);
        QCoreApplication::processEvents();
    }
    void queuedPreflightAndInvalidResult()
    {
        Harness h;
        QVERIFY(h.controller->activate({}).isEmpty());
        QSignalSpy failed(h.controller.get(), &QuickGunnerController::shotFailed);
        QSignalSpy captured(h.controller.get(), &QuickGunnerController::shotCaptured);
        QVERIFY(h.controller->shutter().isEmpty());
        QVERIFY(h.controller->shutter().isEmpty());
        h.displays.matches = false;
        h.complete(0);
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(h.calls->requests.size(), 1);
        QVERIFY(!h.controller->capturing());
        QVERIFY(h.controller->active());
        h.displays.matches = true;
        QVERIFY(h.controller->shutter().isEmpty());
        h.calls->callbacks.last()(QImage(10, 10, QImage::Format_RGB32), {});
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(captured.size(), 0);
        QCOMPARE(h.watermarks, 0);
    }
    void nativeInvalidRequestAndDestruction()
    {
        auto once = createRegionCaptureOnce();
        QVERIFY(once);
        int calls = 0;
        QString error;
        once->capture({},
                      [&](QImage image, QString explanation)
                      {
                          QVERIFY(image.isNull());
                          QCOMPARE(QThread::currentThread(), qApp->thread());
                          ++calls;
                          error = explanation;
                      });
        QCOMPARE(calls, 0);
        QTRY_COMPARE(calls, 1);
        QVERIFY(!error.isEmpty());
        if (!regionCaptureSupported())
            QCOMPARE(error, QStringLiteral("Windows 原生区域采集尚未实现"));
        once->capture({}, [&](QImage, QString) { ++calls; });
        once.reset();
        QTest::qWait(50);
        QCOMPARE(calls, 1);
    }
    void globalWatermarkAndBrokenAssetFallback()
    {
        QTemporaryDir temp;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        ApplicationController controller(*qobject_cast<QApplication*>(qApp), options);
        QImage image(320, 200, QImage::Format_RGB32);
        image.fill(Qt::black);
        QCOMPARE(ApplicationControllerTestAccess::watermark(controller, image), image);
        auto& service = ApplicationControllerTestAccess::watermarks(controller);
        WatermarkPreferences preferences;
        WatermarkAsset asset;
        asset.id = QStringLiteral("test-text");
        asset.name = QStringLiteral("署名");
        asset.text = QStringLiteral("B2");
        WatermarkPreset preset;
        preset.id = QStringLiteral("test-preset");
        preset.name = QStringLiteral("居中");
        preset.assetId = asset.id;
        preset.layout = WatermarkLayout::Center;
        preset.opacity = 100;
        preferences.assets = {asset};
        preferences.presets = {preset};
        preferences.autoEnabled = true;
        preferences.defaultPresetId = preset.id;
        QVERIFY(service.savePreferences(preferences).isEmpty());
        QVERIFY(ApplicationControllerTestAccess::watermark(controller, image) != image);
        QVERIFY(service.setAutoEnabled(false).isEmpty());
        QCOMPARE(ApplicationControllerTestAccess::watermark(controller, image), image);
        // 直接写入已有设置中的失效资产，模拟磁盘文件被外部删除；无需触碰用户资产目录。
        preferences.assets[0].kind = WatermarkKind::Image;
        preferences.assets[0].imageFileName = QStringLiteral("b2-missing-asset.png");
        preferences.autoEnabled = true;
        QVERIFY(AppSettings(options.settingsFile).saveWatermarkPreferences(preferences).isEmpty());
        QVERIFY(service.initialize());
        QCOMPARE(ApplicationControllerTestAccess::watermark(controller, image), image);
        QVERIFY(ApplicationControllerTestAccess::warned(controller));
        QCOMPARE(ApplicationControllerTestAccess::watermark(controller, image), image);
    }
    void framingGuardsOrdinaryScreenshot()
    {
        Harness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("application.ini"));
        ApplicationController controller(*qobject_cast<QApplication*>(qApp), options);
        ApplicationControllerTestAccess::installForFramingTest(controller, std::move(h.controller),
                                                               std::make_unique<Displays>(),
                                                               std::make_unique<Capture>());
        auto& quickGunner = ApplicationControllerTestAccess::quickGunner(controller);
        QVERIFY(quickGunner.beginFraming().isEmpty());
        QCOMPARE(quickGunner.state(), QuickGunnerController::State::Framing);
        const auto token = ApplicationControllerTestAccess::token(controller);
        controller.trigger(QStringLiteral("hotkey"));
        QVERIFY(!controller.active());
        QCOMPARE(ApplicationControllerTestAccess::token(controller), token);
        QCOMPARE(quickGunner.state(), QuickGunnerController::State::Framing);
        // 取消框选后，普通截图恢复，使用 fake 采集避免测试读取真实屏幕。
        quickGunner.deactivate();
        controller.trigger(QStringLiteral("hotkey"));
        QVERIFY(controller.active());
        QVERIFY(ApplicationControllerTestAccess::token(controller) > token);
    }
    void modeLifecycleAndShutterGuards()
    {
        Harness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("settings.ini"));
        options.metricsFile = h.temp.filePath(QStringLiteral("metrics.jsonl"));
        auto keys = std::make_unique<Hotkeys>();
        auto* raw = keys.get();
        ApplicationController c(*qobject_cast<QApplication*>(qApp), options, {}, {}, {}, {}, {},
                                std::move(keys));
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        auto* mode = c.menu()->findChild<QAction*>(QStringLiteral("quickGunnerModeAction"));
        QVERIFY(mode);
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QVERIFY(h.calls->requests.isEmpty());
        if (!regionCaptureSupported())
        {
            QVERIFY(!mode->isEnabled());
            return;
        }
        mode->trigger();
        QVERIFY(mode->isChecked());
        QVERIFY(gunner.active());
        QVERIFY(ApplicationControllerTestAccess::registered(c));
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QCOMPARE(h.calls->requests.size(), 1);
        QVERIFY(!c.active());
        h.complete(0);
        QTRY_VERIFY(!gunner.capturing());
        QTRY_COMPARE(gunner.pendingSaveCount(), 0);
        for (int which = 0; which < 4; ++which)
        {
            ApplicationControllerTestAccess::guard(c, which, true);
            raw->fire(HotkeyAction::QuickGunnerShutter);
            QCOMPARE(h.calls->requests.size(), 1);
            ApplicationControllerTestAccess::guard(c, which, false);
        }
        raw->fire(HotkeyAction::Screenshot);
        QVERIFY(c.active());
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QCOMPARE(h.calls->requests.size(), 1);
        ApplicationControllerTestAccess::guard(c, 0, false);
        mode->trigger();
        QVERIFY(!mode->isChecked());
        QCOMPARE(gunner.state(), QuickGunnerController::State::Idle);
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
        raw->fire(HotkeyAction::QuickGunnerShutter);
        QCOMPARE(h.calls->requests.size(), 1);
        raw->rejectShutter = true;
        QTest::ignoreMessage(QtWarningMsg, "快枪手：快枪手快门无法注册：测试系统占用");
        mode->trigger();
        QVERIFY(!mode->isChecked());
        QCOMPARE(gunner.state(), QuickGunnerController::State::Idle);
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
        raw->rejectShutter = false;
        mode->trigger();
        QVERIFY(gunner.active());
        auto* frame = c.menu()->findChild<QAction*>(QStringLiteral("quickGunnerFrameAction"));
        frame->trigger();
        QCOMPARE(gunner.state(), QuickGunnerController::State::Framing);
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
        // 当前 fake 冻结帧尚未完成，模拟采集失败后仍恢复 Active。
        h.capture.callbacks.last()({{}, CaptureError::Unavailable, QStringLiteral("重框采集失败")});
        QVERIFY(gunner.active());
        QVERIFY(mode->isChecked());
        QVERIFY(ApplicationControllerTestAccess::registered(c));
        // 重启模拟：偏好保留，模式和 F2 注册均不保留。
        ApplicationController restarted(*qobject_cast<QApplication*>(qApp), options, {}, {}, {}, {},
                                        {}, std::make_unique<Hotkeys>());
        ApplicationControllerTestAccess::initializeHotkeys(restarted);
        QVERIFY(!ApplicationControllerTestAccess::registered(restarted));
        QCOMPARE(ApplicationControllerTestAccess::quickGunner(restarted).preferences().directory,
                 gunner.preferences().directory);
        QCOMPARE(
            ApplicationControllerTestAccess::quickGunner(restarted).preferences().region->toJson(),
            gunner.preferences().region->toJson());
        QVERIFY(!restarted.menu()
                     ->findChild<QAction*>(QStringLiteral("quickGunnerModeAction"))
                     ->isChecked());
    }
    void burstFilesReachBrowserAndActions_data()
    {
        QTest::addColumn<bool>("historyEnabled");
        QTest::addColumn<bool>("jpeg");
        QTest::newRow("history-off-png") << false << false;
        QTest::newRow("history-on-png") << true << false;
        QTest::newRow("history-off-jpeg") << false << true;
        QTest::newRow("history-on-jpeg") << true << true;
    }
    void burstFilesReachBrowserAndActions()
    {
        QFETCH(bool, historyEnabled);
        QFETCH(bool, jpeg);
        ApplicationController* application = nullptr;
        Harness h(true, true, [&](const QImage& image)
                  { return ApplicationControllerTestAccess::watermark(*application, image); });
        QVERIFY(h.settings.saveSavePreferences({{}, jpeg ? ImageFormat::Jpeg : ImageFormat::Png})
                    .isEmpty());
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("settings.ini"));
        options.historyRoot = h.temp.filePath(QStringLiteral("history"));
        options.historyEnabledOverride = historyEnabled;
        QImage copied, pinned;
        QString revealed;
        QStringList trashed;
        HistoryWindowActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.pinImage = [&](const QImage& image)
        {
            pinned = image;
            return ImageOutputResult{true, {}};
        };
        actions.reveal = [&](const QString& path)
        {
            revealed = path;
            return HistoryStatus{true, {}};
        };
        actions.trash = [&](const QString& path)
        {
            trashed << path;
            return HistoryStatus{QFile::remove(path), {}};
        };
        auto keys = std::make_unique<Hotkeys>();
        auto* raw = keys.get();
        ApplicationController c(*qApp, options, {}, {}, {}, actions, {}, std::move(keys));
        application = &c;
        WatermarkPreferences watermarks;
        WatermarkAsset asset;
        asset.id = QStringLiteral("signature");
        asset.name = QStringLiteral("署名");
        asset.text = QStringLiteral("B6");
        WatermarkPreset center;
        center.id = QStringLiteral("center");
        center.name = QStringLiteral("居中");
        center.assetId = asset.id;
        center.layout = WatermarkLayout::Center;
        center.opacity = 100;
        auto corner = center;
        corner.id = QStringLiteral("corner");
        corner.name = QStringLiteral("左下");
        corner.layout = WatermarkLayout::BottomLeft;
        watermarks.assets = {asset};
        watermarks.presets = {center, corner};
        watermarks.defaultPresetId = center.id;
        watermarks.autoEnabled = true;
        watermarks.retainOriginal = true;
        auto& watermarkService = ApplicationControllerTestAccess::watermarks(c);
        QVERIFY(watermarkService.savePreferences(watermarks).isEmpty());
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        auto& history = ApplicationControllerTestAccess::history(c);
        qint64 historyBytes = 0;
        if (historyEnabled)
        {
            QImage image(32, 24, QImage::Format_RGB32);
            image.fill(Qt::green);
            QVERIFY(history.record(history.beginSession(), image, HistorySource::Overlay).success);
            historyBytes = history.totalUsage().managedBytes;
        }
        const auto group = history.currentGroupId();
        c.openResourceBrowser(QStringLiteral("quick-gunner"));
        auto* browser = ApplicationControllerTestAccess::browser(c);
        QVERIFY(browser);
        auto* list = browser->findChild<QListWidget*>(QStringLiteral("historyThumbnails"));
        auto* combo = browser->findChild<QComboBox*>(QStringLiteral("resourceSourceCombo"));
        auto* preview = browser->findChild<HistoryImageView*>(QStringLiteral("historyImageView"));
        QVERIFY(list && combo && preview);
        QCOMPARE(combo->count(), historyEnabled ? 2 : 1);
        QSignalSpy saved(h.saves, &QuickGunnerSaveQueue::saved);
        QSignalSpy dropped(&gunner, &QuickGunnerController::shotsDropped);
        // Windows 共享流程使用 fake 激活，托盘仍按既有用例保持禁用。
        if (regionCaptureSupported())
            c.menu()->findChild<QAction*>(QStringLiteral("quickGunnerModeAction"))->trigger();
        else
            QVERIFY(gunner.activate({}).isEmpty());
        QVERIFY(gunner.active());
        QString selected;
        QImage original(100, 80, QImage::Format_RGB32);
        original.fill(Qt::red);
        const auto centered = ApplicationControllerTestAccess::watermark(c, original);
        QVERIFY(centered != original);
        for (int i = 0; i < 10; ++i)
        {
            if (i == 4)
                QVERIFY(watermarkService.setAutoEnabled(false).isEmpty());
            if (i == 7)
            {
                QVERIFY(watermarkService.setCurrentPreset(corner.id).isEmpty());
                QVERIFY(watermarkService.setAutoEnabled(true).isEmpty());
                const auto changed = ApplicationControllerTestAccess::watermark(c, original);
                QVERIFY(changed != centered && changed != original);
            }
            const auto expected = ApplicationControllerTestAccess::watermark(c, original);
            raw->fire(HotkeyAction::QuickGunnerShutter);
            QCOMPARE(h.calls->requests.size(), i + 1);
            h.complete(i);
            QTRY_COMPARE_WITH_TIMEOUT(list->count(), i + 1, 5000);
            QTRY_COMPARE(saved.size(), i + 1);
            if (!jpeg)
                QCOMPARE(QImage(saved.last()[0].toString()).convertToFormat(QImage::Format_ARGB32),
                         expected.convertToFormat(QImage::Format_ARGB32));
            if (i == 0)
            {
                selected = browser->selectedItemId();
                preview->actualSize();
            }
            QCOMPARE(browser->selectedItemId(), selected);
            QCOMPARE(preview->zoom(), qreal(1));
        }
        QCOMPARE(saved.size(), 10);
        QVERIFY(dropped.isEmpty());
        const QDir directory(gunner.preferences().directory);
        const auto files = directory.entryList(QDir::Files);
        QCOMPARE(files.size(), 10);
        QVERIFY(!browser->findChild<QToolButton*>(QStringLiteral("historyOriginalToggle"))
                     ->isVisible());
        for (const auto& file : files)
        {
            QVERIFY(file.endsWith(jpeg ? QStringLiteral(".jpg") : QStringLiteral(".png")));
            const QImage image(directory.filePath(file));
            QCOMPARE(image.size(), QSize(100, 80));
            QVERIFY(image.pixelColor(99, 0).red() > 240);
        }
        if (historyEnabled)
        {
            QCOMPARE(history.totalUsage().managedCount, 1);
            QCOMPARE(history.totalUsage().externalCount, 0);
            QCOMPARE(history.totalUsage().managedBytes, historyBytes);
            QCOMPARE(history.runCleanup().deletedCount, 0);
            QCOMPARE(directory.entryList(QDir::Files).size(), 10);
        }
        auto button = [browser](const char* name)
        { return browser->findChild<QToolButton*>(QLatin1String(name)); };
        button("historyStar")->click();
        QCOMPARE(ApplicationControllerTestAccess::source(c).snapshot().usage.starredCount, 1);
        button("historyCopy")->click();
        QCOMPARE(copied, QImage(selected));
        QTest::keyClick(browser, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(pinned, copied);
        QVERIFY(browser->isVisible());
        button("historyReveal")->click();
        QCOMPARE(revealed, selected);
        button("historyDelete")->click();
        QCOMPARE(list->count(), 9);
        QVERIFY(!QFile::exists(selected));
        QVERIFY(!browser->selectedItemId().isEmpty());
        // 清空走真实窗口确认，文件操作只作用于临时目录的回收站替身。
        bool confirmed = false;
        QTimer answer;
        answer.setInterval(10);
        connect(&answer, &QTimer::timeout, browser,
                [&]
                {
                    auto* box =
                        browser->findChild<QMessageBox*>(QStringLiteral("resourceClearConfirm"));
                    if (!box)
                        return;
                    confirmed = true;
                    answer.stop();
                    box->done(QMessageBox::Yes);
                });
        answer.start();
        button("resourceClearAll")->click();
        QVERIFY(confirmed);
        QCOMPARE(list->count(), 0);
        QCOMPARE(trashed.size(), 10);
        QCOMPARE(history.currentGroupId(), group);
        if (historyEnabled)
        {
            const auto usage = history.totalUsage();
            QCOMPARE(usage.managedCount, 1);
            QCOMPARE(usage.externalCount, 0);
            QCOMPARE(usage.managedBytes, historyBytes);
            QCOMPARE(history.runCleanup().deletedCount, 0);
            c.openHistory();
            QCOMPARE(ApplicationControllerTestAccess::browser(c), browser);
            QCOMPARE(browser->selectedSourceId(), QStringLiteral("history"));
            QCOMPARE(list->count(), 1);
            c.openResourceBrowser(QStringLiteral("quick-gunner"));
            QCOMPARE(history.currentGroupId(), group);
        }
        else
            QVERIFY(!QFileInfo::exists(options.historyRoot));
        gunner.deactivate();
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
    }
    void failedDirectoryRecoveryAndReselectionReachOpenBrowser()
    {
        Harness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("settings.ini"));
        options.historyEnabledOverride = false;
        options.historyRoot = h.temp.filePath(QStringLiteral("history"));
        auto keys = std::make_unique<Hotkeys>();
        auto* raw = keys.get();
        ApplicationController c(*qApp, options, {}, {}, {}, {}, {}, std::move(keys));
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        QVERIFY(gunner.activate({}).isEmpty());
        c.openResourceBrowser(QStringLiteral("quick-gunner"));
        auto* browser = ApplicationControllerTestAccess::browser(c);
        auto* list = browser->findChild<QListWidget*>(QStringLiteral("historyThumbnails"));
        const auto directory = gunner.preferences().directory;
        QVERIFY(QDir(directory).removeRecursively());
        for (int i = 0; i < 2; ++i)
        {
            raw->fire(HotkeyAction::QuickGunnerShutter);
            h.complete(i);
            QTRY_COMPARE(h.saves->retryCount(), i + 1);
            QVERIFY(gunner.active());
        }
        QCOMPARE(list->count(), 0);
        QVERIFY(QDir().mkdir(directory));
        // 不显式重试或刷新浏览器，验证两个真实定时恢复链路。
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 6500);
        QTRY_COMPARE(gunner.pendingSaveCount(), 0);
        QVERIFY(QDir(directory).removeRecursively());
        raw->fire(HotkeyAction::QuickGunnerShutter);
        h.complete(2);
        QTRY_COMPARE(h.saves->retryCount(), 1);
        const auto replacement = h.temp.filePath(QStringLiteral("new shots"));
        QVERIFY(QDir().mkdir(replacement));
        QVERIFY(gunner.setDirectory(replacement, {options.historyRoot}).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 5000);
        QTRY_COMPARE(gunner.pendingSaveCount(), 0);
        QCOMPARE(QFileInfo(browser->selectedItemId()).absolutePath(),
                 QFileInfo(replacement).canonicalFilePath());
        QCOMPARE(QDir(replacement).entryList(QDir::Files).size(), 1);
        QVERIFY(!QFileInfo::exists(options.historyRoot));
        QVERIFY(gunner.active());
    }
    void firstFramingCancelAndActivate_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::newRow("cancel") << false;
        QTest::newRow("accept") << true;
    }
    void firstFramingCancelAndActivate()
    {
        if (!regionCaptureSupported())
            QSKIP("Windows 原生区域采集后补。");
        QFETCH(bool, accept);
        Harness h(true, false);
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("application.ini"));
        QPointer<QuickGunnerRegionPicker> picker;
        auto gunner = std::make_unique<QuickGunnerController>(
            h.settings, h.displays, h.capture,
            [&](CaptureFrame frame, QWidget* parent)
            {
                picker = new QuickGunnerRegionPicker(std::move(frame), parent);
                return picker.data();
            });
        ApplicationController c(*qobject_cast<QApplication*>(qApp), options, {}, {}, {}, {}, {},
                                std::make_unique<Hotkeys>());
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(gunner), std::make_unique<Displays>(), std::make_unique<Capture>());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        auto* mode = c.menu()->findChild<QAction*>(QStringLiteral("quickGunnerModeAction"));
        QVERIFY(mode);
        mode->trigger();
        QVERIFY(mode->isChecked());
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::green);
        h.capture.callbacks.last()({{h.displays.target, image, 0}, CaptureError::None, {}});
        QVERIFY(picker);
        if (accept)
            picker->regionSelected({20, 40, 100, 80});
        else
            picker->close();
        QCOMPARE(mode->isChecked(), accept);
        QCOMPARE(ApplicationControllerTestAccess::registered(c), accept);
        QCOMPARE(ApplicationControllerTestAccess::quickGunner(c).active(), accept);
    }
    void firstDirectorySelection_data()
    {
        QTest::addColumn<int>("decision");
        QTest::newRow("cancel") << 0;
        QTest::newRow("select-then-frame") << 1;
        QTest::newRow("reserved-directory") << 2;
    }
    void firstDirectorySelection()
    {
        if (!regionCaptureSupported())
            QSKIP("Windows 原生区域采集后补。");
        QFETCH(int, decision);
        Harness h(false, false);
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("application.ini"));
        // 历史关闭仍保护已配置根路径，不因快枪手入口创建历史目录。
        options.historyEnabledOverride = false;
        options.historyRoot = h.temp.filePath(QStringLiteral("reserved"));
        QVERIFY(QDir().mkdir(options.historyRoot));
        ApplicationController c(*qobject_cast<QApplication*>(qApp), options, {}, {}, {}, {}, {},
                                std::make_unique<Hotkeys>());
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        bool saw = false;
        if (decision == 2)
            QTest::ignoreMessage(
                QtWarningMsg, "快枪手：快枪手目录不能与历史根目录或分组目录相同，也不能互相包含。");
        QTimer::singleShot(0,
                           [&]
                           {
                               auto* dialog =
                                   qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                               QVERIFY(dialog);
                               saw = true;
                               if (decision == 0)
                                   dialog->reject();
                               else
                               {
                                   const auto directory =
                                       decision == 1 ? h.temp.filePath(QStringLiteral("shots"))
                                                     : options.historyRoot;
                                   if (decision == 1)
                                       QVERIFY(QDir().mkdir(directory));
                                   dialog->setDirectory(directory);
                                   QMetaObject::invokeMethod(dialog, "accept");
                               }
                           });
        auto* mode = c.menu()->findChild<QAction*>(QStringLiteral("quickGunnerModeAction"));
        QVERIFY(mode);
        mode->trigger();
        QVERIFY(saw);
        QCOMPARE(mode->isChecked(), decision == 1);
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        QCOMPARE(gunner.state(), decision == 1 ? QuickGunnerController::State::Framing
                                               : QuickGunnerController::State::Idle);
        QVERIFY(!ApplicationControllerTestAccess::registered(c));
        QVERIFY(!QFile::exists(QDir(options.historyRoot).filePath(QStringLiteral(".waibusnap"))));
        if (decision == 1)
        {
            QCOMPARE(h.settings.loadQuickGunnerPreferences().directory,
                     h.temp.filePath(QStringLiteral("shots")));
            gunner.deactivate();
            QVERIFY(!mode->isChecked());
        }
        else
            QVERIFY(gunner.preferences().directory.isEmpty());
    }
    void quitBacklog_data()
    {
        QTest::addColumn<bool>("discard");
        QTest::addColumn<int>("kind");
        QTest::newRow("capture-cancel") << false << 0;
        QTest::newRow("capture-discard") << true << 0;
        QTest::newRow("save-retry-cancel") << false << 1;
        QTest::newRow("save-retry-discard") << true << 1;
        QTest::newRow("empty-no-dialog") << true << 2;
        QTest::newRow("in-flight-only-cancel") << false << 3;
        QTest::newRow("in-flight-only-discard") << true << 3;
    }
    void quitBacklog()
    {
        QFETCH(bool, discard);
        QFETCH(int, kind);
        Harness h;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = h.temp.filePath(QStringLiteral("application.ini"));
        options.metricsFile = h.temp.filePath(QStringLiteral("metrics.jsonl"));
        ApplicationController c(*qobject_cast<QApplication*>(qApp), options);
        ApplicationControllerTestAccess::installForFramingTest(
            c, std::move(h.controller), std::make_unique<Displays>(), std::make_unique<Capture>());
        auto& gunner = ApplicationControllerTestAccess::quickGunner(c);
        if (kind == 0 || kind == 3)
        {
            QVERIFY(gunner.shutter().isEmpty());
            if (kind == 0)
                QVERIFY(gunner.shutter().isEmpty());
            QCOMPARE(gunner.pendingCaptureCount(), kind == 0 ? 1 : 0);
            QVERIFY(gunner.capturing());
        }
        else if (kind == 1)
        {
            QVERIFY(QDir(gunner.preferences().directory).removeRecursively());
            QVERIFY(gunner.shutter().isEmpty());
            h.complete(0);
            QTRY_COMPARE(h.saves->retryCount(), 1);
            QCOMPARE(gunner.pendingSaveCount(), 1);
        }
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::green);
        const auto windows =
            ApplicationControllerTestAccess::pausedUi(c, {h.displays.target, image, 0});
        bool saw = false;
        QString backlogText;
        if (kind != 2)
            QTimer::singleShot(
                0, &c,
                [&]
                {
                    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    QVERIFY(box);
                    saw = true;
                    for (auto* window : windows)
                        QVERIFY(!window->isVisible());
                    backlogText = box->text();
                    auto* cancel =
                        box->findChild<QPushButton*>(QStringLiteral("quickGunnerQuitCancel"));
                    auto* leave =
                        box->findChild<QPushButton*>(QStringLiteral("quickGunnerQuitDiscard"));
                    QVERIFY(cancel && leave);
                    QCOMPARE(box->defaultButton(), cancel);
                    (discard ? leave : cancel)->click();
                });
        c.quit();
        QCOMPARE(saw, kind != 2);
        if (kind != 2)
            QCOMPARE(backlogText, QStringLiteral("快枪手还有 %1 张图片待保存或重试，退出会丢失。")
                                      .arg(kind == 0 ? 2 : 1));
        QCOMPARE(ApplicationControllerTestAccess::quitting(c), discard);
        if (!discard)
        {
            QVERIFY(c.active());
            for (auto* window : windows)
                QVERIFY(window->isVisible());
        }
    }
    void trayEntriesAndGuards()
    {
        QTemporaryDir temp;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        ApplicationController controller(*qobject_cast<QApplication*>(qApp), options);
        auto* mode =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerModeAction"));
        auto* frame =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerFrameAction"));
        auto* directory =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerDirectoryAction"));
        auto* status =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerStatusAction"));
        auto* open =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerOpenAction"));
        QVERIFY(mode && frame && directory && status && open);
        QVERIFY(open->isEnabled());
        QVERIFY(mode->isCheckable());
        QVERIFY(!mode->isChecked());
        QVERIFY(!controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerShootAction")));
        QVERIFY(!status->isEnabled());
        QCOMPARE(mode->isEnabled(), regionCaptureSupported());
        QCOMPARE(frame->isEnabled(), regionCaptureSupported());
        QVERIFY(directory->isEnabled());
        for (int which = 0; which < 4; ++which)
        {
            ApplicationControllerTestAccess::guard(controller, which, true);
            QVERIFY(!mode->isEnabled());
            QVERIFY(!frame->isEnabled());
            QVERIFY(!directory->isEnabled());
            QVERIFY(!open->isEnabled());
            ApplicationControllerTestAccess::guard(controller, which, false);
            QCOMPARE(mode->isEnabled(), regionCaptureSupported());
        }
        const auto actions = controller.menu()->actions();
        auto* quickMenu = controller.menu()->findChild<QMenu*>(QStringLiteral("quickGunnerMenu"));
        QVERIFY(quickMenu);
        QCOMPARE(quickMenu->actions(), (QList<QAction*>{mode, open, frame, directory, status}));
        const auto position = actions.indexOf(quickMenu->menuAction());
        QVERIFY(position > 0);
        QCOMPARE(actions[position - 1]->objectName(), QStringLiteral("scrollCaptureAction"));
    }
};
QTEST_MAIN(QuickGunnerCaptureTest)
#include "quick_gunner_capture_test.moc"
