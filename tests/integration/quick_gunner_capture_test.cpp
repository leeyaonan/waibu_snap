#include "app/application_controller.h"
#include "app/quick_gunner.h"
#include "app/quick_gunner_save_queue.h"
#include "ui/quick_gunner_flash_window.h"
#include <QAction>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
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
    }
    static QuickGunnerController& quickGunner(ApplicationController& controller)
    {
        return *controller.quickGunner_;
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
    void capture(const DisplayTarget&, std::function<void(CaptureResult)>) override {}
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
    Harness(bool directory = true, bool region = true)
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
        auto queue = std::make_unique<QuickGunnerSaveQueue>(settings);
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
    void trayEntriesAndGuards()
    {
        QTemporaryDir temp;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        ApplicationController controller(*qobject_cast<QApplication*>(qApp), options);
        auto* shoot =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerShootAction"));
        auto* frame =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerFrameAction"));
        auto* directory =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerDirectoryAction"));
        auto* status =
            controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerStatusAction"));
        QVERIFY(shoot && frame && directory && status);
        QVERIFY(!status->isEnabled());
        QCOMPARE(shoot->isEnabled(), regionCaptureSupported());
        QCOMPARE(frame->isEnabled(), regionCaptureSupported());
        QCOMPARE(directory->isEnabled(), regionCaptureSupported());
        QVERIFY(!controller.menu()->findChild<QAction*>(QStringLiteral("quickGunnerModeAction")));
        for (int which = 0; which < 4; ++which)
        {
            ApplicationControllerTestAccess::guard(controller, which, true);
            QVERIFY(!shoot->isEnabled());
            QVERIFY(!frame->isEnabled());
            QVERIFY(!directory->isEnabled());
            ApplicationControllerTestAccess::guard(controller, which, false);
            QCOMPARE(shoot->isEnabled(), regionCaptureSupported());
        }
        const auto actions = controller.menu()->actions();
        auto* quickMenu = controller.menu()->findChild<QMenu*>(QStringLiteral("quickGunnerMenu"));
        QVERIFY(quickMenu);
        const auto position = actions.indexOf(quickMenu->menuAction());
        QVERIFY(position > 0);
        QCOMPARE(actions[position - 1]->objectName(), QStringLiteral("scrollCaptureAction"));
    }
};
QTEST_MAIN(QuickGunnerCaptureTest)
#include "quick_gunner_capture_test.moc"
