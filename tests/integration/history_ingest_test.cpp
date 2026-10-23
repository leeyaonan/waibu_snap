#include "app/application_controller.h"
#include "app/history_manager.h"
#include "session/session_metrics.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLockFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
using namespace waibusnap;
namespace
{
QImage pixels(QColor color = QColor(20, 60, 100, 150))
{
    QImage image(160, 240, QImage::Format_ARGB32);
    image.fill(color);
    image.setPixelColor(5, 7, QColor(210, 35, 70, 255));
    image.setDevicePixelRatio(2);
    return image;
}
CaptureFrame frame()
{
    return {{1, QRect(0, 0, 640, 480), 1, 1, QRect(0, 0, 640, 480)}, pixels(), 0};
}
RunOptions options(const QTemporaryDir& temp)
{
    if (!temp.isValid())
        qFatal("无法创建临时目录，拒绝使用真实历史库。");
    RunOptions result;
    result.historyRoot = temp.filePath(QStringLiteral("历史 根目录"));
    result.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
    result.metricsFile = temp.filePath(QStringLiteral("metrics.jsonl"));
    return result;
}
QVector<HistoryItem> items(const QString& root)
{
    QVector<HistoryItem> result;
    for (const auto& group : HistoryStore(root).scan().groups)
        result += group.items;
    return result;
}
void comparePixels(const QImage& actual, QImage expected)
{
    QCOMPARE(actual.size(), expected.size());
    QCOMPARE(actual.devicePixelRatio(), qreal(1));
    expected = expected.convertToFormat(QImage::Format_ARGB32);
    const QImage normalized = actual.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x)
            QCOMPARE(normalized.pixelColor(x, y), expected.pixelColor(x, y));
}
QPushButton* button(QWidget& widget, const char* name)
{
    auto* result = widget.findChild<QPushButton*>(QLatin1String(name));
    if (!result)
        qFatal("测试按钮缺失：%s", name);
    return result;
}
OverlayActions actionsFor(HistoryManager& manager, HistorySession session)
{
    OverlayActions actions;
    actions.historySessionId = session.id;
    actions.historyGroupId = session.groupId;
    actions.recordHistory = [&manager, session](const QImage& image, const QString&)
    { return manager.record(session, image, HistorySource::Overlay); };
    actions.copyImage =
        actions.pinImage = [](const QImage&) { return ImageOutputResult{true, {}}; };
    return actions;
}
class Displays final : public DisplayTopology
{
    DisplayResult cursorDisplay() const override { return {frame().display, {}}; }
    bool stillMatches(const DisplayTarget&) const override { return true; }
};
class Capture final : public CaptureProvider
{
    bool hasPermission() const override { return true; }
    bool requestPermission() override { return true; }
    void capture(const DisplayTarget&, std::function<void(CaptureResult)> callback) override
    {
        QTimer::singleShot(0, qApp, [callback] { callback({frame(), CaptureError::None, {}}); });
    }
};
class Windows final : public WindowEnumerator
{
    WindowListResult visibleWindows(const DisplayTarget&) const override { return {}; }
};
struct Frames
{
    std::function<void(QImage)> send;
};
class Region final : public RegionCaptureSession
{
  public:
    explicit Region(std::shared_ptr<Frames> frames) : frames_(std::move(frames)) {}
    void start(std::function<void(QImage)> send, std::function<void(QString)>) override
    {
        frames_->send = std::move(send);
    }
    void stop() override { frames_->send = {}; }

  private:
    std::shared_ptr<Frames> frames_;
};
class Injector final : public ScrollInjector
{
    bool hasAccessibilityPermission() const override { return false; }
    void requestAccessibilityPermission() override {}
    bool postScroll(ScrollInjection) override { return false; }
};
}
namespace waibusnap
{
struct OverlayScrollTestAccess
{
    static void select(SelectionOverlay& overlay, QRect rect = QRect(0, 0, 160, 240))
    {
        overlay.setSelection(rect);
        overlay.ensureToolbar();
        overlay.updateToolbar();
    }
    static void enable(SelectionOverlay& overlay) { overlay.scrollSupported_ = true; }
    static void annotate(SelectionOverlay& overlay)
    {
        Annotation annotation;
        annotation.type = AnnotationType::Cover;
        annotation.first = {20, 20};
        annotation.last = {70, 80};
        annotation.style.color = Qt::red;
        overlay.history_.add(annotation);
        overlay.markDirty();
    }
    static void ocrCopy(SelectionOverlay& overlay)
    {
        overlay.textMode_ = true;
        overlay.ocrResult_ = {true, {}, {{QStringLiteral("文字"), {0, 0, 50, 20}, {}}}};
        overlay.ocrSelection_ =
            text_layout::selectionRange(overlay.ocrResult_.lines, {0, 0}, {0, 0});
        overlay.copyShortcut();
    }
};
struct ScrollCaptureTestAccess
{
    static ScrollCaptureSession* create(RegionCaptureRequest request, QRect bar,
                                        std::shared_ptr<Frames> frames, QObject* parent)
    {
        return new ScrollCaptureSession(
            request, bar, std::make_unique<Region>(frames), std::make_unique<Injector>(),
            [] { return QPoint(); }, {}, parent);
    }
};
struct ApplicationControllerTestAccess
{
    static HistoryManager& history(ApplicationController& controller)
    {
        return controller.history_;
    }
    static SelectionOverlay* overlay(ApplicationController& controller)
    {
        return controller.overlay_;
    }
    static ScrollResultWindow* result(ApplicationController& controller)
    {
        return controller.scrollResults_.isEmpty() ? nullptr
                                                   : controller.scrollResults_.last().data();
    }
    static void prepare(ApplicationController& controller, std::shared_ptr<Frames> frames,
                        QString savePath = {})
    {
        controller.displays_ = std::make_unique<Displays>();
        controller.capture_ = std::make_unique<Capture>();
        controller.windows_ = std::make_unique<Windows>();
        controller.overlayFactory_ = [savePath](CaptureFrame input, OverlayActions actions,
                                                QVector<QRect> windows, QString error)
        {
            actions.copyImage =
                actions.pinImage = [](const QImage&) { return ImageOutputResult{true, {}}; };
            actions.chooseSavePath = [savePath](QWidget*, const QString&) { return savePath; };
            auto* overlay =
                new SelectionOverlay(std::move(input), std::move(actions), windows, error);
            OverlayScrollTestAccess::enable(*overlay);
            return overlay;
        };
        controller.scrollFactory_ =
            [frames](RegionCaptureRequest request, QRect bar, QObject* parent)
        { return ScrollCaptureTestAccess::create(request, bar, frames, parent); };
    }
    static void completeScroll(ApplicationController& controller)
    {
        controller.scrollSession_->complete();
    }
};
}
class HistoryIngestTest final : public QObject
{
    Q_OBJECT
  private slots:
    void init() { QApplication::setQuitOnLastWindowClosed(false); }
    void originalVariantAndUpdateRemoval()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        AppSettings settings(run.settingsFile);
        HistoryManager manager(settings, run.historyRoot);
        QVERIFY(manager.initialize().success);
        const auto session = manager.beginSession();
        const QImage watermarked = pixels(QColor(20, 60, 200));
        const QImage original = pixels(QColor(200, 30, 40));
        const auto recorded =
            manager.record(session, watermarked, HistorySource::Overlay, original);
        QVERIFY2(recorded.success, qPrintable(recorded.error));
        QVERIFY(recorded.item.hasOriginal);
        QVERIFY(recorded.item.originalBytes > 0);
        const QString itemId = historyItemId(recorded.item);
        const auto loaded = manager.loadImage(itemId);
        QVERIFY2(loaded.success, qPrintable(loaded.error));
        comparePixels(loaded.image, watermarked);
        const auto loadedOriginal = manager.loadImage(itemId, {}, HistoryImageVariant::Original);
        QVERIFY2(loadedOriginal.success, qPrintable(loadedOriginal.error));
        comparePixels(loadedOriginal.image, original);
        const auto plain = manager.findItem(itemId);
        QVERIFY(plain && plain->hasOriginal);
        QVERIFY(plain->originalBytes > 0);

        const auto replaced =
            manager.record(session, pixels(QColor(20, 180, 40)), HistorySource::Overlay);
        QVERIFY2(replaced.success, qPrintable(replaced.error));
        QVERIFY(!replaced.item.hasOriginal);
        QVERIFY(!manager.loadImage(itemId, {}, HistoryImageVariant::Original).success);
    }
    void defaultsFallbackSwitchAndRestart()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        AppSettings settings(run.settingsFile);
        HistoryPreferences preferences{
            run.historyRoot, QUuid::createUuid().toString(QUuid::WithoutBraces), {}};
        preferences.defaultCleanup.maxCount = 7;
        QVERIFY(settings.saveHistoryPreferences(preferences).isEmpty());
        HistoryManager manager(settings);
        QVERIFY(manager.initialize().success);
        QVERIFY(manager.ready());
        QCOMPARE(manager.groups().size(), 1);
        QVERIFY(manager.currentGroup()->isDefault);
        QCOMPARE(manager.currentGroup()->cleanup.maxCount, 7);
        QCOMPARE(settings.loadHistoryPreferences().currentGroupId, manager.currentGroupId());
        const auto oldSession = manager.beginSession();
        const auto created = HistoryStore(run.historyRoot).createGroup(QStringLiteral("会议"));
        QVERIFY(created.success);
        QVERIFY(manager.setCurrentGroup(created.group.id).success);
        QCOMPARE(manager.groups().size(), 2);
        QVERIFY(!manager.setCurrentGroup(QStringLiteral("missing")).success);
        QCOMPARE(manager.currentGroupId(), created.group.id);
        QVERIFY(manager.record(oldSession, pixels(), HistorySource::Overlay).success);
        QCOMPARE(items(run.historyRoot).first().groupId, oldSession.groupId);
        HistoryManager restarted(settings);
        QVERIFY(restarted.initialize().success);
        QCOMPARE(restarted.currentGroupId(), created.group.id);
        QVERIFY(restarted.beginSession().id != oldSession.id);
    }
    void missingIdAndRootOverride()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        AppSettings settings(run.settingsFile);
        const QString configured = temp.filePath(QStringLiteral("配置根目录"));
        QVERIFY(settings.saveHistoryPreferences({configured, {}, {}}).isEmpty());
        HistoryManager manager(settings, run.historyRoot);
        QVERIFY(manager.initialize().success);
        QCOMPARE(settings.loadHistoryPreferences().rootDirectory, configured);
        QVERIFY(!QFileInfo::exists(configured));
        QCOMPARE(settings.loadHistoryPreferences().currentGroupId, manager.currentGroupId());
    }
    void rootFailureIsRetryable()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        QFile obstacle(run.historyRoot);
        QVERIFY(obstacle.open(QIODevice::WriteOnly));
        obstacle.close();
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QSignalSpy errors(&manager, &HistoryManager::errorChanged);
        QVERIFY(!manager.initialize().success);
        QVERIFY(!manager.ready());
        QVERIFY(!manager.error().isEmpty());
        QVERIFY(!manager.record(manager.beginSession(), pixels(), HistorySource::Overlay).success);
        QVERIFY(!errors.isEmpty());
        QVERIFY(QFile::remove(run.historyRoot));
        QVERIFY(manager.initialize().success);
        QVERIFY(manager.record(manager.beginSession(), pixels(), HistorySource::Overlay).success);
        QVERIFY(manager.error().isEmpty());
    }
    void switchingSettingsFailureKeepsCurrentGroup()
    {
        QTemporaryDir temp;
        auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        const auto initial = manager.currentGroupId();
        const auto group = HistoryStore(run.historyRoot).createGroup(QStringLiteral("另一组"));
        QVERIFY(group.success);
        QVERIFY(QFile::remove(run.settingsFile));
        QVERIFY(QDir().mkdir(run.settingsFile));
        QVERIFY(!manager.setCurrentGroup(group.group.id).success);
        QCOMPARE(manager.currentGroupId(), initial);
    }
    void sessionUpdatesAndRejectsRebinding()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        const auto session = manager.beginSession();
        auto first = manager.record(session, pixels(), HistorySource::Overlay);
        QVERIFY(first.success);
        auto changed = pixels(Qt::green);
        auto second = manager.record(session, changed, HistorySource::Overlay);
        QVERIFY(second.success);
        QCOMPARE(first.item.absolutePath, second.item.absolutePath);
        QCOMPARE(first.item.createdAtMs, second.item.createdAtMs);
        QCOMPARE(items(run.historyRoot).size(), 1);
        comparePixels(QImage(second.item.absolutePath), changed);
        QVERIFY(!manager.record(session, changed, HistorySource::Scroll).success);
        QVERIFY(!manager
                     .record({session.id, QUuid::createUuid().toString()}, changed,
                             HistorySource::Overlay)
                     .success);
        QCOMPARE(items(run.historyRoot).size(), 1);
    }
    void disabledControllerDoesNotWriteHistory()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        auto run = options(temp);
        run.historyEnabledOverride = false;
        ApplicationController controller(*qApp, run);
        auto frames = std::make_shared<Frames>();
        ApplicationControllerTestAccess::prepare(controller, frames,
                                                 temp.filePath(QStringLiteral("output.png")));
        controller.trigger(QStringLiteral("privacy-disabled"));
        QTRY_VERIFY(ApplicationControllerTestAccess::overlay(controller));
        auto* overlay = ApplicationControllerTestAccess::overlay(controller);
        OverlayScrollTestAccess::select(*overlay);
        button(*overlay, "copyButton")->click();
        QCoreApplication::processEvents();
        QVERIFY(!QFileInfo::exists(run.historyRoot));
        QVERIFY(!ApplicationControllerTestAccess::history(controller).ready());
    }
    void overlaySuccess_data()
    {
        QTest::addColumn<QString>("operation");
        for (const char* name : {"copyButton", "pinButton", "saveButton", "export"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void overlaySuccess()
    {
        QFETCH(QString, operation);
        QTemporaryDir temp;
        const auto run = options(temp);
        ApplicationController controller(*qApp, run);
        auto frames = std::make_shared<Frames>();
        ApplicationControllerTestAccess::prepare(controller, frames,
                                                 temp.filePath(QStringLiteral("output.png")));
        controller.trigger(QStringLiteral("history-test"));
        QTRY_VERIFY(ApplicationControllerTestAccess::overlay(controller));
        auto* overlay = ApplicationControllerTestAccess::overlay(controller);
        OverlayScrollTestAccess::select(*overlay);
        OverlayScrollTestAccess::annotate(*overlay);
        const QImage expected =
            renderAnnotatedSelection(frame().pixels, overlay->annotations(), overlay->selection());
        QSignalSpy finished(overlay, &SelectionOverlay::finished);
        if (operation == QStringLiteral("export"))
            QVERIFY(overlay->exportToPath(temp.filePath(QStringLiteral("output.png"))));
        else
            button(*overlay, operation.toLatin1().constData())->click();
        const auto stored = items(run.historyRoot);
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().source, HistorySource::Overlay);
        comparePixels(QImage(stored.first().absolutePath), expected);
        if (operation == QStringLiteral("saveButton") || operation == QStringLiteral("export"))
        {
            QVERIFY(overlay->isSelectionSaved());
            QVERIFY(finished.isEmpty());
            overlay->close();
        }
        else
        {
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(1).toInt(), operation == QStringLiteral("copyButton")
                                                         ? copiedSessionOutcome
                                                         : pinnedSessionOutcome);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void saveThenEditAndOutput_data()
    {
        QTest::addColumn<QString>("last");
        for (const char* name : {"copyButton", "pinButton", "saveButton", "export"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void saveThenEditAndOutput()
    {
        QFETCH(QString, last);
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        auto actions = actionsFor(manager, manager.beginSession());
        QImage copied;
        bool sharedOutputBuffer = true;
        const auto record = actions.recordHistory;
        actions.recordHistory = [&](const QImage& image, const QString& path)
        {
            if (!copied.isNull())
                sharedOutputBuffer &= image.constBits() == copied.constBits();
            return record(image, path);
        };
        actions.copyImage = actions.pinImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        { return temp.filePath(QStringLiteral("out.png")); };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        QVERIFY(overlay.exportToPath(temp.filePath(QStringLiteral("first.png"))));
        const auto first = items(run.historyRoot).first();
        OverlayScrollTestAccess::annotate(overlay);
        const QImage expected =
            renderAnnotatedSelection(frame().pixels, overlay.annotations(), overlay.selection());
        if (last == QStringLiteral("export"))
            QVERIFY(overlay.exportToPath(temp.filePath(QStringLiteral("second.png"))));
        else
            button(overlay, last.toLatin1().constData())->click();
        const auto stored = items(run.historyRoot);
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().absolutePath, first.absolutePath);
        comparePixels(QImage(first.absolutePath), expected);
        if (!copied.isNull())
            comparePixels(copied, expected);
        QVERIFY(sharedOutputBuffer);
    }
    void jpegSaveKeepsLosslessPngHistory()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        auto actions = actionsFor(manager, manager.beginSession());
        actions.loadSavePreferences = [&]
        { return SavePreferences{temp.path(), ImageFormat::Jpeg}; };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        button(overlay, "saveButton")->click();
        QVERIFY(overlay.isSelectionSaved());
        QCOMPARE(QDir(temp.path()).entryList({QStringLiteral("*.jpg")}, QDir::Files).size(), 1);
        const auto stored = items(run.historyRoot);
        QCOMPARE(stored.size(), 1);
        QVERIFY(stored.first().fileName.endsWith(QStringLiteral(".png")));
        comparePixels(QImage(stored.first().absolutePath), pixels());
        QCOMPARE(QImage(stored.first().absolutePath).pixelColor(0, 0).alpha(), 150);
    }
    void shapeCopyPreservesFinalImageAndSharedBuffer()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        auto actions = actionsFor(manager, manager.beginSession());
        QImage copied;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        const auto record = actions.recordHistory;
        bool shared = false;
        actions.recordHistory = [&](const QImage& image, const QString& path)
        {
            shared = copied.constBits() == image.constBits();
            return record(image, path);
        };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        button(overlay, "shapeButton")->click();
        button(overlay, "shapeOption1Button")->click();
        button(overlay, "copyButton")->click();
        QVERIFY(shared);
        QCOMPARE(items(run.historyRoot).size(), 1);
        comparePixels(QImage(items(run.historyRoot).first().absolutePath), copied);
        QCOMPARE(copied.pixelColor(0, 0).alpha(), 0);
        QVERIFY(copied.pixelColor(copied.width() / 2, copied.height() / 2).alpha() > 0);
    }
    void noOutputNoHistory_data()
    {
        QTest::addColumn<QString>("operation");
        for (const char* name : {"cancel", "save-cancel", "copy-fail", "pin-fail", "save-fail",
                                 "export-fail", "ocr-text"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void noOutputNoHistory()
    {
        QFETCH(QString, operation);
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        auto actions = actionsFor(manager, manager.beginSession());
        actions.copyImage = actions.pinImage = [](const QImage&)
        { return ImageOutputResult{false, QStringLiteral("输出失败")}; };
        const QString failure = temp.filePath(QStringLiteral("missing/out.png"));
        actions.chooseSavePath = [=](QWidget*, const QString&)
        { return operation == QStringLiteral("save-cancel") ? QString() : failure; };
        QString copiedText;
        actions.setClipboardText = [&](const QString& text) { copiedText = text; };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        if (operation == QStringLiteral("cancel"))
            overlay.close();
        else if (operation.startsWith(QStringLiteral("save")))
            button(overlay, "saveButton")->click();
        else if (operation == QStringLiteral("export-fail"))
            QVERIFY(!overlay.exportToPath(failure));
        else if (operation == QStringLiteral("ocr-text"))
            OverlayScrollTestAccess::ocrCopy(overlay);
        else
            button(overlay, operation == QStringLiteral("copy-fail") ? "copyButton" : "pinButton")
                ->click();
        if (operation == QStringLiteral("ocr-text"))
            QCOMPARE(copiedText, QStringLiteral("文字"));
        QVERIFY(!overlay.isSelectionSaved());
        QVERIFY(items(run.historyRoot).isEmpty());
    }
    void historyFailureDoesNotAffectOutput_data() { overlaySuccess_data(); }
    void historyFailureDoesNotAffectOutput()
    {
        QFETCH(QString, operation);
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        QLockFile lock(QDir(run.historyRoot).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock());
        auto actions = actionsFor(manager, manager.beginSession());
        actions.chooseSavePath = [&](QWidget*, const QString&)
        { return temp.filePath(QStringLiteral("out.png")); };
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        if (operation == QStringLiteral("export"))
            QVERIFY(overlay.exportToPath(temp.filePath(QStringLiteral("out.png"))));
        else
            button(overlay, operation.toLatin1().constData())->click();
        QVERIFY(!manager.error().isEmpty());
        if (operation == QStringLiteral("saveButton") || operation == QStringLiteral("export"))
        {
            QVERIFY(overlay.isSelectionSaved());
            QVERIFY(finished.isEmpty());
            QVERIFY(QFileInfo::exists(temp.filePath(QStringLiteral("out.png"))));
        }
        else
        {
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(1).toInt(), operation == QStringLiteral("copyButton")
                                                         ? copiedSessionOutcome
                                                         : pinnedSessionOutcome);
        }
        lock.unlock();
        QVERIFY(items(run.historyRoot).isEmpty());
    }
    void scrollCompletionCropRetry_data()
    {
        QTest::addColumn<bool>("failed");
        QTest::newRow("normal") << false;
        QTest::newRow("history-unavailable") << true;
    }
    void scrollCompletionCropRetry()
    {
        QFETCH(bool, failed);
        QTemporaryDir temp;
        const auto run = options(temp);
        if (failed)
        {
            QFile obstacle(run.historyRoot);
            QVERIFY(obstacle.open(QIODevice::WriteOnly));
        }
        ApplicationController controller(*qApp, run);
        const auto frames = std::make_shared<Frames>();
        ApplicationControllerTestAccess::prepare(controller, frames);
        controller.trigger(QStringLiteral("history-scroll-test"));
        QTRY_VERIFY(ApplicationControllerTestAccess::overlay(controller));
        auto* overlay = ApplicationControllerTestAccess::overlay(controller);
        const auto initialGroup =
            ApplicationControllerTestAccess::history(controller).currentGroupId();
        QString nextGroup;
        if (!failed)
        {
            const auto created =
                HistoryStore(run.historyRoot).createGroup(QStringLiteral("重试分组"));
            QVERIFY(created.success);
            nextGroup = created.group.id;
            QVERIFY(ApplicationControllerTestAccess::history(controller)
                        .setCurrentGroup(nextGroup)
                        .success);
        }
        overlay->enterScrollReady(QRect(0, 0, 160, 240));
        button(*overlay, "scrollStartButton")->click();
        QVERIFY(frames->send);
        QImage source = pixels().convertToFormat(QImage::Format_RGB32);
        source.setDevicePixelRatio(1);
        frames->send(source);
        ApplicationControllerTestAccess::completeScroll(controller);
        auto* result = ApplicationControllerTestAccess::result(controller);
        QVERIFY(result && result->isVisible());
        auto stored = items(run.historyRoot);
        QCOMPARE(stored.size(), failed ? 0 : 1);
        QString firstPath;
        if (!failed)
        {
            firstPath = stored.first().absolutePath;
            QCOMPARE(stored.first().groupId, initialGroup);
            QCOMPARE(stored.first().source, HistorySource::Scroll);
            comparePixels(QImage(firstPath), source);
        }
        button(*result, "scrollResultZoom100Button")->click();
        button(*result, "scrollResultCropButton")->click();
        auto* view = result->findChild<QWidget*>(QStringLiteral("scrollResultView"));
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, QPoint(60, 1));
        QTest::mouseMove(view, QPoint(60, 30));
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, QPoint(60, 30));
        button(*result, "scrollResultCropApplyButton")->click();
        QCOMPARE(result->image().size(), QSize(160, 210));
        QVERIFY(!result->hasOutput());
        if (!failed)
        {
            QCOMPARE(items(run.historyRoot).size(), 1);
            comparePixels(QImage(firstPath), source.copy(0, 30, 160, 210));
        }
        button(*result, "scrollResultRetryButton")->click();
        QTRY_VERIFY(ApplicationControllerTestAccess::overlay(controller));
        overlay = ApplicationControllerTestAccess::overlay(controller);
        QVERIFY(overlay->isScrollReady());
        button(*overlay, "scrollStartButton")->click();
        QVERIFY(frames->send);
        source.fill(Qt::blue);
        frames->send(source);
        ApplicationControllerTestAccess::completeScroll(controller);
        result = ApplicationControllerTestAccess::result(controller);
        QVERIFY(result && result->isVisible());
        QCOMPARE(items(run.historyRoot).size(), failed ? 0 : 2);
        if (!failed)
        {
            const auto retried = HistoryStore(run.historyRoot).itemsForGroup(nextGroup);
            QVERIFY(retried.success);
            QCOMPARE(retried.items.size(), 1);
            comparePixels(QImage(retried.items.first().absolutePath), source);
            comparePixels(QImage(firstPath),
                          pixels().convertToFormat(QImage::Format_RGB32).copy(0, 30, 160, 210));
        }
        result->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(items(run.historyRoot).size(), failed ? 0 : 2);
        QFile metrics(run.metricsFile);
        QVERIFY(metrics.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(metrics.readLine())
                     .object()
                     .value(QStringLiteral("outcome"))
                     .toInt(),
                 scrollCompletedOutcome);
        QCOMPARE(QJsonDocument::fromJson(metrics.readLine())
                     .object()
                     .value(QStringLiteral("outcome"))
                     .toInt(),
                 scrollCompletedOutcome);
    }
    void failedUpdatePreservesPixelsAndCanRetry()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        auto actions = actionsFor(manager, manager.beginSession());
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        const auto output = temp.filePath(QStringLiteral("output.png"));
        QVERIFY(overlay.exportToPath(output));
        const auto stored = items(run.historyRoot).first();
        const QImage original(stored.absolutePath);
        OverlayScrollTestAccess::annotate(overlay);
        QVERIFY(!overlay.isSelectionSaved());
        QLockFile lock(QDir(run.historyRoot).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock());
        QVERIFY(overlay.exportToPath(output));
        QVERIFY(overlay.isSelectionSaved());
        comparePixels(QImage(stored.absolutePath), original);
        lock.unlock();
        QVERIFY(overlay.exportToPath(output));
        QCOMPARE(items(run.historyRoot).size(), 1);
        comparePixels(QImage(stored.absolutePath), QImage(output));
        QVERIFY(manager.error().isEmpty());
    }
    void scrollCancelledOrEmpty_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("cancel-with-frame") << true;
        QTest::newRow("empty-completion") << false;
    }
    void scrollCancelledOrEmpty()
    {
        QFETCH(bool, cancel);
        QTemporaryDir temp;
        const auto run = options(temp);
        ApplicationController controller(*qApp, run);
        auto frames = std::make_shared<Frames>();
        ApplicationControllerTestAccess::prepare(controller, frames);
        controller.trigger(QStringLiteral("history-scroll-test"));
        QTRY_VERIFY(ApplicationControllerTestAccess::overlay(controller));
        auto* overlay = ApplicationControllerTestAccess::overlay(controller);
        overlay->enterScrollReady(QRect(0, 0, 160, 240));
        button(*overlay, "scrollStartButton")->click();
        QVERIFY(frames->send);
        if (cancel)
        {
            frames->send(pixels());
            controller.quit();
        }
        else
            ApplicationControllerTestAccess::completeScroll(controller);
        QVERIFY(!controller.active());
        QVERIFY(!ApplicationControllerTestAccess::result(controller));
        QVERIFY(items(run.historyRoot).isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollResultOutputsDoNotWriteHistory()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        HistoryManager manager(AppSettings(run.settingsFile), run.historyRoot);
        QVERIFY(manager.initialize().success);
        const auto session = manager.beginSession();
        const auto recorded = manager.record(session, pixels(), HistorySource::Scroll);
        QVERIFY(recorded.success);
        ScrollResultActions actions;
        int outputs = 0, updates = 0;
        actions.copyImage = actions.pinImage = [&](const QImage&)
        {
            ++outputs;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        { return temp.filePath(QStringLiteral("scroll.png")); };
        actions.historyUpdated = [&](const QImage& image)
        {
            ++updates;
            manager.record(session, image, HistorySource::Scroll);
        };
        auto* window = new ScrollResultWindow(pixels(), {}, actions);
        window->show();
        button(*window, "scrollResultCopyButton")->click();
        button(*window, "scrollResultPinButton")->click();
        button(*window, "scrollResultSaveButton")->click();
        QCOMPARE(outputs, 2);
        QCOMPARE(updates, 0);
        QCOMPARE(items(run.historyRoot).size(), 1);
        comparePixels(QImage(recorded.item.absolutePath), pixels());
        window->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void nonCaptureOutputsNeverIngest()
    {
        QTemporaryDir temp;
        const auto run = options(temp);
        StickerActions actions;
        actions.copyImage = [](const QImage&) { return ImageOutputResult{true, {}}; };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        { return temp.filePath(QStringLiteral("sticker.png")); };
        ApplicationController controller(*qApp, run, actions, {[] { return pixels(); }});
        auto& manager = ApplicationControllerTestAccess::history(controller);
        const auto recorded =
            manager.record(manager.beginSession(), pixels(), HistorySource::Overlay);
        QVERIFY(recorded.success);
        // 后续历史回放使用相同 create 入口，不为贴图管理器安装入库钩子。
        QVERIFY(controller.stickers()
                    .create(QImage(recorded.item.absolutePath), QPoint(20, 20), true)
                    .success);
        auto* replay = controller.stickers().windows().first().data();
        QVERIFY(replay->saveImage());
        controller.menu()->findChild<QAction*>(QStringLiteral("clipboardStickerAction"))->trigger();
        QCOMPARE(controller.stickers().count(), 2);
        auto* clipboard = controller.stickers().windows().last().data();
        QVERIFY(clipboard->saveImage());
        clipboard->setEditing(true);
        button(*clipboard, "stickerEditCopyButton")->click();
        QCOMPARE(items(run.historyRoot).size(), 1);
        comparePixels(QImage(recorded.item.absolutePath), pixels());
    }
};
QTEST_MAIN(HistoryIngestTest)
#include "history_ingest_test.moc"
