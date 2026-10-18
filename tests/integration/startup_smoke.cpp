#include "app/app_settings.h"
#include "app/application_controller.h"
#include "app/hotkey_rules.h"
#include "app/sticker_manager.h"
#include "core/sticker_geometry.h"
#include "interfaces/tray_icon.h"
#include "output/annotation_renderer.h"
#include "session/monotonic_clock.h"
#include "session/session_metrics.h"
#include "ui/annotation_text_edit.h"
#include "ui/ocr_dialog.h"
#include "ui/selection_overlay.h"
#include "ui/settings_dialog.h"
#include "ui/text_recognition_task.h"
#include "ui/toolbar_icons.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QSignalSpy>
#include <QString>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTextEdit>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace waibusnap
{
struct OverlayScrollTestAccess
{
    static void panel(SelectionOverlay& overlay, bool open) { overlay.saveDialogOpen_ = open; }
    static void enable(SelectionOverlay& overlay) { overlay.scrollSupported_ = true; }
    static void clearSelection(SelectionOverlay& overlay) { overlay.setSelection({}); }
};
struct ScrollCaptureTestAccess
{
    static ScrollCaptureSession* create(RegionCaptureRequest request, QRect bar,
                                        std::unique_ptr<RegionCaptureSession> capture,
                                        std::unique_ptr<ScrollInjector> injector,
                                        std::function<QPoint()> cursor,
                                        ScrollStitchLimits limits = {}, QObject* parent = nullptr)
    {
        return new ScrollCaptureSession(request, bar, std::move(capture), std::move(injector),
                                        std::move(cursor), limits, parent);
    }
    static void tick(ScrollCaptureSession& session) { session.driveAutomatic(); }
    static void stopTimer(ScrollCaptureSession& session) { session.autoTimer_.stop(); }
    static void ageLastMotion(ScrollCaptureSession& session) { session.lastMotionMs_ = -3000; }
};
class RetryDisplays final : public DisplayTopology
{
  public:
    explicit RetryDisplays(DisplayTarget display) : display_(display) {}
    DisplayResult cursorDisplay() const override { return {display_, {}}; }
    bool stillMatches(const DisplayTarget&) const override { return true; }

  private:
    DisplayTarget display_;
};
class RetryCapture final : public CaptureProvider
{
  public:
    explicit RetryCapture(CaptureFrame frame) : frame_(std::move(frame)) {}
    bool hasPermission() const override { return true; }
    bool requestPermission() override { return true; }
    void capture(const DisplayTarget&, std::function<void(CaptureResult)> completion) override
    {
        const auto frame = frame_;
        QTimer::singleShot(0, QCoreApplication::instance(),
                           [frame, completion] { completion({frame, CaptureError::None, {}}); });
    }

  private:
    CaptureFrame frame_;
};
struct ApplicationControllerTestAccess
{
    static void seedSession(ApplicationController& controller, SelectionOverlay* overlay,
                            qint64 ageMs = 0)
    {
        controller.active_ = true;
        ++controller.token_;
        controller.metrics_ = {};
        controller.metrics_.t0 = monotonicNs() - ageMs * 1000000;
        controller.metrics_.trigger = QStringLiteral("smoke-state-injection");
        controller.overlay_ = overlay;
        controller.settingsAction_->setEnabled(false);
        controller.refreshStickerActions();
        controller.captureTimeout_.start(15000);
        controller.sessionWatchdog_.start();
    }
    static void retryDependencies(ApplicationController& controller, CaptureFrame frame)
    {
        controller.displays_ = std::make_unique<RetryDisplays>(frame.display);
        controller.capture_ = std::make_unique<RetryCapture>(std::move(frame));
        controller.overlayFactory_ =
            [](CaptureFrame input, OverlayActions actions, QVector<QRect> windows, QString error)
        {
            auto* overlay = new SelectionOverlay(std::move(input), std::move(actions),
                                                 std::move(windows), std::move(error));
            OverlayScrollTestAccess::enable(*overlay);
            return overlay;
        };
    }
    static QPointer<SelectionOverlay> overlay(ApplicationController& controller)
    {
        return controller.overlay_;
    }
    static void connectScroll(ApplicationController& controller, DisplayTarget display)
    {
        controller.bindScrollOverlay(display);
        QObject::connect(controller.overlay_, &SelectionOverlay::finished, &controller,
                         [&controller](QRect pixels, int outcome)
                         { controller.finish(pixels, outcome); });
    }
    static void
    factory(ApplicationController& controller,
            std::function<ScrollCaptureSession*(RegionCaptureRequest, QRect, QObject*)> factory)
    {
        controller.scrollFactory_ = std::move(factory);
    }
    static void visibilityState(ApplicationController& controller, bool available, bool onScreen)
    {
        controller.visibilityQuery_ = [available, onScreen](QWindow*)
        { return WindowVisibility{available, onScreen, 67471, 0, 0, 25, 0}; };
    }
    static void visibility(ApplicationController& controller, bool onScreen)
    {
        controller.visibilityQuery_ = [onScreen](QWindow*)
        { return WindowVisibility{true, onScreen, 67471, 0, 0, 25, 0}; };
    }
    static void ageInvisible(ApplicationController& controller, qint64 ms)
    {
        controller.invisibleSinceNs_ = monotonicNs() - ms * 1000000;
    }
    static void recovery(ApplicationController& controller, std::function<bool(QWindow*, bool)> fn)
    {
        controller.visibilityRecovery_ = std::move(fn);
    }
    static void presentation(ApplicationController& controller)
    {
        controller.presentationCompleted(controller.token_, {monotonicNs(), 16.67, true});
    }
    static bool captureTimeoutActive(ApplicationController& controller)
    {
        return controller.captureTimeout_.isActive();
    }
    static void eventRecovery(ApplicationController& controller)
    {
        controller.recoverOverlayWindow(controller.overlay_, true, QStringLiteral("测试系统事件"));
    }
    static void watchdogCheck(ApplicationController& controller)
    {
        controller.checkSessionWatchdog();
    }
    static bool readyGuard(const ApplicationController& controller)
    {
        return controller.scrollFlowActive_ && !controller.captureTimeout_.isActive();
    }
    static QPointer<ScrollResultWindow> result(ApplicationController& controller)
    {
        return controller.scrollResults_.isEmpty() ? nullptr : controller.scrollResults_.last();
    }
    static QPointer<ScrollControlBar> bar(ApplicationController& controller)
    {
        return controller.scrollBar_;
    }
    static void cancelNormal(ApplicationController& controller) { controller.finish({}, 2); }
    static void cancel(ApplicationController& controller)
    {
        controller.finish({}, scrollCancelledOutcome);
    }
    static void changed(ApplicationController& controller) { controller.layoutChanged(); }
    static QRect retryRegion(const ApplicationController& controller)
    {
        return controller.retryRegion_;
    }
    static void fail(ApplicationController& controller)
    {
        controller.fail(8, QStringLiteral("测试注入：未观察到可交互覆盖窗口。"));
    }
    static QTimer& watchdog(ApplicationController& controller)
    {
        return controller.sessionWatchdog_;
    }
    static bool timersStopped(const ApplicationController& controller)
    {
        return !controller.captureTimeout_.isActive() && !controller.sessionWatchdog_.isActive();
    }
    static void observe(ApplicationController& controller)
    {
        controller.observation_ = std::make_unique<PresentationObservation>();
    }
    static bool observationCleared(const ApplicationController& controller)
    {
        return !controller.observation_;
    }
    static quint64 token(const ApplicationController& controller) { return controller.token_; }
    static void staleCapture(ApplicationController& controller, quint64 token)
    {
        controller.captureCompleted(token, {{}, CaptureError::Unavailable, QStringLiteral("迟到")});
    }
};
}

namespace
{
inline constexpr std::array<int, 3> sampleLineWidths = {2, 4, 8};

void annotationWheel(QWidget& canvas, int angle, QPoint position = {130, 130}, int pixels = 0,
                     Qt::ScrollPhase phase = Qt::NoScrollPhase)
{
    QWheelEvent event(position, canvas.mapToGlobal(position), {0, pixels}, {0, angle}, Qt::NoButton,
                      Qt::NoModifier, phase, false);
    QApplication::sendEvent(&canvas, &event);
}
void chooseAnnotationColor(QWidget& canvas, int index, bool sticker = false)
{
    const QString name =
        (sticker ? QStringLiteral("stickerEditColor") : QStringLiteral("annotationColor")) +
        QString::number(index) + QStringLiteral("Button");
    auto* button = canvas.findChild<QPushButton*>(name);
    QVERIFY(button);
    QTest::mouseClick(button, Qt::LeftButton);
}
void chooseAnnotationWidth(QWidget& canvas, int width, bool sticker = false)
{
    auto* label = canvas.findChild<QLabel*>(sticker ? QStringLiteral("stickerEditWidthLabel")
                                                    : QStringLiteral("annotationWidthLabel"));
    QVERIFY(label);
    const int previous = label->text().section(' ', 1).chopped(2).toInt();
    annotationWheel(canvas, (width - previous) * 120);
    QCOMPARE(label->text(), QStringLiteral("线宽 %1px").arg(width));
}
waibusnap::TextRecognitionResult sampleOcrResult()
{
    return {true,
            {},
            {{QStringLiteral("Hello, world!  "),
              {20, 20, 500, 40},
              {{"Hello", {20, 20, 80, 40}, 0, 5}, {"world", {130, 20, 100, 40}, 7, 5}}},
             {QString::fromUtf8("你好，😀 OCR。"),
              {20, 100, 500, 40},
              {{QStringLiteral("你好"), {20, 100, 80, 40}, 0, 2},
               {QString::fromUtf8("😀"), {130, 100, 30, 40}, 3, 2},
               {"OCR", {190, 100, 80, 40}, 6, 3}}}}};
}
class OcrStub final : public waibusnap::TextRecognizer
{
  public:
    mutable std::atomic<int> calls{0};
    mutable std::atomic<int> completions{0};
    mutable std::atomic<bool> worker{false};
    int delayMs = 0;
    bool firstOnlyDelay = false;
    QVector<waibusnap::TextRecognitionResult> results{sampleOcrResult()};
    waibusnap::TextRecognitionResult recognize(const QImage& image) const override
    {
        const int index = calls.fetch_add(1);
        worker = QThread::currentThread() != qApp->thread();
        {
            QMutexLocker lock(&mutex_);
            input_ = image;
        }
        if (!firstOnlyDelay || index == 0)
            QThread::msleep(delayMs);
        ++completions;
        return results[std::min(index, int(results.size()) - 1)];
    }
    QImage input() const
    {
        QMutexLocker lock(&mutex_);
        return input_;
    }

  private:
    mutable QMutex mutex_;
    mutable QImage input_;
};

struct AutostartStub
{
    waibusnap::AutostartState state;
    QString error;
    int queries = 0;
    QVector<bool> requests;
    waibusnap::AutostartActions actions()
    {
        return {[this]
                {
                    ++queries;
                    return state;
                },
                [this](bool enabled)
                {
                    requests.append(enabled);
                    if (error.isEmpty())
                        state = {enabled, false, {}};
                    return error;
                }};
    }
};
const QString clipboardToastText =
    QStringLiteral("剪贴板中没有可用图片，请先复制图片后再试；剪贴板内容未被修改。");
QLabel* clipboardToast()
{
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("clipboardStickerToast"))
            return qobject_cast<QLabel*>(widget);
    return nullptr;
}
QAction* clipboardAction(waibusnap::ApplicationController& controller)
{
    return controller.menu()->findChild<QAction*>(QStringLiteral("clipboardStickerAction"));
}
void compareImagePixels(const QImage& actual, const QImage& expected)
{
    QCOMPARE(actual.size(), expected.size());
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x)
            QCOMPARE(actual.pixelColor(x, y), expected.pixelColor(x, y));
}
class ClipboardRestore final
{
  public:
    ClipboardRestore()
    {
        const auto* original = QGuiApplication::clipboard()->mimeData();
        if (original)
            for (const QString& format : original->formats())
                saved_->setData(format, original->data(format));
    }
    ~ClipboardRestore() { QGuiApplication::clipboard()->setMimeData(saved_.release()); }

  private:
    std::unique_ptr<QMimeData> saved_ = std::make_unique<QMimeData>();
};
struct RegionStubState
{
    std::function<void(QImage)> frame;
    std::function<void(QString)> error;
    int starts = 0;
    int stops = 0;
};
class RegionStub final : public waibusnap::RegionCaptureSession
{
  public:
    explicit RegionStub(std::shared_ptr<RegionStubState> state) : state_(std::move(state)) {}
    void start(std::function<void(QImage)> frame, std::function<void(QString)> error) override
    {
        ++state_->starts;
        state_->frame = std::move(frame);
        state_->error = std::move(error);
    }
    void stop() override { ++state_->stops; }

  private:
    std::shared_ptr<RegionStubState> state_;
};
struct InjectorStubState
{
    bool authorized = false;
    int requested = 0;
    QVector<waibusnap::ScrollInjection> posts;
};
class InjectorStub final : public waibusnap::ScrollInjector
{
  public:
    explicit InjectorStub(std::shared_ptr<InjectorStubState> state) : state_(std::move(state)) {}
    bool hasAccessibilityPermission() const override { return state_->authorized; }
    void requestAccessibilityPermission() override { ++state_->requested; }
    bool postScroll(waibusnap::ScrollInjection event) override
    {
        if (!state_->authorized)
            return false;
        state_->posts.append(event);
        return true;
    }

  private:
    std::shared_ptr<InjectorStubState> state_;
};
QImage scrollDocument(int width = 320, int height = 1600)
{
    QImage image(width, height, QImage::Format_RGB32);
    quint32 random = 42;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            random = random * 1664525 + 1013904223;
            image.setPixel(x, y, qRgb(random >> 24, (random >> 16) & 255, (random >> 8) & 255));
        }
    return image;
}
QPushButton* scrollButton(QWidget* window, const char* name)
{
    return window->findChild<QPushButton*>(QString::fromLatin1(name));
}
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
waibusnap::CaptureFrame annotationFrame()
{
    auto frame = sampleFrame();
    frame.display.logicalGeometry.setSize({800, 600});
    frame.pixels = QImage(1600, 1200, QImage::Format_ARGB32_Premultiplied);
    frame.pixels.fill(Qt::white);
    frame.pixels.setDevicePixelRatio(2);
    return frame;
}
QPushButton* button(waibusnap::SelectionOverlay& overlay, const char* name)
{
    return overlay.findChild<QPushButton*>(QString::fromLatin1(name));
}
// 沿真实一级入口进入组，再选择具体子工具；再次选择当前工具通过一级按钮取消。
void clickAnnotationTool(QWidget& editor, const char* name)
{
    using namespace waibusnap;
    const QString requested = QString::fromLatin1(name);
    auto* target = editor.findChild<QPushButton*>(requested);
    QVERIFY(target);
    if (!target->property("annotationType").isValid())
    {
        QTest::mouseClick(target, Qt::LeftButton);
        return;
    }
    const auto type = static_cast<AnnotationType>(target->property("annotationType").toInt());
    QString primaryName = annotationGroupObject(annotationToolSlot(type));
    if (requested.startsWith(QStringLiteral("stickerEdit")))
    {
        primaryName[0] = primaryName[0].toUpper();
        primaryName.prepend(QStringLiteral("stickerEdit"));
    }
    auto* primary = editor.findChild<QPushButton*>(primaryName);
    QVERIFY(primary);
    if (target->isChecked())
        QTest::mouseClick(primary, Qt::LeftButton);
    else
    {
        if (!primary->isChecked())
            QTest::mouseClick(primary, Qt::LeftButton);
        if (!target->isChecked())
            QTest::mouseClick(target, Qt::LeftButton);
    }
}
void standardKey(QWidget* widget, QKeySequence::StandardKey key)
{
    const auto combination = QKeySequence(key)[0];
    QTest::keyClick(widget, combination.key(), combination.keyboardModifiers());
}
void drag(waibusnap::SelectionOverlay& overlay, QPoint first, QPoint second)
{
    QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(&overlay, second);
    QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, second);
}
// 直接投递逻辑坐标事件，避免 QTest 鼠标移动被实际桌面边界 / 负坐标副屏夹取。
void dragLogical(waibusnap::SelectionOverlay& overlay, QPoint first, QPoint last)
{
    const QPoint positions[] = {first, last, last};
    const QEvent::Type types[] = {QEvent::MouseButtonPress, QEvent::MouseMove,
                                  QEvent::MouseButtonRelease};
    for (int index = 0; index < 3; ++index)
    {
        QMouseEvent event(types[index], positions[index], overlay.mapToGlobal(positions[index]),
                          index == 1 ? Qt::NoButton : Qt::LeftButton,
                          index == 2 ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &event);
    }
}
QPointF selectionCenter(QRect rect)
{
    return {rect.x() + rect.width() / 2.0, rect.y() + rect.height() / 2.0};
}
void verifySelectionRatio(QRect rect, waibusnap::AspectRatio ratio, QSize bounds)
{
    QVERIFY(!rect.isEmpty());
    QVERIFY(QRect(QPoint(), bounds).contains(rect));
    const double r = ratio.width / ratio.height;
    QVERIFY(std::min(std::abs(rect.width() - rect.height() * r),
                     std::abs(rect.height() - rect.width() / r)) <= 1.0 + 1e-8);
}
void ratioMouse(waibusnap::SelectionOverlay& overlay, QEvent::Type type, QPoint position,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, overlay.mapToGlobal(position),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers);
    QApplication::sendEvent(&overlay, &event);
}
void stickerClick(waibusnap::StickerWindow& sticker, const char* name)
{
    auto* control = sticker.findChild<QPushButton*>(QString::fromLatin1(name));
    QVERIFY(control);
    clickAnnotationTool(sticker, name);
}
void stickerDrag(waibusnap::StickerWindow& sticker, QPoint first, QPoint last)
{
    const QPointF origin = sticker.mapToGlobal(QPoint(0, 0));
    const QPointF positions[] = {origin + first, origin + last, origin + last};
    const QEvent::Type types[] = {QEvent::MouseButtonPress, QEvent::MouseMove,
                                  QEvent::MouseButtonRelease};
    for (int index = 0; index < 3; ++index)
    {
        QMouseEvent event(types[index], sticker.mapFromGlobal(positions[index]), positions[index],
                          index == 1 ? Qt::NoButton : Qt::LeftButton,
                          index == 2 ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&sticker, &event);
    }
}
void compareAnnotations(const QVector<waibusnap::Annotation>& actual,
                        const QVector<waibusnap::Annotation>& expected)
{
    QCOMPARE(actual.size(), expected.size());
    for (qsizetype index = 0; index < expected.size(); ++index)
    {
        const auto& a = actual[index];
        const auto& b = expected[index];
        QCOMPARE(a.type, b.type);
        QCOMPARE(a.first, b.first);
        QCOMPARE(a.last, b.last);
        QCOMPARE(a.points, b.points);
        QCOMPARE(a.text, b.text);
        QCOMPARE(a.style.color, b.style.color);
        QCOMPARE(a.style.lineWidth, b.style.lineWidth);
        QCOMPARE(a.style.textSize, b.style.textSize);
        QCOMPARE(a.style.fontFamily, b.style.fontFamily);
    }
}
void compareCoveredPixels(const QImage& actual, const QImage& before, QRect cover, QColor color)
{
    QCOMPARE(actual.size(), before.size());
    QCOMPARE(actual.devicePixelRatio(), qreal(1));
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x)
            QCOMPARE(actual.pixelColor(x, y),
                     cover.contains(x, y) ? color : before.pixelColor(x, y));
}
QImage mosaicTestImage(QSize size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256));
    return image;
}
void compareMosaicPixels(const QImage& actual, const QImage& before, QRect region)
{
    QCOMPARE(actual.size(), before.size());
    QCOMPARE(actual.devicePixelRatio(), qreal(1));
    const int block =
        std::clamp(qRound(qreal(std::min(region.width(), region.height())) / 12), 4, 48);
    bool changed = false, distinctBlocks = false;
    const QColor first = actual.pixelColor(region.topLeft());
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x)
        {
            if (!region.contains(x, y))
            {
                QCOMPARE(actual.pixelColor(x, y), before.pixelColor(x, y));
                continue;
            }
            const QPoint origin(region.x() + (x - region.x()) / block * block,
                                region.y() + (y - region.y()) / block * block);
            QCOMPARE(actual.pixelColor(x, y), actual.pixelColor(origin));
            QCOMPARE(actual.pixelColor(x, y).alpha(), 255);
            changed |= actual.pixelColor(x, y) != before.pixelColor(x, y);
            distinctBlocks |= actual.pixelColor(x, y) != first;
        }
    QVERIFY(changed);
    QVERIFY(distinctBlocks);
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
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); }
    void scrollEntryPreparation_data()
    {
        QTest::addColumn<int>("state");
        QTest::newRow("empty") << 0;
        QTest::newRow("small") << 1;
        QTest::newRow("normal") << 2;
        QTest::newRow("annotations") << 3;
        QTest::newRow("pending-text") << 4;
    }
    void scrollEntryPreparation()
    {
        using namespace waibusnap;
        QFETCH(int, state);
        SelectionOverlay overlay(annotationFrame());
        OverlayScrollTestAccess::enable(overlay);
        overlay.show();
        dragLogical(overlay, {100, 100}, state == 1 ? QPoint(120, 120) : QPoint(300, 250));
        auto* longImage = button(overlay, "longImageButton");
        QVERIFY(longImage && longImage->isEnabled() && !longImage->icon().isNull());
        QCOMPARE(longImage->size(), QSize(28, 28));
        QVERIFY(longImage->x() > button(overlay, "ocrToolButton")->x());
        if (state == 0)
            OverlayScrollTestAccess::clearSelection(overlay);
        if (state == 3)
        {
            button(overlay, "rectangleToolButton")->click();
            dragLogical(overlay, {130, 130}, {180, 170});
            QVERIFY(!overlay.annotations().isEmpty());
        }
        if (state == 4)
        {
            button(overlay, "textToolButton")->click();
            QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {130, 130});
            auto* editor = overlay.findChild<AnnotationTextEdit*>();
            QVERIFY(editor && editor->isVisible());
            editor->setPlainText(QStringLiteral("待提交文字"));
        }
        QSignalSpy ready(&overlay, &SelectionOverlay::scrollReadyChanged);
        longImage->click();
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        if (state < 2)
        {
            QVERIFY(!overlay.isScrollReady());
            QVERIFY(status->isVisible());
            QCOMPARE(status->text(),
                     state == 0 ? QStringLiteral("请先框选区域再截长图。")
                                : QStringLiteral("区域太小，请重新框选（至少 64 × 64 像素）。"));
            QCOMPARE(ready.size(), 0);
        }
        else
        {
            QVERIFY(overlay.isScrollReady());
            QCOMPARE(ready.size(), 1);
            QVERIFY(overlay.annotations().isEmpty());
            QVERIFY(!overlay.magnifierVisible());
            QVERIFY(overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"))->isHidden());
            auto* bar = overlay.findChild<QWidget*>(QStringLiteral("scrollReadyBar"));
            QVERIFY(bar && bar->isVisible());
            QVERIFY(overlay.rect().contains(bar->geometry()));
            QCOMPARE(overlay.findChild<QLabel*>(QStringLiteral("scrollReadyTitleLabel"))->text(),
                     QStringLiteral("截长图：选区已锁定"));
            QCOMPARE(overlay.findChild<QLabel*>(QStringLiteral("scrollReadySizeLabel"))->text(),
                     QStringLiteral("400 × 300 像素"));
            if (state >= 3)
                QCOMPARE(status->text(), QStringLiteral("已清除标注，长图结果不含标注。"));
        }
    }
    void scrollReadyTransitions_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("back") << 0;
        QTest::newRow("cancel") << 1;
        QTest::newRow("escape") << 2;
        QTest::newRow("start") << 3;
        QTest::newRow("enter") << 4;
    }
    void scrollReadyTransitions()
    {
        using namespace waibusnap;
        QFETCH(int, action);
        SelectionOverlay overlay(annotationFrame());
        OverlayScrollTestAccess::enable(overlay);
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        const auto rectangle = overlay.selection();
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        QSignalSpy requested(&overlay, &SelectionOverlay::scrollCaptureRequested);
        overlay.enterScrollReady();
        dragLogical(overlay, {150, 150}, {450, 350});
        QTest::keyClick(&overlay, Qt::Key_Right);
        standardKey(&overlay, QKeySequence::Copy);
        QCOMPARE(overlay.selection(), rectangle);
        QCOMPARE(finished.size(), 0);
        const char* names[] = {"scrollBackButton", "scrollReadyCancelButton", "",
                               "scrollStartButton"};
        if (action == 2)
            QTest::keyClick(&overlay, Qt::Key_Escape);
        else if (action == 4)
            QTest::keyClick(&overlay, Qt::Key_Return);
        else
            button(overlay, names[action])->click();
        if (action == 0)
        {
            QVERIFY(!overlay.isScrollReady());
            QVERIFY(overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"))->isVisible());
            QCOMPARE(finished.size(), 0);
        }
        else if (action < 3)
        {
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(1).toInt(), scrollCancelledOutcome);
            QCOMPARE(finished.first().at(0).toRect(), QRect());
        }
        else
        {
            QCOMPARE(requested.size(), 1);
            QCOMPARE(requested.first().first().toRect(), rectangle);
            QCOMPARE(finished.size(), 0);
        }
    }
    void scrollPlatformAndMenu()
    {
        using namespace waibusnap;
        ApplicationController controller(*qApp, {});
        auto* action =
            controller.menu()->findChild<QAction*>(QStringLiteral("scrollCaptureAction"));
        QVERIFY(action);
        QCOMPARE(controller.menu()->actions().indexOf(action), 1);
        QCOMPARE(action->isEnabled(), regionCaptureSupported());
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        QCOMPARE(button(overlay, "longImageButton")->isEnabled(), regionCaptureSupported());
        if (!regionCaptureSupported())
            QCOMPARE(button(overlay, "longImageButton")->toolTip(),
                     QStringLiteral("截长图暂不支持当前平台（Windows 后补）。"));
    }
    void scrollControlPlacement_data()
    {
        QTest::addColumn<QRect>("region");
        QTest::newRow("below") << QRect(100, 100, 500, 200);
        QTest::newRow("above") << QRect(100, 450, 500, 100);
        QTest::newRow("inside") << QRect(0, 24, 800, 552);
        QTest::newRow("narrow") << QRect(0, 24, 320, 552);
    }
    void scrollControlPlacement()
    {
        using namespace waibusnap;
        QFETCH(QRect, region);
        const QRect available(0, 24, region.width() == 320 ? 320 : 800, 552);
        ScrollControlBar bar(region, available);
        ScrollBorderWindow border(region);
        bar.show();
        border.show();
        QVERIFY(available.contains(bar.geometry()));
        QVERIFY(border.windowFlags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(border.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QCOMPARE(border.geometry(), region.adjusted(-2, -2, 2, 2));
        ScrollStitchStats stats;
        stats.lengthPx = 1200;
        stats.frames = 18;
        bar.setStatistics(stats, 20000);
        auto* label = bar.findChild<QLabel*>(QStringLiteral("scrollStatusLabel"));
        QCOMPARE(label->toolTip(), QStringLiteral("已拼接 1200 逻辑 px · 18 帧"));
        QVERIFY(scrollButton(&bar, "scrollSpeedButton")->isHidden());
        bar.setAutomatic(true);
        for (auto* control : bar.findChildren<QPushButton*>())
            if (control->isVisible())
                QVERIFY(bar.rect().contains(control->geometry()));
        QVERIFY(scrollButton(&bar, "scrollSpeedButton")->isVisible());
        QVERIFY(available.contains(bar.geometry()));
        QSignalSpy finish(&bar, &ScrollControlBar::finishRequested);
        QSignalSpy cancel(&bar, &ScrollControlBar::cancelRequested);
        QTest::keyClick(&bar, Qt::Key_Return);
        QTest::keyClick(&bar, Qt::Key_Escape);
        QCOMPARE(finish.size(), 1);
        QCOMPARE(cancel.size(), 1);
        bar.setNotice(QStringLiteral("自动滚动暂挂：请将鼠标移到采集区域内。"));
        QVERIFY(label->toolTip().startsWith(QStringLiteral("自动滚动暂挂")));
    }
    void scrollAutomaticPermissionAndSuspension()
    {
        using namespace waibusnap;
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        QPoint cursor(50, 50);
        RegionCaptureRequest request{{0, {0, 0, 800, 600}, 2, 0, {}}, {0, 0, 320, 400}, {}};
        std::unique_ptr<ScrollCaptureSession> session(ScrollCaptureTestAccess::create(
            request, {0, 150, 160, 40}, std::make_unique<RegionStub>(capture),
            std::make_unique<InjectorStub>(injector), [&] { return cursor; }));
        QSignalSpy permission(session.get(), &ScrollCaptureSession::permissionMissing);
        QSignalSpy notices(session.get(), &ScrollCaptureSession::noticeChanged);
        session->start();
        const auto source = scrollDocument();
        capture->frame(source.copy(0, 0, 320, 400));
        session->setAutomatic(true);
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(permission.size(), 1);
        QCOMPARE(injector->posts.size(), 0);
        QVERIFY(!session->automatic());
        session->openAccessibilitySettings();
        QCOMPARE(injector->requested, 1);
        injector->authorized = true;
        session->setAutomatic(true);
        ScrollCaptureTestAccess::stopTimer(*session);
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(injector->posts.size(), 1);
        QCOMPARE(injector->posts.last().dy, -60);
        capture->frame(source.copy(0, 60, 320, 400));
        cursor = {200, 50};
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(injector->posts.size(), 1);
        QVERIFY(notices.last().first().toString().startsWith(QStringLiteral("自动滚动暂挂")));
        cursor = {50, 160};
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(injector->posts.size(), 1);
        cursor = {50, 50};
        session->cycleSpeed();
        session->cycleDirection();
        session->cycleDirection();
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(injector->posts.last().dx, -120);
        QCOMPARE(injector->posts.last().dy, 0);
        injector->authorized = false;
        const auto count = injector->posts.size();
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(injector->posts.size(), count);
        QVERIFY(!session->automatic());
        session->stop();
        const int frames = session->stats().frames;
        capture->frame(source.copy(0, 120, 320, 400));
        capture->error(QStringLiteral("迟到错误"));
        QCOMPARE(session->stats().frames, frames);
    }
    void scrollAutomaticProbeAndBottom()
    {
        using namespace waibusnap;
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        injector->authorized = true;
        RegionCaptureRequest request{{0, {0, 0, 800, 600}, 1, 0, {}}, {0, 0, 320, 400}, {}};
        std::unique_ptr<ScrollCaptureSession> session(ScrollCaptureTestAccess::create(
            request, {}, std::make_unique<RegionStub>(capture),
            std::make_unique<InjectorStub>(injector), [] { return QPoint(50, 50); }));
        QSignalSpy completed(session.get(), &ScrollCaptureSession::completed);
        QSignalSpy notices(session.get(), &ScrollCaptureSession::noticeChanged);
        session->start();
        const auto source = scrollDocument();
        const auto first = source.copy(0, 0, 320, 400);
        capture->frame(first);
        session->setAutomatic(true);
        ScrollCaptureTestAccess::stopTimer(*session);
        for (int index = 0; index < 11; ++index)
        {
            ScrollCaptureTestAccess::tick(*session);
            capture->frame(first);
        }
        QCOMPARE(completed.size(), 0);
        QVERIFY(injector->posts.at(5).dy > 0);
        bool noMotion = false;
        for (const auto& notice : notices)
            noMotion |= notice.first().toString().startsWith(QStringLiteral("未检测到滚动"));
        QVERIFY(noMotion);
        capture->frame(source.copy(0, 60, 320, 400));
        ScrollCaptureTestAccess::tick(*session);
        capture->frame(source.copy(0, 60, 320, 400));
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(completed.size(), 0);
        capture->frame(source.copy(0, 60, 320, 400));
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(completed.size(), 1);
        QCOMPARE(session->takeResult(), source.copy(0, 0, 320, 460));
    }
    void scrollManualBottomAndLimit_data()
    {
        QTest::addColumn<bool>("limited");
        QTest::newRow("manual-bottom") << false;
        QTest::newRow("length-limit") << true;
    }
    void scrollManualBottomAndLimit()
    {
        using namespace waibusnap;
        QFETCH(bool, limited);
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        std::unique_ptr<ScrollCaptureSession> session(ScrollCaptureTestAccess::create(
            {}, {}, std::make_unique<RegionStub>(capture), std::make_unique<InjectorStub>(injector),
            [] { return QPoint(); }, {limited ? 450 : 20000, 30000000}));
        QSignalSpy completed(session.get(), &ScrollCaptureSession::completed);
        QSignalSpy notices(session.get(), &ScrollCaptureSession::noticeChanged);
        session->start();
        const auto source = scrollDocument();
        capture->frame(source.copy(0, 0, 320, 400));
        capture->frame(source.copy(0, 60, 320, 400));
        if (limited)
        {
            QCOMPARE(completed.size(), 1);
            QCOMPARE(session->stats().lengthPx, 450);
            QVERIFY(notices.last().first().toString().startsWith(QStringLiteral("已达长度上限")));
        }
        else
        {
            ScrollCaptureTestAccess::ageLastMotion(*session);
            capture->frame(source.copy(0, 60, 320, 400));
            QCOMPARE(completed.size(), 0);
            QVERIFY(
                notices.last().first().toString().startsWith(QStringLiteral("似乎已滚动到底部")));
            capture->frame(source.copy(0, 120, 320, 400));
            QCOMPARE(notices.last().first().toString(), QString());
        }
    }
    void scrollResultPreviewCropOutput_data()
    {
        QTest::addColumn<bool>("horizontal");
        QTest::newRow("vertical") << false;
        QTest::newRow("horizontal") << true;
    }
    void scrollResultPreviewCropOutput()
    {
        using namespace waibusnap;
        QFETCH(bool, horizontal);
        const QImage image = scrollDocument(horizontal ? 1200 : 320, horizontal ? 320 : 1200);
        ScrollStitchStats stats;
        stats.warnings = 2;
        ScrollResultActions actions;
        QImage copied;
        actions.copyImage = [&](const QImage& result)
        {
            copied = result;
            return ImageOutputResult{true, {}};
        };
        actions.confirmDiscard = [](QWidget*) { return true; };
        auto* window = new ScrollResultWindow(image, stats, actions);
        window->show();
        QTest::qWait(1);
        auto* warning = window->findChild<QWidget*>(QStringLiteral("scrollResultWarningBar"));
        QVERIFY(warning->isVisible());
        QVERIFY(window->findChild<QLabel*>(QStringLiteral("scrollResultStatusLabel"))
                    ->text()
                    .contains(horizontal ? QStringLiteral("1200 × 320")
                                         : QStringLiteral("320 × 1200")));
        QVERIFY(window->zoom() <= 1);
        scrollButton(window, "scrollResultZoom100Button")->click();
        QCOMPARE(window->zoom(), qreal(1));
        auto* view = window->findChild<QWidget*>(QStringLiteral("scrollResultView"));
        QWheelEvent zoomIn(QPointF(50, 50), view->mapToGlobal(QPoint(50, 50)), {}, {0, 9600},
                           Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &zoomIn);
        QCOMPARE(window->zoom(), qreal(4));
        scrollButton(window, "scrollResultZoom100Button")->click();
        scrollButton(window, "scrollResultCropButton")->click();
        const QPoint start = horizontal ? QPoint(1, 100) : QPoint(100, 1);
        const QPoint end = horizontal ? QPoint(50, 100) : QPoint(100, 50);
        const auto send =
            [&](QEvent::Type type, QPoint point, Qt::MouseButton changed, Qt::MouseButtons held)
        {
            QMouseEvent event(type, point, view->mapToGlobal(point), changed, held, Qt::NoModifier);
            QApplication::sendEvent(view, &event);
        };
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        QVERIFY(window->findChild<QLabel*>(QStringLiteral("scrollResultStatusLabel"))
                    ->text()
                    .contains(horizontal ? QStringLiteral("1150 × 320")
                                         : QStringLiteral("320 × 1150")));
        scrollButton(window, "scrollResultCropResetButton")->click();
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        scrollButton(window, "scrollResultCropApplyButton")->click();
        QCOMPARE(window->image(),
                 horizontal ? image.copy(50, 0, 1150, 320) : image.copy(0, 50, 320, 1150));
        scrollButton(window, "scrollResultCopyButton")->click();
        QCOMPARE(copied, window->image());
        QVERIFY(window->hasOutput());
        QVERIFY(window->isVisible());
        QSignalSpy retry(window, &ScrollResultWindow::retryRequested);
        scrollButton(window, "scrollResultRetryButton")->click();
        QCOMPARE(retry.size(), 1);
        window->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollResultFullOverview_data()
    {
        QTest::addColumn<bool>("horizontal");
        QTest::newRow("vertical-20000") << false;
        QTest::newRow("horizontal-20000") << true;
    }
    void scrollResultFullOverview()
    {
        using namespace waibusnap;
        QFETCH(bool, horizontal);
        QImage image(horizontal ? QSize(20000, 320) : QSize(320, 20000), QImage::Format_RGB32);
        image.fill(Qt::white);
        auto* window = new ScrollResultWindow(image, {});
        window->show();
        QTest::qWait(1);
        auto* view = window->findChild<QWidget*>(QStringLiteral("scrollResultView"));
        auto* area = window->findChild<QScrollArea*>();
        scrollButton(window, "scrollResultFitButton")->click();
        QVERIFY(window->zoom() < 0.1);
        QVERIFY(view->width() <= area->viewport()->width());
        QVERIFY(view->height() <= area->viewport()->height());
        QTest::mouseDClick(view, Qt::LeftButton, Qt::NoModifier, {10, 10});
        QCOMPARE(window->zoom(), qreal(1));
        QWheelEvent zoomOut(QPointF(10, 10), view->mapToGlobal(QPoint(10, 10)), {}, {0, -9600},
                            Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &zoomOut);
        QCOMPARE(window->zoom(), qreal(0.1));
        window->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollResultCloseConfirmation()
    {
        using namespace waibusnap;
        auto* window = new ScrollResultWindow(scrollDocument(), {});
        window->show();
        bool observed = false;
        QTimer::singleShot(0,
                           [&, window]
                           {
                               auto* dialog =
                                   qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                               observed =
                                   dialog && dialog->text() == QStringLiteral("放弃这张长图？");
                               if (dialog)
                                   dialog->button(QMessageBox::Cancel);
                               if (dialog)
                                   dialog->reject();
                               Q_UNUSED(window);
                           });
        QVERIFY(!window->close());
        QVERIFY(observed && window->isVisible());
        window->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollControllerLifecycle_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("complete") << 0;
        QTest::newRow("cancel") << 1;
        QTest::newRow("layout-interruption") << 2;
        QTest::newRow("error-interruption") << 3;
    }
    void scrollControllerLifecycle()
    {
        using namespace waibusnap;
        QFETCH(int, ending);
        QTemporaryDir temp;
        RunOptions options;
        options.metricsFile = temp.filePath(QStringLiteral("metrics.jsonl"));
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        ApplicationController controller(*qApp, options);
        ApplicationControllerTestAccess::factory(
            controller,
            [=](RegionCaptureRequest request, QRect bar, QObject* parent)
            {
                return ScrollCaptureTestAccess::create(
                    request, bar, std::make_unique<RegionStub>(capture),
                    std::make_unique<InjectorStub>(injector), [] { return QPoint(50, 50); }, {},
                    parent);
            });
        const auto frame = annotationFrame();
        auto* overlay = new SelectionOverlay(frame);
        OverlayScrollTestAccess::enable(*overlay);
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::connectScroll(controller, frame.display);
        overlay->enterScrollReady({200, 200, 320, 400});
        QVERIFY(ApplicationControllerTestAccess::readyGuard(controller));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QVERIFY(controller.active());
        scrollButton(overlay, "scrollStartButton")->click();
        QVERIFY(!overlay->isVisible());
        const auto bar = ApplicationControllerTestAccess::bar(controller);
        QVERIFY(bar && bar->isVisible());
        const auto source = scrollDocument();
        capture->frame(source.copy(0, 0, 320, 400));
        capture->frame(source.copy(0, 60, 320, 400));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QVERIFY(controller.active());
        if (ending < 2)
            scrollButton(bar, ending == 0 ? "scrollFinishButton" : "scrollCancelButton")->click();
        else if (ending == 2)
            ApplicationControllerTestAccess::changed(controller);
        else
            capture->error(QStringLiteral("假采集器中断"));
        QVERIFY(!controller.active());
        QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
        auto result = ApplicationControllerTestAccess::result(controller);
        QCOMPARE(bool(result), ending != 1);
        if (result)
        {
            QCOMPARE(result->image(), source.copy(0, 0, 320, 460));
            QCOMPARE(
                result->findChild<QWidget*>(QStringLiteral("scrollResultWarningBar"))->isVisible(),
                ending >= 2);
            result->closeWithoutConfirmation();
        }
        QFile metrics(options.metricsFile);
        QVERIFY(metrics.open(QIODevice::ReadOnly));
        const auto record = QJsonDocument::fromJson(metrics.readLine()).object();
        QCOMPARE(record.value(QStringLiteral("outcome")).toInt(), ending == 1 ? 14 : 13);
        QCOMPARE(record.value(QStringLiteral("selection_height_px")).toInt(),
                 ending == 1 ? 0 : 460);
        capture->frame(source.copy(0, 120, 320, 400));
        QVERIFY(!controller.active());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollReadyDprAndClamping_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<int>("width");
        for (qreal dpr : {1.0, 1.5, 2.0})
            for (int width : {320, 480, 800})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(dpr).arg(width)))
                    << dpr << width;
    }
    void scrollReadyDprAndClamping()
    {
        using namespace waibusnap;
        QFETCH(qreal, dpr);
        QFETCH(int, width);
        CaptureFrame frame;
        frame.display = {0, {-800, 0, width, 600}, dpr, 0, {-790, 24, width - 20, 552}};
        frame.pixels = QImage(qRound(width * dpr), qRound(600 * dpr), QImage::Format_RGB32);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(dpr);
        SelectionOverlay overlay(frame);
        OverlayScrollTestAccess::enable(overlay);
        overlay.show();
        overlay.enterScrollReady(frame.pixels.rect());
        QVERIFY(overlay.isScrollReady());
        auto* bar = overlay.findChild<QWidget*>(QStringLiteral("scrollReadyBar"));
        QVERIFY(QRect(10, 24, width - 20, 552).contains(bar->geometry()));
        for (auto* control : bar->findChildren<QPushButton*>())
            QVERIFY(bar->rect().contains(control->geometry()));
        QCOMPARE(
            overlay.findChild<QLabel*>(QStringLiteral("scrollReadySizeLabel"))->text(),
            QStringLiteral("%1 × %2 像素").arg(frame.pixels.width()).arg(frame.pixels.height()));
        QSignalSpy request(&overlay, &SelectionOverlay::scrollCaptureRequested);
        QTest::keyClick(&overlay, Qt::Key_Return);
        QCOMPARE(request.first().first().toRect(), frame.pixels.rect());
    }
    void scrollControllerRetryAndQuit()
    {
        using namespace waibusnap;
        QTemporaryDir temp;
        RunOptions options;
        options.metricsFile = temp.filePath(QStringLiteral("metrics.jsonl"));
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        ApplicationController controller(*qApp, options);
        auto frame = annotationFrame();
        ApplicationControllerTestAccess::retryDependencies(controller, frame);
        ApplicationControllerTestAccess::factory(
            controller,
            [=](RegionCaptureRequest request, QRect bar, QObject* parent)
            {
                return ScrollCaptureTestAccess::create(
                    request, bar, std::make_unique<RegionStub>(capture),
                    std::make_unique<InjectorStub>(injector), [] { return QPoint(50, 50); }, {},
                    parent);
            });
        auto* overlay = new SelectionOverlay(frame);
        OverlayScrollTestAccess::enable(*overlay);
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::connectScroll(controller, frame.display);
        const QRect region(200, 200, 320, 400);
        overlay->enterScrollReady(region);
        scrollButton(overlay, "scrollStartButton")->click();
        const auto source = scrollDocument();
        capture->frame(source.copy(0, 0, 320, 400));
        capture->error(QStringLiteral("重试用警告"));
        auto result = ApplicationControllerTestAccess::result(controller);
        QVERIFY(result && result->isVisible());
        scrollButton(result, "scrollResultRetryButton")->click();
        QVERIFY(controller.active());
        QTRY_VERIFY(ApplicationControllerTestAccess::overlay(controller));
        auto retry = ApplicationControllerTestAccess::overlay(controller);
        QVERIFY(retry->isScrollReady());
        QCOMPARE(retry->selection(), region);
        QVERIFY(ApplicationControllerTestAccess::readyGuard(controller));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QVERIFY(controller.active());
        // 准备态退出直接丢弃，不弹结果确认；会话码 14。
        controller.quit();
        QVERIFY(!controller.active());
        QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
        QFile metrics(options.metricsFile);
        QVERIFY(metrics.open(QIODevice::ReadOnly));
        const auto first = QJsonDocument::fromJson(metrics.readLine()).object();
        const auto second = QJsonDocument::fromJson(metrics.readLine()).object();
        QCOMPARE(first.value(QStringLiteral("outcome")).toInt(), 13);
        QCOMPARE(second.value(QStringLiteral("outcome")).toInt(), 14);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollControllerPermissionDialogAndQuitCancel()
    {
        using namespace waibusnap;
        QTemporaryDir temp;
        RunOptions options;
        options.metricsFile = temp.filePath(QStringLiteral("metrics.jsonl"));
        ApplicationController controller(*qApp, options);
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        ApplicationControllerTestAccess::factory(
            controller,
            [=](RegionCaptureRequest request, QRect bar, QObject* parent)
            {
                return ScrollCaptureTestAccess::create(
                    request, bar, std::make_unique<RegionStub>(capture),
                    std::make_unique<InjectorStub>(injector), [] { return QPoint(50, 50); }, {},
                    parent);
            });
        const auto frame = annotationFrame();
        auto* overlay = new SelectionOverlay(frame);
        OverlayScrollTestAccess::enable(*overlay);
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::connectScroll(controller, frame.display);
        overlay->enterScrollReady({200, 200, 320, 400});
        scrollButton(overlay, "scrollStartButton")->click();
        auto bar = ApplicationControllerTestAccess::bar(controller);
        bool permissionSeen = false;
        QTimer::singleShot(0,
                           [&]
                           {
                               auto* dialog =
                                   qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                               permissionSeen =
                                   dialog &&
                                   dialog->text() == ScrollInjector::permissionExplanation() &&
                                   !bar->isVisible() && !overlay->isVisible();
                               if (dialog)
                                   dialog->reject();
                           });
        scrollButton(bar, "scrollAutoToggle")->click();
        QVERIFY(permissionSeen);
        QVERIFY(bar->isVisible());
        QVERIFY(!scrollButton(bar, "scrollAutoToggle")->isChecked());
        QCOMPARE(injector->posts.size(), 0);
        capture->frame(scrollDocument().copy(0, 0, 320, 400));
        scrollButton(bar, "scrollFinishButton")->click();
        auto result = ApplicationControllerTestAccess::result(controller);
        QVERIFY(result);
        bool quitSeen = false;
        QTimer::singleShot(
            0,
            [&]
            {
                auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                quitSeen =
                    dialog && dialog->text() == QStringLiteral("有未保存的长图，退出将丢弃。");
                if (dialog)
                    dialog->reject();
            });
        controller.quit();
        QVERIFY(quitSeen && result && result->isVisible());
        result->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollResultSavePinAndLimit()
    {
        using namespace waibusnap;
        QTemporaryDir temp;
        const auto source = scrollDocument();
        ScrollStitchStats stats;
        stats.limitReached = true;
        ScrollResultActions actions;
        actions.loadSavePreferences = [&]
        { return SavePreferences{temp.path(), ImageFormat::Png}; };
        QImage pinned;
        actions.pinImage = [&](const QImage& image)
        {
            pinned = image;
            return ImageOutputResult{true, {}};
        };
        auto* result = new ScrollResultWindow(source, stats, actions);
        result->show();
        QVERIFY(result->findChild<QWidget*>(QStringLiteral("scrollResultWarningBar"))->isVisible());
        QVERIFY(scrollButton(result, "scrollResultRetryButton")->isHidden());
        scrollButton(result, "scrollResultSaveButton")->click();
        QVERIFY(result->hasOutput());
        const auto files = QDir(temp.path()).entryList({QStringLiteral("*.png")}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QCOMPARE(QImage(QDir(temp.path()).filePath(files.first())), source);
        scrollButton(result, "scrollResultPinButton")->click();
        QCOMPARE(pinned, source);
        QVERIFY(result->isVisible());
        QVERIFY(result->close());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void scrollUnreliableFramesNeverTriggerBottom()
    {
        using namespace waibusnap;
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        injector->authorized = true;
        RegionCaptureRequest request{{0, {0, 0, 800, 600}, 1, 0, {}}, {0, 0, 320, 400}, {}};
        std::unique_ptr<ScrollCaptureSession> session(ScrollCaptureTestAccess::create(
            request, {}, std::make_unique<RegionStub>(capture),
            std::make_unique<InjectorStub>(injector), [] { return QPoint(50, 50); }));
        QSignalSpy completed(session.get(), &ScrollCaptureSession::completed);
        QSignalSpy notices(session.get(), &ScrollCaptureSession::noticeChanged);
        session->start();
        const auto source = scrollDocument();
        capture->frame(source.copy(0, 0, 320, 400));
        capture->frame(source.copy(0, 60, 320, 400));
        session->setAutomatic(true);
        ScrollCaptureTestAccess::stopTimer(*session);
        ScrollCaptureTestAccess::tick(*session);
        // 未收到帧时不继续投递，缺口帧不能算作静止到底。
        ScrollCaptureTestAccess::tick(*session);
        QCOMPARE(injector->posts.size(), 1);
        for (int index = 0; index < 3; ++index)
        {
            capture->frame(source.copy(0, 800, 320, 400));
            ScrollCaptureTestAccess::tick(*session);
        }
        QCOMPARE(completed.size(), 0);
        QCOMPARE(session->stats().warnings, 0);
        int gapNotices = 0;
        for (const auto& notice : notices)
        {
            const QString text = notice.first().toString();
            QVERIFY(text != QStringLiteral("滚动较快，部分画面已跳过。"));
            gapNotices += text == QStringLiteral("画面重叠不足或内容变化，部分帧未拼接。");
        }
        QCOMPARE(gapNotices, 0);
        session->complete();
        QCOMPARE(session->stats().unverifiedTails, 1);
    }
    void scrollLatestUnreliableFramePreventsBottom()
    {
        using namespace waibusnap;
        auto capture = std::make_shared<RegionStubState>();
        auto injector = std::make_shared<InjectorStubState>();
        injector->authorized = true;
        RegionCaptureRequest request{{0, {0, 0, 800, 600}, 1, 0, {}}, {0, 0, 320, 400}, {}};
        std::unique_ptr<ScrollCaptureSession> session(ScrollCaptureTestAccess::create(
            request, {}, std::make_unique<RegionStub>(capture),
            std::make_unique<InjectorStub>(injector), [] { return QPoint(50, 50); }));
        QSignalSpy completed(session.get(), &ScrollCaptureSession::completed);
        const QImage source = scrollDocument();
        session->start();
        capture->frame(source.copy(0, 0, 320, 400));
        capture->frame(source.copy(0, 60, 320, 400));
        session->setAutomatic(true);
        ScrollCaptureTestAccess::stopTimer(*session);
        ScrollCaptureTestAccess::tick(*session);
        for (int index = 0; index < 3; ++index)
        {
            // 一次投递之后先静止再失配，不能用早先的静止帧覆盖最新的不确定状态。
            capture->frame(source.copy(0, 60, 320, 400));
            capture->frame(source.copy(0, 900, 320, 400));
            ScrollCaptureTestAccess::tick(*session);
        }
        QCOMPARE(completed.size(), 0);
        capture->frame(source.copy(0, 120, 320, 400));
        session->complete();
        QCOMPARE(session->stats().warnings, 0);
    }
    void scrollBudgetExhaustionIsVisible()
    {
        using namespace waibusnap;
        QTemporaryDir temp;
        RunOptions options;
        options.metricsFile = temp.filePath(QStringLiteral("metrics.jsonl"));
        auto capture = std::make_shared<RegionStubState>();
        ApplicationController controller(*qApp, options);
        ApplicationControllerTestAccess::factory(
            controller,
            [=](RegionCaptureRequest request, QRect bar, QObject* parent)
            {
                ScrollStitchLimits limits;
                limits.workingSetBytes = 1;
                return ScrollCaptureTestAccess::create(
                    request, bar, std::make_unique<RegionStub>(capture), nullptr,
                    [] { return QPoint(50, 50); }, limits, parent);
            });
        const auto frame = annotationFrame();
        auto* overlay = new SelectionOverlay(frame);
        OverlayScrollTestAccess::enable(*overlay);
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::connectScroll(controller, frame.display);
        overlay->enterScrollReady({200, 200, 320, 400});
        scrollButton(overlay, "scrollStartButton")->click();
        const auto bar = ApplicationControllerTestAccess::bar(controller);
        bool observed = false;
        QTimer::singleShot(0, &controller,
                           [&]
                           {
                               auto* dialog =
                                   qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                               observed =
                                   dialog && !bar->isVisible() &&
                                   dialog->text().contains(QStringLiteral("请缩小选区后重试"));
                               if (dialog)
                                   dialog->accept();
                           });
        capture->frame(scrollDocument().copy(0, 0, 320, 400));
        QVERIFY(observed);
        QVERIFY(!controller.active());
        QVERIFY(!ApplicationControllerTestAccess::result(controller));
        QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
    }
    void scrollHiccupsHaveNoWarningOrRetry()
    {
        using namespace waibusnap;
        ScrollStitchStats stats;
        stats.hiccups = 120;
        stats.lowConfidence = 10;
        auto* result = new ScrollResultWindow(scrollDocument(), stats);
        result->show();
        QVERIFY(result->findChild<QWidget*>(QStringLiteral("scrollResultWarningBar"))->isHidden());
        QVERIFY(scrollButton(result, "scrollResultRetryButton")->isHidden());
        result->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ScrollControlBar bar({0, 0, 800, 400}, {0, 0, 1000, 800});
        stats.devicePixelRatio = 2;
        stats.lengthPx = 1800;
        stats.frames = 30;
        bar.setStatistics(stats, 2000);
        QCOMPARE(bar.findChild<QLabel*>(QStringLiteral("scrollStatusLabel"))->toolTip(),
                 QStringLiteral("已拼接 900 逻辑 px（接近上限）"));
    }
    void scrollCropRetainsCaptureAxis()
    {
        using namespace waibusnap;
        ScrollStitchStats stats;
        stats.motionDetected = true;
        stats.axis = ScrollAxis::Vertical;
        auto* result = new ScrollResultWindow(scrollDocument(800, 400), stats);
        result->show();
        scrollButton(result, "scrollResultZoom100Button")->click();
        auto* view = result->findChild<QWidget*>(QStringLiteral("scrollResultView"));
        for (int round = 0; round < 2; ++round)
        {
            scrollButton(result, "scrollResultCropButton")->click();
            QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, {100, 1});
            QMouseEvent move(QEvent::MouseMove, QPointF(100, 50),
                             view->mapToGlobal(QPoint(100, 50)), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(view, &move);
            QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, {100, 50});
            scrollButton(result, "scrollResultCropApplyButton")->click();
            QCOMPARE(result->image().size(), QSize(800, 350 - round * 50));
        }
        result->closeWithoutConfirmation();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void ocrModeLifecycle()
    {
        auto engine = std::make_shared<OcrStub>();
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        const QRect selection = overlay.selection();
        QTemporaryDir temp;
        QVERIFY(overlay.exportToPath(temp.filePath("ocr.png")));
        QVERIFY(overlay.isSelectionSaved());
        int tools = 0;
        for (auto* control : overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"))
                                 ->findChildren<QPushButton*>())
            if (control->objectName().endsWith("ToolButton"))
                ++tools;
        QCOMPARE(tools, 6);
        QCOMPARE(overlay.findChild<QWidget*>(QStringLiteral("annotationToolBar"))
                     ->findChildren<QPushButton*>()
                     .size(),
                 qsizetype(6));
        QVERIFY(!button(overlay, "ocrToolButton")->icon().isNull());
        QCOMPARE(button(overlay, "ocrToolButton")->toolTip(), QStringLiteral("取字（文字识别）"));
        auto* extract = button(overlay, "extractAllTextButton");
        QVERIFY(extract->isHidden());
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QVERIFY(overlay.isTextMode());
        QTRY_VERIFY(!overlay.isRecognizingText());
        QVERIFY(!extract->isHidden());
        QVERIFY(button(overlay, "ocrToolButton")->isChecked());
        QVERIFY(!overlay.activeTool());
        QCOMPARE(engine->calls.load(), 1);
        QVERIFY(engine->worker.load());
        QCOMPARE(overlay.selection(), selection);
        QVERIFY(overlay.isSelectionSaved());
        drag(overlay, {100, 100}, {500, 320});
        QCOMPARE(overlay.selection(), selection);
        QVERIFY(overlay.annotations().isEmpty());
        QVERIFY(!overlay.magnifierVisible());
        overlay.setFocus();
        QTest::keyClick(&overlay, Qt::Key_Right);
        QTest::keyClick(&overlay, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(&overlay, Qt::Key_Left, Qt::AltModifier);
        QCOMPARE(overlay.selection(), selection);
        QVERIFY(!overlay.magnifierVisible());
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QVERIFY(!overlay.isTextMode());
        QVERIFY(overlay.selectedOcrText().isEmpty());
        QVERIFY(extract->isHidden());
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QCOMPARE(engine->calls.load(), 2);
        clickAnnotationTool(overlay, "lineToolButton");
        QVERIFY(!overlay.isTextMode());
        QCOMPARE(overlay.activeTool(),
                 std::optional<waibusnap::AnnotationType>(waibusnap::AnnotationType::Line));
        drag(overlay, {200, 250}, {300, 300});
        QCOMPARE(overlay.annotations().size(), 1);
        clickAnnotationTool(overlay, "lineToolButton");
        overlay.setFocus();
        QTest::keyClick(&overlay, Qt::Key_Right);
        QCOMPARE(overlay.selection(), selection.translated(1, 0));
        QVERIFY(overlay.magnifierVisible());
        QCOMPARE(finished.count(), 0);
    }
    void ocrSelection_data()
    {
        QTest::addColumn<int>("gesture");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<int>("boxes");
        QTest::newRow("english") << 0 << QString("Hello") << 1;
        QTest::newRow("chinese") << 1 << QStringLiteral("你好") << 1;
        QTest::newRow("emoji") << 2 << QString::fromUtf8("😀") << 1;
        QTest::newRow("cross-line") << 3 << QString::fromUtf8("Hello, world!  \n你好，😀 OCR") << 5;
        QTest::newRow("reverse") << 4 << QString::fromUtf8("Hello, world!  \n你好，😀 OCR") << 5;
        QTest::newRow("punctuation-space") << 5 << QString("Hello, world") << 2;
    }
    void ocrSelection()
    {
        QFETCH(int, gesture);
        QFETCH(QString, expected);
        QFETCH(int, boxes);
        auto engine = std::make_shared<OcrStub>();
        QString copied;
        int copies = 0;
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        actions.setClipboardText = [&](const QString& text)
        {
            copied = text;
            ++copies;
        };
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        const QRect selection = overlay.selection();
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QMouseEvent textMove(QEvent::MouseMove, QPointF(125, 120),
                             overlay.mapToGlobal(QPoint(125, 120)), Qt::NoButton, Qt::NoButton,
                             Qt::NoModifier);
        QApplication::sendEvent(&overlay, &textMove);
        QCOMPARE(overlay.cursor().shape(), Qt::IBeamCursor);
        QMouseEvent blankMove(QEvent::MouseMove, QPointF(400, 300),
                              overlay.mapToGlobal(QPoint(400, 300)), Qt::NoButton, Qt::NoButton,
                              Qt::NoModifier);
        QApplication::sendEvent(&overlay, &blankMove);
        QCOMPARE(overlay.cursor().shape(), Qt::ArrowCursor);
        if (gesture < 3)
        {
            const QPoint points[] = {{125, 120}, {125, 160}, {172, 160}};
            QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier, points[gesture]);
        }
        else if (gesture == 3)
            drag(overlay, {125, 120}, {215, 160});
        else if (gesture == 4)
            drag(overlay, {215, 160}, {125, 120});
        else
            drag(overlay, {125, 120}, {185, 120});
        QCOMPARE(overlay.selectedOcrText(), expected);
        QCOMPARE(overlay.ocrHighlightBoxes().size(), boxes);
        QVERIFY(overlay.ocrHighlightBoxes().first().width() > 0);
        QImage painted(overlay.size() * overlay.devicePixelRatioF(), QImage::Format_ARGB32);
        painted.setDevicePixelRatio(overlay.devicePixelRatioF());
        overlay.render(&painted);
        const QRectF first = overlay.ocrHighlightBoxes().first();
        const QPointF center = (first.center() + QPointF(selection.topLeft())) / 2;
        QVERIFY(painted.pixelColor(qRound(center.x() * painted.devicePixelRatio()),
                                   qRound(center.y() * painted.devicePixelRatio())) !=
                QColor(Qt::white));
        overlay.setFocus();
        standardKey(&overlay, QKeySequence::Copy);
        QCOMPARE(copies, 1);
        QCOMPARE(copied, expected);
        QCOMPARE(overlay.findChild<QLabel*>("outputStatus")->text(),
                 QStringLiteral("已复制 %1 个字符").arg(expected.size()));
        QCOMPARE(overlay.selection(), selection);
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.isVisible());
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {400, 300});
        QVERIFY(overlay.selectedOcrText().isEmpty());
        QVERIFY(overlay.ocrHighlightBoxes().isEmpty());
        standardKey(&overlay, QKeySequence::Copy);
        QCOMPARE(copies, 1);
    }
    void ocrCopyDispatchAndSavedState()
    {
        auto engine = std::make_shared<OcrStub>();
        int imageCopies = 0;
        int textCopies = 0;
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        actions.copyImage = [&](const QImage&)
        {
            ++imageCopies;
            return waibusnap::ImageOutputResult{true, {}};
        };
        actions.setClipboardText = [&](const QString&) { ++textCopies; };
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTemporaryDir temp;
        const auto path = temp.filePath("saved.png");
        QVERIFY(overlay.exportToPath(path));
        const auto saved = QImage(path);
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        overlay.setFocus();
        standardKey(&overlay, QKeySequence::Copy);
        QCOMPARE(imageCopies, 0);
        QCOMPARE(textCopies, 0);
        QVERIFY(overlay.findChild<QLabel*>("outputStatus")->isHidden());
        QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier, {172, 160});
        standardKey(&overlay, QKeySequence::Copy);
        QCOMPARE(textCopies, 1);
        QCOMPARE(imageCopies, 0);
        QVERIFY(overlay.isSelectionSaved());
        QCOMPARE(finished.count(), 0);
        compareImagePixels(QImage(path), saved);
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        QCOMPARE(imageCopies, 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first()[1].toInt(), waibusnap::copiedSessionOutcome);
        waibusnap::SelectionOverlay ordinary(annotationFrame(), actions);
        QSignalSpy ordinaryFinished(&ordinary, &waibusnap::SelectionOverlay::finished);
        ordinary.show();
        drag(ordinary, {100, 100}, {650, 450});
        ordinary.activateWindow();
        ordinary.setFocus();
        standardKey(&ordinary, QKeySequence::Copy);
        QCOMPARE(imageCopies, 2);
        QCOMPARE(ordinaryFinished.first()[1].toInt(), waibusnap::copiedSessionOutcome);
    }
    void ocrDelayedProgressExitAndStaleResult()
    {
        auto engine = std::make_shared<OcrStub>();
        engine->delayMs = 650;
        engine->firstOnlyDelay = true;
        engine->results = {{false, QStringLiteral("旧结果不得展示"), {}}, sampleOcrResult()};
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_COMPARE(engine->calls.load(), 1);
        QVERIFY(overlay.isRecognizingText());
        QTRY_COMPARE(overlay.findChild<QLabel*>("outputStatus")->text(),
                     QStringLiteral("正在识别文字…"));
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(elapsed.elapsed() < 150);
        QVERIFY(!overlay.isTextMode());
        QVERIFY(!overlay.isRecognizingText());
        QVERIFY(overlay.findChild<QLabel*>("outputStatus")->isHidden());
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QCOMPARE(engine->calls.load(), 2);
        QTest::qWait(700);
        QVERIFY(overlay.findChild<QLabel*>("outputStatus")->isHidden());
        QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier, {125, 120});
        QCOMPARE(overlay.selectedOcrText(), QString("Hello"));
        QCOMPARE(finished.count(), 0);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(!overlay.isTextMode());
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first()[1].toInt(), waibusnap::cancelledSessionOutcome);
    }
    void ocrDialogEscAndRetry()
    {
        auto engine = std::make_shared<OcrStub>();
        engine->results = {{false, QStringLiteral("测试失败"), {}}, sampleOcrResult()};
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        const QRect selection = overlay.selection();
        const auto annotations = overlay.annotations();
        QTemporaryDir temp;
        QVERIFY(overlay.exportToPath(temp.filePath("before-dialog.png")));
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QVERIFY(overlay.findChild<QLabel*>("outputStatus")
                    ->text()
                    .contains(QStringLiteral("测试失败")));
        QTest::mouseClick(button(overlay, "extractAllTextButton"), Qt::LeftButton);
        auto* dialog = overlay.findChild<waibusnap::OcrDialog*>();
        QVERIFY(dialog);
        QVERIFY(dialog->isModal());
        QCOMPARE(dialog->parentWidget(), &overlay);
        drag(overlay, {125, 120}, {215, 160});
        QVERIFY(overlay.selectedOcrText().isEmpty());
        auto* retry = dialog->findChild<QPushButton*>("ocrRetryButton");
        QVERIFY(retry->isVisible());
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QCOMPARE(engine->calls.load(), 2);
        QCOMPARE(dialog->findChild<QTextEdit*>("ocrTextEdit")->toPlainText(),
                 waibusnap::text_layout::assembleFullText(sampleOcrResult().lines));
        QTest::mouseClick(dialog->findChild<QPushButton*>("ocrEditButton"), Qt::LeftButton);
        dialog->findChild<QTextEdit*>("ocrTextEdit")
            ->insertPlainText(QStringLiteral("临时编辑不改图"));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QVERIFY(!dialog->isVisible());
        QVERIFY(overlay.isTextMode());
        QVERIFY(overlay.isSelectionSaved());
        QCOMPARE(finished.count(), 0);
        QCOMPARE(overlay.selection(), selection);
        compareAnnotations(overlay.annotations(), annotations);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(!overlay.isTextMode());
        QCOMPARE(finished.count(), 0);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
    }
    void ocrDialogLoadingAndEmpty()
    {
        auto engine = std::make_shared<OcrStub>();
        engine->delayMs = 450;
        engine->results = {{true, {}, {}}};
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "extractAllTextButton"), Qt::LeftButton);
        auto* dialog = overlay.findChild<waibusnap::OcrDialog*>();
        QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLabel*>("ocrStateLabel")->text(),
                 QStringLiteral("正在识别文字…"));
        QTRY_VERIFY(!overlay.isRecognizingText());
        QCOMPARE(dialog->findChild<QLabel*>("ocrStateLabel")->text(),
                 QStringLiteral("未识别到文字"));
        QVERIFY(!dialog->findChild<QPushButton*>("ocrCopyButton")->isEnabled());
        dialog->reject();
        QCOMPARE(overlay.findChild<QLabel*>("outputStatus")->text(),
                 QStringLiteral("未识别到文字"));
    }
    void ocrDialogFormats_data()
    {
        QTest::addColumn<QByteArray>("control");
        QTest::addColumn<QString>("htmlMarker");
        QTest::newRow("bold") << QByteArray("ocrBoldButton") << QString("font-weight:700");
        QTest::newRow("italic") << QByteArray("ocrItalicButton") << QString("font-style:italic");
        QTest::newRow("underline")
            << QByteArray("ocrUnderlineButton") << QString("text-decoration: underline");
        QTest::newRow("size") << QByteArray("ocrSizeCombo") << QString("font-size:22pt");
        QTest::newRow("red") << QByteArray("ocrColorCombo")
                             << waibusnap::annotationColors()[0].name();
        QTest::newRow("bullets") << QByteArray("ocrBulletsButton") << QString("<ul");
        QTest::newRow("numbers") << QByteArray("ocrNumbersButton") << QString("<ol");
        QTest::newRow("shortcut-bold") << QByteArray("B") << QString("font-weight:700");
        QTest::newRow("shortcut-italic") << QByteArray("I") << QString("font-style:italic");
        QTest::newRow("shortcut-underline")
            << QByteArray("U") << QString("text-decoration: underline");
    }
    void ocrDialogFormats()
    {
        QFETCH(QByteArray, control);
        QFETCH(QString, htmlMarker);
        QString html;
        QString text;
        QStringList mimeFormats;
        int copies = 0;
        waibusnap::OcrDialog dialog(nullptr,
                                    [&](const QMimeData& mime)
                                    {
                                        html = mime.html();
                                        text = mime.text();
                                        mimeFormats = mime.formats();
                                        ++copies;
                                    });
        dialog.setResult(sampleOcrResult());
        dialog.show();
        dialog.activateWindow();
        auto* editor = dialog.findChild<QTextEdit*>("ocrTextEdit");
        QVERIFY(editor->isReadOnly());
        QVERIFY(dialog.findChild<QWidget*>("ocrFormatToolbar")->isHidden());
        const QString original = editor->toPlainText();
        QTest::keyClicks(editor, "blocked");
        QCOMPARE(editor->toPlainText(), original);
        QTest::mouseClick(dialog.findChild<QPushButton*>("ocrEditButton"), Qt::LeftButton);
        QVERIFY(!editor->isReadOnly());
        QVERIFY(!dialog.findChild<QWidget*>("ocrFormatToolbar")->isHidden());
        editor->moveCursor(QTextCursor::End);
        editor->insertPlainText(" edited");
        editor->selectAll();
        if (control.endsWith("Combo"))
        {
            auto* combo = dialog.findChild<QComboBox*>(QString::fromLatin1(control));
            const int index = control == "ocrSizeCombo" ? 2 : 1;
            combo->setCurrentIndex(index);
            QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
        }
        else if (control.size() == 1)
        {
            editor->setFocus();
            QTest::keyClick(editor, Qt::Key(control[0]), Qt::ControlModifier);
        }
        else
            QTest::mouseClick(dialog.findChild<QPushButton*>(QString::fromLatin1(control)),
                              Qt::LeftButton);
        QTest::mouseClick(dialog.findChild<QPushButton*>("ocrEditButton"), Qt::LeftButton);
        QVERIFY(editor->isReadOnly());
        QTest::mouseClick(dialog.findChild<QPushButton*>("ocrCopyButton"), Qt::LeftButton);
        QCOMPARE(copies, 1);
        QVERIFY(mimeFormats.contains("text/html"));
        QVERIFY(mimeFormats.contains("text/plain"));
        QVERIFY2(html.contains(htmlMarker), qPrintable(html));
        QVERIFY(text.endsWith(" edited"));
        QCOMPARE(text, editor->toPlainText());
        QCOMPARE(dialog.findChild<QLabel*>("ocrCopyFeedback")->text(), QStringLiteral("已复制"));
        QTest::mouseClick(dialog.findChild<QPushButton*>("ocrPlainCopyButton"), Qt::LeftButton);
        QCOMPARE(copies, 2);
        QVERIFY(!mimeFormats.contains("text/html"));
        QCOMPARE(mimeFormats, QStringList{"text/plain"});
        QCOMPARE(text, editor->toPlainText());
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(copies, 2);
    }
    void ocrPreviewLocalCopyIsPlain()
    {
        ClipboardRestore restore;
        waibusnap::OcrDialog dialog;
        dialog.setResult(sampleOcrResult());
        dialog.show();
        dialog.activateWindow();
        auto* editor = dialog.findChild<QTextEdit*>("ocrTextEdit");
        QTextCursor cursor = editor->textCursor();
        cursor.setPosition(0);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        editor->setTextCursor(cursor);
        editor->setFocus();
        standardKey(editor, QKeySequence::Copy);
        QCOMPARE(QApplication::clipboard()->mimeData()->text(), QString("Hello"));
        QVERIFY(!QApplication::clipboard()->mimeData()->hasHtml());
    }
    void ocrInputUsesAnnotatedPixelsAndFallback()
    {
        auto engine = std::make_shared<OcrStub>();
        auto result = sampleOcrResult();
        result.lines[0].tokens.clear();
        engine->results = {result};
        waibusnap::OverlayActions actions;
        actions.recognizer = engine;
        waibusnap::SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        drag(overlay, {100, 100}, {650, 450});
        clickAnnotationTool(overlay, "coverToolButton");
        drag(overlay, {120, 120}, {260, 170});
        clickAnnotationTool(overlay, "mosaicToolButton");
        drag(overlay, {320, 140}, {430, 210});
        const QImage expected = waibusnap::renderAnnotatedSelection(
            annotationFrame().pixels, overlay.annotations(), overlay.selection());
        const auto before = overlay.annotations();
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QCOMPARE(engine->input().devicePixelRatio(), 1.0);
        compareImagePixels(engine->input(), expected);
        compareAnnotations(overlay.annotations(), before);
        QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier, {125, 120});
        QCOMPARE(overlay.selectedOcrText(), result.lines[0].text);
        QCOMPARE(overlay.ocrHighlightBoxes(), QVector<QRectF>{result.lines[0].box});
    }
    void ocrTaskDestructionWaits()
    {
        auto engine = std::make_shared<OcrStub>();
        engine->delayMs = 120;
        auto task =
            std::make_unique<waibusnap::TextRecognitionTask>(engine, sampleFrame().pixels, 1);
        task->start();
        QTRY_COMPARE(engine->calls.load(), 1);
        task.reset();
        QCOMPARE(engine->completions.load(), 1);
    }

    void trayIconUsesEmbeddedPlatformAssets()
    {
        const auto icon = waibusnap::createTrayIcon();
        QVERIFY(!icon.isNull());
        constexpr int extent = WAIBUSNAP_TEST_TRAY_SIZE;
        constexpr bool mask = WAIBUSNAP_TEST_TRAY_MASK != 0;
        QCOMPARE(icon.isMask(), mask);
        const QSize logicalSize(extent, extent);
        const auto sizes = icon.availableSizes();
        QVERIFY(sizes.contains(logicalSize));
        QVERIFY(sizes.contains(logicalSize * 2));
        for (const auto& size : sizes)
            QVERIFY(size == logicalSize || size == logicalSize * 2);
        const QString basename = mask ? QStringLiteral(":/icons/tray/waibusnap-tray-mask")
                                      : QStringLiteral(":/icons/tray/waibusnap-tray-color");
        for (int scale : {1, 2})
        {
            const QString suffix = scale == 1 ? QStringLiteral(".png") : QStringLiteral("@2x.png");
            const QImage resource(basename + suffix);
            QVERIFY(!resource.isNull());
            QVERIFY(resource.hasAlphaChannel());
            QCOMPARE(resource.size(), logicalSize * scale);
            const auto pixmap = icon.pixmap(logicalSize, qreal(scale));
            QVERIFY(!pixmap.isNull());
            QCOMPARE(pixmap.size(), logicalSize * scale);
            QCOMPARE(pixmap.devicePixelRatio(), qreal(scale));
        }
    }
    void pinCommitsTextAndEndsWithEleven()
    {
        using namespace waibusnap;
        const auto frame = annotationFrame();
        QImage pinned;
        QImage expected;
        int count = 0;
        SelectionOverlay* current = nullptr;
        OverlayActions actions;
        actions.pinImage = [&](const QImage& image)
        {
            ++count;
            pinned = image;
            expected = renderAnnotatedSelection(frame.pixels, current->annotations(),
                                                current->selection());
            return ImageOutputResult{true, {}};
        };
        actions.copyImage = [](const QImage&) { return ImageOutputResult{false, {}}; };
        SelectionOverlay overlay(frame, actions);
        current = &overlay;
        overlay.show();
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        drag(overlay, {100, 100}, {300, 250});
        const QRect selection = overlay.selection();
        auto* pin = button(overlay, "pinButton");
        QVERIFY(pin && pin->isVisible());
        QVERIFY(!pin->icon().isNull());
        QCOMPARE(pin->toolTip(), QStringLiteral("钉到屏幕"));
        QVERIFY(button(overlay, "saveButton")->x() < pin->x());
        QVERIFY(pin->x() < button(overlay, "cancelButton")->x());
        clickAnnotationTool(overlay, "lineToolButton");
        drag(overlay, {120, 150}, {280, 150});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 120});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        editor->setPlainText(QStringLiteral("钉图前确认\nABC 123"));
        QTest::mouseClick(pin, Qt::LeftButton);
        QCOMPARE(count, 1);
        QCOMPARE(pinned, expected);
        QCOMPARE(pinned.devicePixelRatio(), qreal(1));
        QVERIFY(pinned != cropFrozenSelection(frame.pixels, selection));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toRect(), selection);
        QCOMPARE(finished.first().at(1).toInt(), pinnedSessionOutcome);
        QVERIFY(overlay.annotations().isEmpty());
        QVERIFY(!overlay.activeTool());
        QVERIFY(!overlay.isVisible());
        QVERIFY(!editor->isVisible());
    }
    void pinFailureKeepsSessionAndCanRetry()
    {
        using namespace waibusnap;
        int attempts = 0;
        OverlayActions actions;
        actions.pinImage = [&](const QImage&)
        { return ImageOutputResult{++attempts == 2, QStringLiteral("测试创建失败")}; };
        SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "lineToolButton");
        drag(overlay, {120, 150}, {280, 150});
        QTest::mouseClick(button(overlay, "pinButton"), Qt::LeftButton);
        QCOMPARE(attempts, 1);
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.isVisible());
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QVERIFY(overlay.findChild<QLabel*>(QStringLiteral("outputStatus"))
                    ->text()
                    .contains(QStringLiteral("钉图失败")));
        QTest::mouseClick(button(overlay, "pinButton"), Qt::LeftButton);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 11);
    }
    void pinInheritsSavedStateAndGlobalPosition_data()
    {
        QTest::addColumn<bool>("saved");
        QTest::newRow("unsaved") << false;
        QTest::newRow("saved") << true;
    }
    void pinInheritsSavedStateAndGlobalPosition()
    {
        using namespace waibusnap;
        QFETCH(bool, saved);
        StickerManager manager;
        auto frame = annotationFrame();
        frame.display.logicalGeometry.moveTopLeft({-1000, -600});
        SelectionOverlay* current = nullptr;
        OverlayActions actions;
        actions.pinImage = [&](const QImage& image) {
            return manager.create(image, current->selectionGlobalPosition(),
                                  current->isSelectionSaved());
        };
        SelectionOverlay overlay(frame, actions);
        current = &overlay;
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        QCOMPARE(overlay.selectionGlobalPosition(), QPoint(-900, -500));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        if (saved)
            QVERIFY(overlay.exportToPath(temporary.filePath(QStringLiteral("已保存.png"))));
        QTest::mouseClick(button(overlay, "pinButton"), Qt::LeftButton);
        QCOMPARE(manager.count(), 1);
        const auto sticker = manager.windows().first();
        QCOMPARE(sticker->pos(), QPoint(-900, -500));
        QCOMPARE(sticker->isSaved(), saved);
        manager.closeAll();
        QVERIFY(!sticker);
        QCOMPARE(manager.count(), 0);
    }
    void stickerWindowsScaleDragAndCloseIndependently()
    {
        using namespace waibusnap;
        StickerActions actions;
        actions.confirmClose = [](QWidget*) { return StickerCloseDecision::Discard; };
        StickerManager manager(nullptr, actions);
        QImage image(480, 360, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::green);
        QVERIFY(!manager.create({}, {}).success);
        QCOMPARE(manager.count(), 0);
        QVERIFY(manager.create(image, {30, 40}).success);
        QVERIFY(manager.create(image, {300, 220}, true).success);
        QCOMPARE(manager.count(), 2);
        QPointer<StickerWindow> first = manager.windows().first();
        QPointer<StickerWindow> second = manager.windows().last();
        const qreal dpr = first->windowHandle()->screen()->devicePixelRatio();
        QCOMPARE(first->size(), stickerWindowSize(image.size(), 1, dpr));
        QCOMPARE(first->image().devicePixelRatio(), qreal(1));
        QVERIFY(first->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(first->windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(first->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(first->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        auto* controls = first->findChild<QWidget*>(QStringLiteral("stickerControls"));
        QVERIFY(!controls->isVisible());
        QEnterEvent enter({20, 20}, {20, 20}, first->mapToGlobal(QPoint(20, 20)));
        QApplication::sendEvent(first, &enter);
        QVERIFY(controls->isVisible());
        const QPointF anchor = QPointF(first->pos()) + QPointF(60, 50);
        const QPointF oldPosition = first->pos();
        const QSize oldSize = first->size();
        QWheelEvent wheel(first->mapFromGlobal(anchor), anchor, {}, {0, 120}, Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(first, &wheel);
        QCOMPARE(first->scale(), qreal(1.25));
        QCOMPARE(first->size(), stickerWindowSize(image.size(), 1.25, dpr));
        const QPointF expected =
            anchoredStickerPosition(oldPosition, oldSize, first->size(), anchor);
        QCOMPARE(first->pos(), expected.toPoint());
        QCOMPARE(second->scale(), qreal(1));
        const auto click = [&](const char* name)
        {
            auto* control = first->findChild<QPushButton*>(QString::fromLatin1(name));
            QVERIFY(control);
            QTest::mouseClick(control, Qt::LeftButton);
        };
        click("stickerResetButton");
        QCOMPARE(first->scale(), qreal(1));
        const QPointF center =
            QPointF(first->pos()) + QPointF(first->width() / 2.0, first->height() / 2.0);
        for (int index = 0; index < 20; ++index)
            click("stickerZoomInButton");
        QCOMPARE(first->scale(), qreal(4));
        QCOMPARE(first->size(), stickerWindowSize(image.size(), 4, dpr));
        QCOMPARE(QPointF(first->pos()) + QPointF(first->width() / 2.0, first->height() / 2.0),
                 center);
        for (int index = 0; index < 20; ++index)
            click("stickerZoomOutButton");
        QCOMPARE(first->scale(), qreal(0.25));
        QCOMPARE(first->size(), stickerWindowSize(image.size(), 0.25, dpr));
        click("stickerResetButton");
        const QPoint before = first->pos();
        const QPointF press = QPointF(before) + QPointF(80, 80);
        const QPointF release = press + QPointF(70, 40);
        const auto sendMouse = [&](QEvent::Type type, QPointF global, Qt::MouseButtons buttons)
        {
            QMouseEvent event(type, first->mapFromGlobal(global), global,
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                              Qt::NoModifier);
            QApplication::sendEvent(first, &event);
        };
        sendMouse(QEvent::MouseButtonPress, press, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, release, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, release, Qt::NoButton);
        QCOMPARE(first->pos(), before + QPoint(70, 40));
        QCOMPARE(second->pos(), QPoint(300, 220));
        first->setEditing(true);
        const QRect editingGeometry = first->geometry();
        first->setScale(4, press);
        sendMouse(QEvent::MouseButtonPress, press, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, release, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, release, Qt::NoButton);
        QCOMPARE(first->geometry(), editingGeometry);
        first->setEditing(false);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(first, &leave);
        QVERIFY(!controls->isVisible());
        controls->show();
        click("stickerCloseButton");
        QCOMPARE(manager.count(), 1);
        QTRY_VERIFY(!first);
        QVERIFY(second && second->isVisible());
        second->close();
        QCOMPARE(manager.count(), 0);
        QTRY_VERIFY(!second);
        QVERIFY(!qApp->quitOnLastWindowClosed());
    }
    void stickerOutputsKeepOriginalPixelsAndRetryFailures()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto frame = annotationFrame();
        Annotation line{AnnotationType::Line, {}, {200, 300}, {600, 300}, {}, {}};
        const auto image = renderAnnotatedSelection(frame.pixels, {line}, {200, 200, 400, 300});
        QImage copied;
        int copies = 0;
        int saves = 0;
        StickerActions actions;
        actions.copyImage = [&](const QImage& output)
        {
            copied = output;
            return ImageOutputResult{++copies == 2, QStringLiteral("测试复制失败")};
        };
        const QString path = temporary.filePath(QStringLiteral("中文 空格.png"));
        actions.chooseSavePath = [&](QWidget*, const QString& suggestion)
        {
            ++saves;
            if (!suggestion.endsWith(QStringLiteral(".png")))
                return QString();
            return saves == 1 ? temporary.filePath(QStringLiteral("不存在/图.png")) : path;
        };
        StickerManager manager(nullptr, actions);
        QVERIFY(manager.create(image, {30, 40}).success);
        const auto sticker = manager.windows().first();
        sticker->setScale(4, {30, 40});
        const auto click = [&](const char* name) {
            QTest::mouseClick(sticker->findChild<QPushButton*>(QString::fromLatin1(name)),
                              Qt::LeftButton);
        };
        auto* controls = sticker->findChild<QWidget*>(QStringLiteral("stickerControls"));
        controls->show();
        click("stickerSaveButton");
        QVERIFY(!sticker->isSaved());
        auto* status = sticker->findChild<QLabel*>(QStringLiteral("stickerStatus"));
        QVERIFY(status->text().contains(QStringLiteral("保存失败")));
        click("stickerSaveButton");
        QVERIFY(sticker->isSaved());
        const QImage saved(path);
        QCOMPARE(saved.size(), image.size());
        QCOMPARE(saved.devicePixelRatio(), qreal(1));
        QCOMPARE(saved, image.convertToFormat(saved.format()));
        click("stickerCopyButton");
        QCOMPARE(copied, image);
        QVERIFY(status->text().contains(QStringLiteral("复制失败")));
        click("stickerCopyButton");
        QCOMPARE(copied, image);
        QCOMPARE(copies, 2);
        QCOMPARE(copied.devicePixelRatio(), qreal(1));
        QVERIFY(status->text().contains(QStringLiteral("已复制")));
        QCOMPARE(manager.count(), 1);
        QVERIFY(sticker->isVisible());
        QVERIFY(sticker->isSaved());
    }
    void stickerSaveCollisionCancelAndExitDuringPanel()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString bare = temporary.filePath(QStringLiteral("原文件"));
        const QString path = bare + QStringLiteral(".png");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original");
        file.close();
        int dialogs = 0;
        QString repeated;
        bool exitDuringPanel = false;
        StickerManager* current = nullptr;
        StickerActions actions;
        actions.chooseSavePath = [&](QWidget*, const QString& suggestion)
        {
            ++dialogs;
            if (exitDuringPanel)
            {
                current->closeAll();
                return temporary.filePath(QStringLiteral("不能保存.png"));
            }
            if (dialogs == 1)
                return bare;
            repeated = suggestion;
            return QString();
        };
        StickerManager manager(nullptr, actions);
        current = &manager;
        QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        QVERIFY(manager.create(image, {30, 40}).success);
        const auto sticker = manager.windows().first();
        sticker->findChild<QWidget*>(QStringLiteral("stickerControls"))->show();
        QTest::mouseClick(sticker->findChild<QPushButton*>(QStringLiteral("stickerSaveButton")),
                          Qt::LeftButton);
        QCOMPARE(dialogs, 2);
        QCOMPARE(repeated, path);
        QVERIFY(!sticker->isSaved());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
        file.close();
        exitDuringPanel = true;
        QTest::mouseClick(sticker->findChild<QPushButton*>(QStringLiteral("stickerSaveButton")),
                          Qt::LeftButton);
        QVERIFY(!sticker);
        QCOMPARE(manager.count(), 0);
        QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("不能保存.png"))));
    }
    void tinyStickerKeepsAllActionsAccessible()
    {
        using namespace waibusnap;
        StickerActions actions;
        actions.confirmClose = [](QWidget*) { return StickerCloseDecision::Discard; };
        StickerManager manager(nullptr, actions);
        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        QVERIFY(manager.create(image, {30, 40}).success);
        const auto sticker = manager.windows().first();
        sticker->setScale(0.25, {30, 40});
        QEnterEvent enter({1, 1}, {1, 1}, sticker->mapToGlobal(QPoint(1, 1)));
        QApplication::sendEvent(sticker, &enter);
        auto* more = sticker->findChild<QPushButton*>(QStringLiteral("stickerMoreButton"));
        QVERIFY(more && more->isVisible());
        QVERIFY(sticker->rect().contains(more->geometry()));
        QCOMPARE(sticker->actions().size(), qsizetype(7));
        QCOMPARE(sticker->actions().last()->text(), QStringLiteral("关闭贴图"));
        sticker->actions().last()->trigger();
        QCOMPARE(manager.count(), 0);
        QTRY_VERIFY(!sticker);
    }
    void managerRecoversOffscreenAndDestroysPendingClose()
    {
        using namespace waibusnap;
        StickerActions actions;
        actions.confirmClose = [](QWidget*) { return StickerCloseDecision::Discard; };
        StickerManager manager(nullptr, actions);
        QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        QVERIFY(manager.create(image, {-10000, -10000}).success);
        auto first = manager.windows().first();
        manager.recoverWindows();
        QVERIFY(QGuiApplication::primaryScreen()->availableGeometry().contains(first->pos()));
        QVERIFY(manager.create(image, {80, 80}).success);
        auto second = manager.windows().last();
        first->close();
        QCOMPARE(manager.count(), 1);
        manager.closeAll();
        QVERIFY(!first);
        QVERIFY(!second);
        QCOMPARE(manager.count(), 0);
        manager.closeAll();
        QCOMPARE(manager.count(), 0);
    }
    void stickerHideRestorePreservesStateAndHistory()
    {
        using namespace waibusnap;
        StickerManager manager;
        manager.hideAll();
        manager.restoreAll();
        QCOMPARE(manager.count(), 0);
        const QImage image = mosaicTestImage({1200, 1000});
        QVector<QRect> geometries;
        QVector<qreal> scales;
        QVector<QImage> images, rendered;
        QVector<QVector<Annotation>> annotations;
        const char* tools[] = {"stickerEditRectangleToolButton", "stickerEditEllipseToolButton",
                               "stickerEditLineToolButton",      "stickerEditArrowToolButton",
                               "stickerEditFreehandToolButton",  "stickerEditCoverToolButton",
                               "stickerEditMosaicToolButton"};
        QImage beforeUndo;
        QVector<Annotation> beforeUndoAnnotations;
        for (int index = 0; index < 3; ++index)
        {
            QVERIFY(manager.create(image, {80 + index * 40, 90 + index * 40}).success);
            auto window = manager.windows().last();
            window->setScale(0.75 + index * 0.5, window->pos());
            window->setEditing(true);
            stickerClick(*window, "stickerEditFreehandToolButton");
            chooseAnnotationColor(*window, index, true);
            chooseAnnotationWidth(*window, sampleLineWidths[index], true);
            window->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"))
                ->setCurrentIndex(index);
            for (const char* tool : tools)
            {
                stickerClick(*window, tool);
                stickerDrag(*window, {100, 100}, {180, 160});
            }
            stickerClick(*window, "stickerEditTextToolButton");
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, {100, 170});
            window->findChild<AnnotationTextEdit*>()->setPlainText(
                QStringLiteral("隐藏往返 %1\nABC 123").arg(index));
            window->setEditing(false);
            QCOMPARE(window->annotations().size(), qsizetype(8));
            if (index == 1)
            {
                beforeUndo = window->renderedImage();
                beforeUndoAnnotations = window->annotations();
                window->setEditing(true);
                stickerClick(*window, "stickerEditUndoAnnotationButton");
                window->setEditing(false);
            }
            window->setSaved(index == 0);
            geometries.append(window->geometry());
            scales.append(window->scale());
            images.append(window->image());
            rendered.append(window->renderedImage());
            annotations.append(window->annotations());
        }
        const auto windows = manager.windows();
        const auto verifyState = [&](bool visible)
        {
            QCOMPARE(manager.count(), 3);
            for (int index = 0; index < windows.size(); ++index)
            {
                const auto& window = windows[index];
                QVERIFY(window);
                QCOMPARE(window->isVisible(), visible);
                QVERIFY(!window->isEditing());
                QCOMPARE(window->geometry(), geometries[index]);
                QCOMPARE(window->scale(), scales[index]);
                QCOMPARE(window->image(), images[index]);
                QCOMPARE(window->renderedImage(), rendered[index]);
                compareAnnotations(window->annotations(), annotations[index]);
                QCOMPARE(window->isSaved(), index == 0);
                QVERIFY(window->testAttribute(Qt::WA_ShowWithoutActivating));
                QVERIFY(window->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
            }
        };
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            manager.hideAll();
            verifyState(false);
            manager.hideAll();
            verifyState(false);
            manager.restoreAll();
            verifyState(true);
            manager.restoreAll();
            verifyState(true);
        }
        windows[1]->setEditing(true);
        auto* redo =
            windows[1]->findChild<QPushButton*>(QStringLiteral("stickerEditRedoAnnotationButton"));
        QVERIFY(redo && redo->isEnabled());
        stickerClick(*windows[1], "stickerEditRedoAnnotationButton");
        compareAnnotations(windows[1]->annotations(), beforeUndoAnnotations);
        QCOMPARE(windows[1]->renderedImage(), beforeUndo);
        stickerClick(*windows[1], "stickerEditUndoAnnotationButton");
        QCOMPARE(windows[1]->renderedImage(), rendered[1]);
    }
    void stickerHideCommitsPendingTextWithoutConfirmation()
    {
        using namespace waibusnap;
        int confirms = 0, saves = 0;
        StickerActions actions;
        actions.confirmClose = [&](QWidget*)
        {
            ++confirms;
            return StickerCloseDecision::Cancel;
        };
        actions.confirmQuit = [&](int)
        {
            ++confirms;
            return StickerQuitDecision::Cancel;
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++saves;
            return QString();
        };
        StickerManager manager(nullptr, actions);
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {80, 90}, true).success);
        auto window = manager.windows().first();
        window->setEditing(true);
        stickerClick(*window, "stickerEditTextToolButton");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, {100, 100});
        auto* editor = window->findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->setPlainText(QStringLiteral("隐藏前提交\n中文 ABC 123"));
        const QRect geometry = window->geometry();
        QVERIFY(window->annotations().isEmpty());
        QVERIFY(window->isSaved());
        manager.hideAll();
        QVERIFY(!window->isVisible());
        QVERIFY(!window->isEditing());
        QVERIFY(!editor->isVisible());
        QCOMPARE(window->annotations().size(), qsizetype(1));
        QCOMPARE(window->annotations().first().type, AnnotationType::Text);
        QCOMPARE(window->annotations().first().text, QStringLiteral("隐藏前提交\n中文 ABC 123"));
        // 文本提交沿用 dirty 语义；隐藏本身既不保存也不关闭。
        QVERIFY(!window->isSaved());
        const QImage rendered = window->renderedImage();
        manager.hideAll();
        manager.restoreAll();
        QVERIFY(window->isVisible());
        QVERIFY(!window->isEditing());
        QCOMPARE(window->geometry(), geometry);
        QCOMPARE(window->renderedImage(), rendered);
        QCOMPARE(window->annotations().size(), qsizetype(1));
        QVERIFY(!window->isSaved());
        QCOMPARE(confirms, 0);
        QCOMPARE(saves, 0);
    }
    void stickerHiddenCloseAndCleanupDoNotResurrect()
    {
        using namespace waibusnap;
        StickerManager manager;
        const QImage image = sampleFrame().pixels;
        for (int index = 0; index < 3; ++index)
            QVERIFY(manager.create(image, {80 + index * 20, 90}).success);
        const auto windows = manager.windows();
        manager.hideAll();
        windows[0]->forceClose();
        QCOMPARE(manager.count(), 2);
        // 延迟销毁前仍有指针的已关闭窗口也不能恢复。
        QVERIFY(windows[0] && !windows[0]->isVisible());
        manager.restoreAll();
        QVERIFY(!windows[0]->isVisible());
        QVERIFY(windows[1]->isVisible());
        QVERIFY(windows[2]->isVisible());
        QTRY_VERIFY(!windows[0]);
        windows[2]->hide();
        bool closingGuardChecked = false;
        connect(windows[1], &StickerWindow::closed, this,
                [&]
                {
                    manager.hideAll();
                    manager.restoreAll();
                    QVERIFY(windows[2] && !windows[2]->isVisible());
                    closingGuardChecked = true;
                });
        manager.closeAll();
        QVERIFY(closingGuardChecked);
        QCOMPARE(manager.count(), 0);
        for (const auto& window : windows)
            QVERIFY(!window);
        manager.restoreAll();
        manager.hideAll();
        QCOMPARE(manager.count(), 0);
    }
    void stickerHiddenQuitDecisionsAndReentrancy_data()
    {
        QTest::addColumn<int>("decision");
        QTest::newRow("cancel") << 0;
        QTest::newRow("save-all") << 1;
        QTest::newRow("discard-all") << 2;
    }
    void stickerHiddenQuitDecisionsAndReentrancy()
    {
        using namespace waibusnap;
        QFETCH(int, decision);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        StickerManager* current = nullptr;
        int summaries = 0, panels = 0;
        const auto verifyGuard = [&]
        {
            current->hideAll();
            current->restoreAll();
            const auto windows = current->windows();
            QCOMPARE(windows.size(), qsizetype(3));
            QVERIFY(!windows[0]->isVisible());
            QVERIFY(!windows[1]->isVisible());
            QVERIFY(windows[2]->isVisible());
        };
        StickerActions actions;
        actions.confirmQuit = [&](int count)
        {
            ++summaries;
            // 汇总包含隐藏未保存项，跳过隐藏已保存项。
            if (count != 2)
                QTest::qFail("退出汇总未包含隐藏未保存贴图", __FILE__, __LINE__);
            verifyGuard();
            return static_cast<StickerQuitDecision>(decision);
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            verifyGuard();
            return directory.filePath(QStringLiteral("%1.png").arg(++panels));
        };
        StickerManager manager(nullptr, actions);
        current = &manager;
        const QImage image = sampleFrame().pixels;
        QVERIFY(manager.create(image, {80, 90}).success);
        QVERIFY(manager.create(image, {100, 110}, true).success);
        manager.hideAll();
        QVERIFY(manager.create(image, {120, 130}).success);
        const auto windows = manager.windows();
        QCOMPARE(manager.resolveUnsavedForQuit(), decision != 0);
        QCOMPARE(summaries, 1);
        QCOMPARE(panels, decision == 1 ? 2 : 0);
        QVERIFY(!windows[0]->isVisible());
        QVERIFY(!windows[1]->isVisible());
        QVERIFY(windows[2]->isVisible());
        QCOMPARE(windows[0]->isSaved(), decision == 1);
        QVERIFY(windows[1]->isSaved());
        QCOMPARE(windows[2]->isSaved(), decision == 1);
        if (decision == 0)
        {
            manager.restoreAll();
            for (const auto& window : windows)
                QVERIFY(window->isVisible());
            manager.hideAll();
            for (const auto& window : windows)
                QVERIFY(!window->isVisible());
        }
        else
        {
            manager.closeAll();
            manager.restoreAll();
            QCOMPARE(manager.count(), 0);
            for (const auto& window : windows)
                QVERIFY(!window);
        }
    }
    void stickerHiddenQuitDefaultDialogStillConfirms()
    {
        using namespace waibusnap;
        StickerActions actions;
        actions.loadCloseReminder = [] { return false; };
        StickerManager manager(nullptr, actions);
        QVERIFY(manager.create(sampleFrame().pixels, {80, 90}).success);
        auto window = manager.windows().first();
        manager.hideAll();
        QString text;
        QStringList labels;
        QTimer::singleShot(0, this,
                           [&]
                           {
                               auto* dialog =
                                   qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                               if (!dialog)
                                   return;
                               QVERIFY(!dialog->checkBox());
                               text = dialog->text();
                               for (auto* button : dialog->buttons())
                                   labels.append(button->text());
                               dialog->reject();
                           });
        QVERIFY(!manager.resolveUnsavedForQuit());
        QVERIFY(text.contains(QStringLiteral("1 张未保存贴图")));
        QVERIFY(labels.contains(QStringLiteral("取消退出")));
        QVERIFY(labels.contains(QStringLiteral("逐张保存")));
        QVERIFY(labels.contains(QStringLiteral("全部放弃")));
        QCOMPARE(manager.count(), 1);
        QVERIFY(window && !window->isVisible() && !window->isSaved());
    }
    void stickerHiddenRecoveryPreservesVisibility()
    {
        using namespace waibusnap;
        StickerManager manager;
        const QImage image = sampleFrame().pixels;
        QVERIFY(manager.create(image, {80, 90}, true).success);
        auto window = manager.windows().first();
        window->setScale(1.75, window->pos());
        const QImage original = window->image();
        const QImage rendered = window->renderedImage();
        manager.hideAll();
        const auto screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        for (bool notification : {false, true})
        {
            window->move(-10000, -10000);
            if (notification)
            {
                // 只模拟屏幕移除通知，真实热插拔保留人工验收。
                QVERIFY(QMetaObject::invokeMethod(qApp, "screenRemoved", Qt::DirectConnection,
                                                  Q_ARG(QScreen*, nullptr)));
            }
            else
                manager.recoverWindows();
            QTRY_VERIFY(screen->availableGeometry().contains(window->pos()));
            QVERIFY(!window->isVisible());
            QCOMPARE(window->scale(), qreal(1.75));
            QCOMPARE(window->image(), original);
            QCOMPARE(window->renderedImage(), rendered);
            QVERIFY(window->isSaved());
        }
        const QRect geometry = window->geometry();
        manager.restoreAll();
        QVERIFY(window->isVisible());
        QCOMPARE(window->geometry(), geometry);
        QVERIFY(screen->availableGeometry().contains(window->pos()));
    }
    void stickerMenuAvailabilityAndOverlayIsolation()
    {
        using namespace waibusnap;
        ApplicationController controller(*qApp, {});
        auto* menu = controller.menu();
        QCOMPARE(menu->actions().size(), qsizetype(7));
        QCOMPARE(menu->actions()[0]->text(), QStringLiteral("截图"));
        QCOMPARE(menu->actions()[2]->text(), QStringLiteral("剪贴板贴图"));
        QCOMPARE(menu->actions()[2]->objectName(), QStringLiteral("clipboardStickerAction"));
        QVERIFY(menu->actions()[2]->isEnabled());
        QCOMPARE(menu->actions()[3]->text(), QStringLiteral("设置…"));
        QCOMPARE(menu->actions()[6]->text(), QStringLiteral("退出"));
        auto* hide = menu->actions()[4];
        auto* restore = menu->actions()[5];
        QCOMPARE(hide->text(), QStringLiteral("隐藏全部贴图"));
        QCOMPARE(restore->text(), QStringLiteral("恢复全部贴图"));
        QCOMPARE(hide->objectName(), QStringLiteral("hideAllStickersAction"));
        QCOMPARE(restore->objectName(), QStringLiteral("restoreAllStickersAction"));
        QVERIFY(!hide->isEnabled());
        QVERIFY(!restore->isEnabled());
        const auto verifyPopup = [&](bool visible, bool hidden)
        {
            // 人为弄旧状态，证明 aboutToShow 会按实时窗口状态刷新。
            hide->setEnabled(!visible);
            restore->setEnabled(!hidden);
            menu->popup({10, 10});
            QVERIFY(menu->isVisible());
            QCOMPARE(hide->isEnabled(), visible);
            QCOMPARE(restore->isEnabled(), hidden);
            menu->hide();
        };
        verifyPopup(false, false);
        auto& manager = controller.stickers();
        const QImage image = sampleFrame().pixels;
        QVERIFY(manager.create(image, {80, 90}, true).success);
        QVERIFY(manager.create(image, {100, 110}).success);
        const auto initial = manager.windows();
        verifyPopup(true, false);
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        const QRect selection = overlay.selection();
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        hide->trigger();
        QVERIFY(!hide->isEnabled());
        QVERIFY(restore->isEnabled());
        for (const auto& window : initial)
            QVERIFY(!window->isVisible());
        verifyPopup(false, true);
        QVERIFY(manager.create(image, {120, 130}).success);
        auto fresh = manager.windows().last();
        verifyPopup(true, true);
        restore->trigger();
        QVERIFY(hide->isEnabled());
        QVERIFY(!restore->isEnabled());
        QVERIFY(fresh->isVisible());
        for (const auto& window : initial)
            QVERIFY(window->isVisible());
        hide->trigger();
        QVERIFY(!hide->isEnabled());
        QVERIFY(restore->isEnabled());
        QVERIFY(manager.create(image, {140, 150}, true).success);
        verifyPopup(true, true);
        hide->trigger();
        for (const auto& window : manager.windows())
            QVERIFY(!window->isVisible());
        QVERIFY(!hide->isEnabled());
        QVERIFY(restore->isEnabled());
        restore->trigger();
        QVERIFY(hide->isEnabled());
        QVERIFY(!restore->isEnabled());
        QVERIFY(overlay.isVisible());
        QCOMPARE(overlay.selection(), selection);
        QCOMPARE(finished.count(), 0);
        manager.closeAll();
        verifyPopup(false, false);
        QCOMPARE(manager.count(), 0);
    }
    void clipboardStickerImageLifecycle_data()
    {
        QTest::addColumn<qreal>("sourceDpr");
        QTest::newRow("one") << qreal(1);
        QTest::newRow("fractional") << qreal(1.5);
        QTest::newRow("two") << qreal(2);
    }
    void clipboardStickerImageLifecycle()
    {
        using namespace waibusnap;
        QFETCH(qreal, sourceDpr);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RunOptions options;
        options.settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        QImage source = mosaicTestImage({120, 80});
        source.setDevicePixelRatio(sourceDpr);
        const QImage before = source;
        int reads = 0, summaries = 0, panels = 0;
        StickerActions actions;
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++panels;
            return temporary.filePath(QStringLiteral("剪贴板图片.png"));
        };
        actions.confirmQuit = [&](int count)
        {
            summaries = count;
            return StickerQuitDecision::Cancel;
        };
        ApplicationController controller(*qApp, options, actions,
                                         {[&]
                                          {
                                              ++reads;
                                              return source;
                                          }});
        auto* action = clipboardAction(controller);
        QVERIFY(action && action->isEnabled());
        const QPoint cursor = QCursor::pos();
        QScreen* screen = QGuiApplication::screenAt(cursor);
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QSize expectedSize = stickerWindowSize(source.size(), 1, screen->devicePixelRatio());
        const auto expected = [&](quint64 sequence)
        { return clipboardStickerPosition(screen->availableGeometry(), expectedSize, sequence); };
        action->trigger();
        QCOMPARE(reads, 1);
        QCOMPARE(controller.stickers().count(), 1);
        auto first = controller.stickers().windows().first();
        QVERIFY(first && first->isVisible());
        QCOMPARE(first->size(), expectedSize);
        QCOMPARE(first->pos(), expected(0));
        QCOMPARE(first->image().devicePixelRatio(), qreal(1));
        compareImagePixels(first->image(), before);
        QCOMPARE(source, before);
        QVERIFY(!first->isSaved());
        QVERIFY(QDir(temporary.path()).entryList(QDir::Files).isEmpty());
        QVERIFY(!QFile::exists(options.metricsFile));
        action->trigger();
        QCOMPARE(reads, 2);
        QCOMPARE(controller.stickers().count(), 2);
        auto second = controller.stickers().windows().last();
        QVERIFY(second && second->isVisible());
        QCOMPARE(second->pos(), expected(1));
        QCOMPARE(second->pos(), first->pos() + QPoint(24, 24));
        controller.menu()->findChild<QAction*>(QStringLiteral("hideAllStickersAction"))->trigger();
        QVERIFY(!first->isVisible() && !second->isVisible());
        controller.quit();
        QCOMPARE(summaries, 2);
        QVERIFY(first && second && !first->isVisible() && !second->isVisible());
        QVERIFY(action->isEnabled());
        controller.menu()
            ->findChild<QAction*>(QStringLiteral("restoreAllStickersAction"))
            ->trigger();
        QVERIFY(first->isVisible() && second->isVisible());
        QVERIFY(first->saveImage());
        QCOMPARE(panels, 1);
        QVERIFY(first->isSaved());
        compareImagePixels(QImage(temporary.filePath(QStringLiteral("剪贴板图片.png"))), before);
        controller.quit();
        QCOMPARE(summaries, 1);
        QCOMPARE(controller.stickers().count(), 2);
        QVERIFY(!QFile::exists(options.metricsFile));
    }
    void clipboardStickerEmptyToastReuseTimeoutAndCleanup()
    {
        using namespace waibusnap;
        QWidget editor;
        auto* input = new QLineEdit(&editor);
        editor.show();
        editor.activateWindow();
        input->setFocus();
        QTRY_VERIFY(input->hasFocus());
        QTemporaryDir temporary;
        RunOptions options;
        options.settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        int reads = 0;
        QPointer<QLabel> toast;
        {
            ApplicationController controller(*qApp, options, {},
                                             {[&]
                                              {
                                                  ++reads;
                                                  return QImage();
                                              }});
            QVERIFY(
                controller.stickers().create(mosaicTestImage({100, 80}), {80, 90}, true).success);
            // 先完成已有贴图夹具，再固定焦点；只测剪贴板失败提示的影响。
            QCoreApplication::processEvents();
            editor.activateWindow();
            input->setFocus();
            QTest::qWait(50);
            QTRY_VERIFY(input->hasFocus());
            QWidget* focused = QApplication::focusWidget();
            QWidget* active = QApplication::activeWindow();
            auto* action = clipboardAction(controller);
            QVERIFY(action && action->isEnabled());
            action->trigger();
            QCOMPARE(reads, 1);
            QCOMPARE(controller.stickers().count(), 1);
            toast = clipboardToast();
            QVERIFY(toast && toast->isVisible());
            QCOMPARE(toast->text(), clipboardToastText);
            QVERIFY(toast->windowFlags().testFlag(Qt::FramelessWindowHint));
            QVERIFY(toast->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            QVERIFY(toast->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
            QVERIFY(toast->testAttribute(Qt::WA_ShowWithoutActivating));
            QCOMPARE(toast->focusPolicy(), Qt::NoFocus);
            QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            QVERIFY(screen && screen->availableGeometry().contains(toast->geometry()));
            QCoreApplication::processEvents();
            if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
            {
                // Qt offscreen 显示非 ToolTip 窗口必发焦点事件，忽略拒绝焦点旗标。
                // 明确断言该后端行为后建立输入场景；真实 Cocoa 保留首次显示断言。
                QCOMPARE(QGuiApplication::focusWindow(), toast->windowHandle());
                editor.activateWindow();
                input->setFocus();
                QTRY_VERIFY(input->hasFocus());
            }
            else
            {
                QCOMPARE(QApplication::focusWidget(), focused);
                QCOMPARE(QApplication::activeWindow(), active);
            }
            QTest::qWait(1500);
            toast->setText(QStringLiteral("旧提示"));
            action->trigger();
            QCOMPARE(clipboardToast(), toast.data());
            QCOMPARE(toast->text(), clipboardToastText);
            QCOMPARE(reads, 2);
            QCOMPARE(controller.stickers().count(), 1);
            QTest::qWait(1500);
            QVERIFY(toast->isVisible());
            QCOMPARE(QApplication::focusWidget(), focused);
            QCOMPARE(QApplication::activeWindow(), active);
            QTRY_VERIFY_WITH_TIMEOUT(!toast->isVisible(), 1800);
            QCOMPARE(QApplication::focusWidget(), focused);
            QCOMPARE(QApplication::activeWindow(), active);
            QVERIFY(!QFile::exists(options.metricsFile));
            action->trigger();
            QVERIFY(toast->isVisible());
        }
        QVERIFY(!toast);
        QVERIFY(!clipboardToast());
    }
    void clipboardStickerToastClampsAtScreenEdges()
    {
        using namespace waibusnap;
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();
        ClipboardStickerToast toast;
        for (QPoint cursor : {available.topLeft(), available.topRight(), available.bottomLeft(),
                              available.bottomRight()})
        {
            toast.showMessage(clipboardToastText, cursor, available);
            QVERIFY(toast.isVisible());
            QVERIFY(available.contains(toast.geometry()));
            QCOMPARE(toast.text(), clipboardToastText);
        }
    }
    void clipboardStickerCleanupDisablesAction()
    {
        using namespace waibusnap;
        int reads = 0;
        ApplicationController controller(*qApp, {}, {},
                                         {[&]
                                          {
                                              ++reads;
                                              return QImage();
                                          }});
        auto* action = clipboardAction(controller);
        action->trigger();
        QPointer<QLabel> toast = clipboardToast();
        QVERIFY(toast && toast->isVisible());
        QVERIFY(QMetaObject::invokeMethod(qApp, "aboutToQuit", Qt::DirectConnection));
        QVERIFY(!toast);
        QVERIFY(!action->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(controller.menu(), "aboutToShow", Qt::DirectConnection));
        QVERIFY(!action->isEnabled());
        action->trigger();
        QCOMPARE(reads, 1);
        QCOMPARE(controller.stickers().count(), 0);
    }
    void clipboardStickerFailureDoesNotAdvanceCascade()
    {
        using namespace waibusnap;
        QImage source;
        ApplicationController controller(*qApp, {}, {}, {[&] { return source; }});
        auto* action = clipboardAction(controller);
        action->trigger();
        QCOMPARE(controller.stickers().count(), 0);
        QVERIFY(clipboardToast() && clipboardToast()->isVisible());
        source = mosaicTestImage({120, 80});
        action->trigger();
        QCOMPARE(controller.stickers().count(), 1);
        QVERIFY(!clipboardToast()->isVisible());
        auto first = controller.stickers().windows().first();
        const QPoint base = first->pos();
        source = {};
        action->trigger();
        QCOMPARE(controller.stickers().count(), 1);
        source = mosaicTestImage({120, 80});
        action->trigger();
        QCOMPARE(controller.stickers().count(), 2);
        QCOMPARE(controller.stickers().windows().last()->pos(), base + QPoint(24, 24));
    }
    void clipboardStickerMenuSessionAndQuitGuards()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        RunOptions options;
        options.settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        int reads = 0, summaries = 0;
        bool quitGuarded = false;
        ApplicationController* current = nullptr;
        StickerActions actions;
        actions.confirmQuit = [&](int count)
        {
            ++summaries;
            quitGuarded = count == 1 && !clipboardAction(*current)->isEnabled();
            quitGuarded &=
                QMetaObject::invokeMethod(current->menu(), "aboutToShow", Qt::DirectConnection);
            quitGuarded &= !clipboardAction(*current)->isEnabled();
            clipboardAction(*current)->trigger();
            quitGuarded &= reads == 1;
            return StickerQuitDecision::Cancel;
        };
        ApplicationController controller(*qApp, options, actions,
                                         {[&]
                                          {
                                              ++reads;
                                              return mosaicTestImage({120, 80});
                                          }});
        current = &controller;
        auto* action = clipboardAction(controller);
        action->setEnabled(false);
        controller.menu()->popup({10, 10});
        QVERIFY(action->isEnabled());
        controller.menu()->hide();
        action->trigger();
        controller.quit();
        QCOMPARE(summaries, 1);
        QVERIFY(quitGuarded);
        QVERIFY(action->isEnabled());
        bool checked = false, sessionGuarded = false;
        QTimer::singleShot(
            0, &controller,
            [&]
            {
                sessionGuarded = controller.active() && !action->isEnabled() &&
                                 !controller.menu()->actions()[3]->isEnabled();
                sessionGuarded &=
                    ApplicationControllerTestAccess::watchdog(controller).isActive() &&
                    ApplicationControllerTestAccess::watchdog(controller).interval() == 5000;
                sessionGuarded &= QMetaObject::invokeMethod(controller.menu(), "aboutToShow",
                                                            Qt::DirectConnection);
                sessionGuarded &= !action->isEnabled();
                action->trigger();
                sessionGuarded &= reads == 1;
                checked = true;
                sessionGuarded &=
                    QMetaObject::invokeMethod(qApp, "screenRemoved", Qt::DirectConnection,
                                              Q_ARG(QScreen*, QGuiApplication::primaryScreen()));
                if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                    dialog->accept();
            });
        controller.trigger(QStringLiteral("tray"));
        QTRY_VERIFY(checked);
        QVERIFY(sessionGuarded);
        QVERIFY(!controller.active());
        QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
        QVERIFY(action->isEnabled());
        QVERIFY(controller.menu()->actions()[3]->isEnabled());
        QCOMPARE(controller.stickers().count(), 1);
    }
    void controllerSessionWatchdogRecovery_data()
    {
        QTest::addColumn<int>("overlayState");
        QTest::newRow("missing") << 0;
        QTest::newRow("hidden") << 1;
        QTest::newRow("destroyed") << 2;
    }
    void controllerSessionWatchdogRecovery()
    {
        using namespace waibusnap;
        QFETCH(int, overlayState);
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        QPointer<SelectionOverlay> overlay;
        if (overlayState)
            overlay = new SelectionOverlay(sampleFrame());
        ApplicationControllerTestAccess::seedSession(controller, overlay, 21000);
        if (overlayState == 2)
            delete overlay.data();
        ApplicationControllerTestAccess::observe(controller);
        const auto oldToken = ApplicationControllerTestAccess::token(controller);
        QVERIFY(!controller.menu()->actions()[3]->isEnabled());
        QVERIFY(!clipboardAction(controller)->isEnabled());
        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression(QStringLiteral("截图会话异常残留，已自动复位：.*")));
        // 直接投递真实定时器信号，避免每个数据行等待 20 秒；年龄由测试缝构造。
        QVERIFY(QMetaObject::invokeMethod(&ApplicationControllerTestAccess::watchdog(controller),
                                          "timeout", Qt::DirectConnection));
        QVERIFY(!controller.active());
        QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
        QVERIFY(ApplicationControllerTestAccess::observationCleared(controller));
        QVERIFY(ApplicationControllerTestAccess::token(controller) > oldToken);
        QVERIFY(controller.menu()->actions()[3]->isEnabled());
        QVERIFY(clipboardAction(controller)->isEnabled());
        if (overlay)
            QTRY_VERIFY(!overlay);
        QFile metrics(options.metricsFile);
        QVERIFY(metrics.open(QIODevice::ReadOnly));
        const auto record = QJsonDocument::fromJson(metrics.readLine()).object();
        QCOMPARE(record.value(QStringLiteral("outcome")).toInt(), 12);
        QVERIFY(metrics.atEnd());
        // 旧捕获回调到达下一会话时仍被 token 拒绝，不结束或污染新会话。
        ApplicationControllerTestAccess::seedSession(controller, nullptr);
        ApplicationControllerTestAccess::staleCapture(controller, oldToken);
        QVERIFY(controller.active());
        QVERIFY(ApplicationControllerTestAccess::watchdog(controller).isActive());
        QVERIFY(metrics.atEnd());
    }
    void controllerNativeInvisibleRegression_data()
    {
        QTest::addColumn<bool>("available");
        QTest::newRow("known-off-screen") << true;
        QTest::newRow("query-unavailable") << false;
    }
    void controllerNativeInvisibleRegression()
    {
        using namespace waibusnap;
        QFETCH(bool, available);
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        QPointer<SelectionOverlay> overlay = new SelectionOverlay(sampleFrame());
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::visibilityState(controller, available, false);
        int attempts = 0;
        bool activated = false;
        ApplicationControllerTestAccess::recovery(controller,
                                                  [&](QWindow*, bool activate)
                                                  {
                                                      activated = activate;
                                                      ++attempts;
                                                      return true;
                                                  });
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(
                                               "截图触发被忽略：.*onScreen=0 windowNumber=67471 "
                                               "isOnActiveSpace=0 occlusionState=0.*")));
        controller.trigger(QStringLiteral("hotkey"));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 "截图会话看门狗发现真实在屏异常：.*onScreen=0 windowNumber=67471 "
                                 "isOnActiveSpace=0 occlusionState=0.*")));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QCOMPARE(attempts, 1);
        QVERIFY(activated);
        QVERIFY(controller.active());
        // 老会话首次消失仍有独立宽限；19 秒不能提前复位。
        ApplicationControllerTestAccess::ageInvisible(controller, 19000);
        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression(QStringLiteral("截图会话看门狗发现真实在屏异常：.*")));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QVERIFY(controller.active());
        ApplicationControllerTestAccess::ageInvisible(controller, 20001);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 "截图会话异常残留，已自动复位：.*overlayVisible=true.*onScreen=0 "
                                 "windowNumber=67471 isOnActiveSpace=0 occlusionState=0.*")));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QVERIFY(!controller.active());
        QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
        QVERIFY(controller.menu()->actions()[3]->isEnabled());
        QVERIFY(clipboardAction(controller)->isEnabled());
        QTRY_VERIFY(!overlay);
        QFile metrics(options.metricsFile);
        QVERIFY(metrics.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(metrics.readLine())
                     .object()
                     .value(QStringLiteral("outcome"))
                     .toInt(),
                 12);
        QVERIFY(metrics.atEnd());
    }
    void controllerNativeVisibilityRecoversAndReadies()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        auto* overlay = new SelectionOverlay(sampleFrame());
        overlay->show();
        overlay->activateWindow();
        QCoreApplication::processEvents();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::visibility(controller, false);
        int attempts = 0;
        ApplicationControllerTestAccess::recovery(controller,
                                                  [&](QWindow*, bool)
                                                  {
                                                      ++attempts;
                                                      return true;
                                                  });
        ApplicationControllerTestAccess::presentation(controller);
        QVERIFY(ApplicationControllerTestAccess::captureTimeoutActive(controller));
        QCOMPARE(attempts, 1);
        // 模拟自愈后下一 tick 查到窗口，清除连续异常起点。
        ApplicationControllerTestAccess::visibility(controller, true);
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QVERIFY(controller.active());
        QVERIFY(ApplicationControllerTestAccess::captureTimeoutActive(controller));
        QTest::ignoreMessage(QtInfoMsg,
                             QRegularExpression(QStringLiteral(
                                 "覆盖层可交互就绪（停止超时保护）.*onScreen=1 windowNumber=67471 "
                                 "isOnActiveSpace=0 occlusionState=0.*")));
        ApplicationControllerTestAccess::presentation(controller);
        QVERIFY(!ApplicationControllerTestAccess::captureTimeoutActive(controller));
        QVERIFY(!QFile::exists(options.metricsFile));
    }
    void controllerVisibilityModalAndPanelGuard_data()
    {
        QTest::addColumn<bool>("panel");
        QTest::newRow("modal") << false;
        QTest::newRow("native-panel-without-qt-modal") << true;
    }
    void controllerVisibilityModalAndPanelGuard()
    {
        using namespace waibusnap;
        QFETCH(bool, panel);
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        auto* overlay = new SelectionOverlay(sampleFrame());
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        ApplicationControllerTestAccess::visibility(controller, false);
        ApplicationControllerTestAccess::ageInvisible(controller, 30000);
        int attempts = 0;
        ApplicationControllerTestAccess::recovery(controller,
                                                  [&](QWindow*, bool)
                                                  {
                                                      ++attempts;
                                                      return true;
                                                  });
        QDialog dialog;
        if (panel)
            OverlayScrollTestAccess::panel(*overlay, true);
        else
        {
            dialog.setWindowModality(Qt::ApplicationModal);
            dialog.show();
        }
        ApplicationControllerTestAccess::watchdogCheck(controller);
        ApplicationControllerTestAccess::eventRecovery(controller);
        QCOMPARE(attempts, 0);
        QVERIFY(controller.active());
        dialog.hide();
        OverlayScrollTestAccess::panel(*overlay, false);
        QTest::ignoreMessage(
            QtWarningMsg, QRegularExpression(QStringLiteral("截图会话看门狗发现真实在屏异常：.*")));
        ApplicationControllerTestAccess::watchdogCheck(controller);
        QCOMPARE(attempts, 1);
        QVERIFY(controller.active());
    }
    void controllerVisibleSessionPressure()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        ApplicationControllerTestAccess::visibility(controller, true);
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            QPointer<SelectionOverlay> overlay = new SelectionOverlay(sampleFrame());
            overlay->show();
            ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
            ApplicationControllerTestAccess::watchdogCheck(controller);
            QVERIFY(controller.active());
            ApplicationControllerTestAccess::cancelNormal(controller);
            QVERIFY(!controller.active());
            QVERIFY(ApplicationControllerTestAccess::timersStopped(controller));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QVERIFY(!overlay);
        }
        QFile metrics(options.metricsFile);
        QVERIFY(metrics.open(QIODevice::ReadOnly));
        int records = 0;
        while (!metrics.atEnd())
        {
            const auto record = QJsonDocument::fromJson(metrics.readLine()).object();
            QCOMPARE(record.value(QStringLiteral("outcome")).toInt(), 2);
            ++records;
        }
        QCOMPARE(records, 100);
    }
    void controllerSessionWatchdogGuards_data()
    {
        QTest::addColumn<int>("state");
        QTest::newRow("inactive") << 0;
        QTest::newRow("capture-grace") << 1;
        QTest::newRow("visible-overlay") << 2;
        QTest::newRow("modal-dialog") << 3;
    }
    void controllerSessionWatchdogGuards()
    {
        using namespace waibusnap;
        QFETCH(int, state);
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        QPointer<SelectionOverlay> overlay;
        if (state == 2)
        {
            overlay = new SelectionOverlay(sampleFrame());
            overlay->show();
        }
        if (state)
            ApplicationControllerTestAccess::seedSession(controller, overlay,
                                                         state == 1 ? 19000 : 30000);
        if (state == 2)
            ApplicationControllerTestAccess::visibility(controller, true);
        QDialog dialog;
        if (state == 3)
        {
            dialog.setWindowModality(Qt::ApplicationModal);
            dialog.show();
            QCOMPARE(QApplication::activeModalWidget(), &dialog);
        }
        QVERIFY(QMetaObject::invokeMethod(&ApplicationControllerTestAccess::watchdog(controller),
                                          "timeout", Qt::DirectConnection));
        QCOMPARE(controller.active(), state != 0);
        QCOMPARE(ApplicationControllerTestAccess::watchdog(controller).isActive(), state != 0);
        QVERIFY(!QFile::exists(options.metricsFile));
        if (overlay)
            QVERIFY(overlay->isVisible());
    }
    void controllerFailureHidesOverlayBeforeDialog()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        ApplicationController controller(*qApp, options);
        auto* overlay = new SelectionOverlay(sampleFrame());
        overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay, 30000);
        bool checked = false, hidden = false, locked = false;
        QTimer::singleShot(0, &controller,
                           [&]
                           {
                               if (auto* dialog = QApplication::activeModalWidget())
                               {
                                   checked = true;
                                   hidden = !overlay->isVisible();
                                   QMetaObject::invokeMethod(
                                       &ApplicationControllerTestAccess::watchdog(controller),
                                       "timeout", Qt::DirectConnection);
                                   locked = controller.active();
                                   dialog->close();
                               }
                           });
        ApplicationControllerTestAccess::fail(controller);
        QVERIFY(checked);
        QVERIFY(hidden);
        QVERIFY(locked);
        QVERIFY(!controller.active());
        QVERIFY(controller.menu()->actions()[3]->isEnabled());
        QVERIFY(clipboardAction(controller)->isEnabled());
    }
    void controllerQuitDialogHidesAndRestoresOverlay_data()
    {
        QTest::addColumn<int>("state");
        QTest::newRow("visible-cancel") << 0;
        QTest::newRow("already-hidden") << 1;
        QTest::newRow("session-ended-during-dialog") << 2;
    }
    void controllerQuitDialogHidesAndRestoresOverlay()
    {
        using namespace waibusnap;
        QFETCH(int, state);
        QTemporaryDir temporary;
        RunOptions options;
        options.metricsFile = temporary.filePath(QStringLiteral("sessions.jsonl"));
        QPointer<SelectionOverlay> overlay;
        bool checked = false, hidden = false;
        StickerActions actions;
        actions.confirmQuit = [&](int)
        {
            checked = true;
            hidden = overlay && !overlay->isVisible();
            if (state == 2)
                QMetaObject::invokeMethod(qApp, "screenRemoved", Qt::DirectConnection,
                                          Q_ARG(QScreen*, QGuiApplication::primaryScreen()));
            return StickerQuitDecision::Cancel;
        };
        ApplicationController controller(*qApp, options, actions);
        QVERIFY(controller.stickers().create(sampleFrame().pixels, {30, 40}).success);
        overlay = new SelectionOverlay(sampleFrame());
        if (state != 1)
            overlay->show();
        ApplicationControllerTestAccess::seedSession(controller, overlay);
        controller.quit();
        QVERIFY(checked);
        QVERIFY(hidden);
        QCOMPARE(controller.active(), state != 2);
        QVERIFY(overlay);
        QCOMPARE(overlay->isVisible(), state == 0);
    }
    void clipboardStickerReadOnly_data()
    {
        QTest::addColumn<bool>("hasImage");
        QTest::newRow("bitmap") << true;
        QTest::newRow("text") << false;
    }
    void clipboardStickerReadOnly()
    {
        using namespace waibusnap;
        QFETCH(bool, hasImage);
        ClipboardRestore restore;
        QClipboard* clipboard = QGuiApplication::clipboard();
        const QImage expected = mosaicTestImage({120, 80});
        const QString text = QStringLiteral("只有文字，不转换为图片");
        if (hasImage)
            clipboard->setImage(expected);
        else
            clipboard->setText(text);
        QCoreApplication::processEvents();
        const bool real = hasImage ? !clipboard->image().isNull() : clipboard->text() == text;
        if (qEnvironmentVariableIsSet("WAIBUSNAP_REQUIRE_SYSTEM_CLIPBOARD"))
            QVERIFY2(real, "本机真实剪贴板必须可用，不得切换替身");
        ClipboardActions input;
        int reads = 0;
        const QImage stored = hasImage ? expected : QImage();
        if (!real)
        {
            qInfo("此平台剪贴板不可用，使用只读替身并保留像素 / 内容不变断言。");
            input.loadImage = [&]
            {
                ++reads;
                return stored;
            };
        }
        const QImage before = real && hasImage ? clipboard->image() : stored;
        const QString beforeText = real && !hasImage ? clipboard->text() : text;
        QSignalSpy changes(clipboard, &QClipboard::dataChanged);
        ApplicationController controller(*qApp, {}, {}, input);
        clipboardAction(controller)->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(changes.size(), 0);
        QCOMPARE(controller.stickers().count(), hasImage ? 1 : 0);
        if (hasImage)
        {
            auto window = controller.stickers().windows().first();
            QVERIFY(window && window->isVisible());
            QCOMPARE(window->image().devicePixelRatio(), qreal(1));
            compareImagePixels(window->image(), expected);
            compareImagePixels(real ? clipboard->image() : stored, before);
        }
        else
        {
            QCOMPARE(real ? clipboard->text() : text, beforeText);
            QVERIFY(clipboardToast() && clipboardToast()->isVisible());
            QCOMPARE(clipboardToast()->text(), clipboardToastText);
        }
        if (!real)
            QCOMPARE(reads, 1);
    }
    void stickerEditingEntrancesGesturesAndCompactTools()
    {
        using namespace waibusnap;
        StickerManager manager;
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {40, 50}, true).success);
        auto sticker = manager.windows().first();
        const QRect before = sticker->geometry();
        stickerClick(*sticker, "stickerEditButton");
        QVERIFY(sticker->isEditing());
        QCOMPARE(sticker->geometry(), before);
        QVERIFY(!sticker->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QCOMPARE(sticker->contextMenuPolicy(), Qt::DefaultContextMenu);
        stickerDrag(*sticker, {80, 80}, {150, 150});
        QCOMPARE(sticker->geometry(), before);
        QVERIFY(sticker->annotations().isEmpty());
        QVERIFY(sticker->isSaved());
        const char* names[] = {"stickerEditRectangleToolButton", "stickerEditEllipseToolButton",
                               "stickerEditLineToolButton",      "stickerEditArrowToolButton",
                               "stickerEditFreehandToolButton",  "stickerEditTextToolButton",
                               "stickerEditCoverToolButton",     "stickerEditMosaicToolButton"};
        for (const char* name : names)
            QVERIFY(sticker->findChild<QPushButton*>(QString::fromLatin1(name)));
        for (int index = 0; index < 5; ++index)
        {
            auto* tool = sticker->findChild<QPushButton*>(QString::fromLatin1(names[index]));
            QVERIFY(tool);
            QCOMPARE(tool->focusPolicy(), Qt::NoFocus);
            stickerClick(*sticker, names[index]);
            stickerDrag(*sticker, {80, 100}, {150, 140});
            QCOMPARE(sticker->annotations().size(), qsizetype(index + 1));
            QCOMPARE(sticker->annotations().last().type, static_cast<AnnotationType>(index));
            QCOMPARE(sticker->pos(), before.topLeft());
            QVERIFY(!sticker->isSaved());
        }
        auto* colors = sticker->findChild<QWidget*>(QStringLiteral("stickerEditColorPalette"));
        auto* widths = sticker->findChild<QLabel*>(QStringLiteral("stickerEditWidthLabel"));
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        QCOMPARE(colors->findChildren<QPushButton*>().size(), qsizetype(annotationColorCount));
        QCOMPARE(widths->text(), QStringLiteral("线宽 4px"));
        QCOMPARE(sizes->count(), 3);
        QVERIFY(!sizes->isEnabled());
        QCOMPARE(sizes->focusPolicy(), Qt::NoFocus);
        for (auto* color : colors->findChildren<QPushButton*>())
            QCOMPARE(color->focusPolicy(), Qt::NoFocus);
        // 取消画笔后仍以原锚点缩放，绘图工具激活时的滚轮另有覆盖。
        stickerClick(*sticker, "stickerEditFreehandToolButton");
        const QPointF anchor = sticker->mapToGlobal(QPoint(100, 100));
        const QRect old = sticker->geometry();
        QWheelEvent wheel({100, 100}, anchor, {}, {0, 120}, Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(sticker, &wheel);
        QCOMPARE(sticker->scale(), qreal(1.25));
        QCOMPARE(
            sticker->pos(),
            anchoredStickerPosition(old.topLeft(), old.size(), sticker->size(), anchor).toPoint());
        const qreal factor =
            sticker->windowHandle()->screen()->devicePixelRatio() / sticker->scale();
        stickerClick(*sticker, "stickerEditLineToolButton");
        stickerDrag(*sticker, {100, 120}, {200, 120});
        QCOMPARE(sticker->annotations().last().first, QPointF(100, 120) * factor);
        QCOMPARE(sticker->annotations().last().last, QPointF(200, 120) * factor);
        const qsizetype count = sticker->annotations().size();
        const QRect scaled = sticker->geometry();
        stickerClick(*sticker, "stickerEditDoneButton");
        QVERIFY(!sticker->isEditing());
        QCOMPARE(sticker->geometry(), scaled);
        QVERIFY(sticker->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(sticker->testAttribute(Qt::WA_ShowWithoutActivating));
        QCOMPARE(sticker->annotations().size(), count);
        stickerDrag(*sticker, {80, 80}, {120, 110});
        QCOMPARE(sticker->pos(), scaled.topLeft() + QPoint(40, 30));
        QTest::mouseDClick(sticker, Qt::LeftButton, Qt::NoModifier, {80, 80});
        QVERIFY(sticker->isEditing());
        QTest::keyClick(sticker, Qt::Key_Escape);
        QVERIFY(!sticker->isEditing());
        QImage tiny(8, 8, QImage::Format_ARGB32_Premultiplied);
        tiny.fill(Qt::white);
        QVERIFY(manager.create(tiny, {40, 50}, true).success);
        sticker = manager.windows().last();
        sticker->setScale(0.25, sticker->pos());
        sticker->setEditing(true);
        sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        auto* more = sticker->findChild<QPushButton*>(QStringLiteral("stickerEditMoreButton"));
        QVERIFY(more && more->isVisible());
        auto* menu = sticker->findChild<QMenu*>(QStringLiteral("stickerEditMenu"));
        QCOMPARE(menu->actions().size(), qsizetype(12));
        QVERIFY(sticker->rect().contains(more->geometry()));
        auto* privacy = menu->actions()[4];
        auto* coverAction = privacy->menu()->actions()[0];
        auto* mosaicAction = privacy->menu()->actions()[1];
        // 折叠菜单保留三组及全部样式 / 输出，二级动作同步选中态。
        menu->actions()[3]->trigger();
        QVERIFY(sizes->isEnabled());
        menu->actions()[6]->menu()->actions()[2]->trigger();
        QCOMPARE(sizes->currentIndex(), 2);
        coverAction->trigger();
        QVERIFY(sticker->activeTool() == AnnotationType::Cover);
        QVERIFY(privacy->isChecked() && coverAction->isChecked());
        QVERIFY(!menu->actions()[6]->isEnabled());
        coverAction->trigger();
        QVERIFY(sticker->activeTool() == AnnotationType::Cover);
        stickerClick(*sticker, "stickerEditCoverToolButton");
        QVERIFY(!sticker->activeTool());
        QVERIFY(menu->actions()[5]->isEnabled());
        mosaicAction->trigger();
        QVERIFY(sticker->activeTool() == AnnotationType::Mosaic);
        QVERIFY(privacy->isChecked() && mosaicAction->isChecked());
        for (int index : {5, 6})
            QVERIFY(!menu->actions()[index]->isEnabled());
        auto* status = sticker->findChild<QLabel*>(QStringLiteral("stickerStatus"));
        QVERIFY(status->isVisible());
        QVERIFY(status->text().contains(QStringLiteral("高敏感内容请用实心遮盖")));
        stickerClick(*sticker, "stickerEditMosaicToolButton");
        QVERIFY(menu->actions()[5]->isEnabled());
        menu->actions().last()->trigger();
        QVERIFY(!sticker->isEditing());
    }
    void stickerTextImeEscapeAndCommitBoundaries()
    {
        using namespace waibusnap;
        StickerManager manager;
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {40, 50}, true).success);
        auto sticker = manager.windows().first();
        sticker->setEditing(true);
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
        auto* editor =
            sticker->findChild<AnnotationTextEdit*>(QStringLiteral("stickerEditTextEditor"));
        QVERIFY(editor && editor->isVisible());
        editor->setPlainText(QStringLiteral("截图说明 ABC 123\n第二行"));
        QInputMethodEvent composing(QStringLiteral("中文候选"), {});
        QApplication::sendEvent(editor, &composing);
        QVERIFY(editor->isComposing());
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(editor->isComposing());
        QVERIFY(sticker->annotations().isEmpty());
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!editor->isComposing());
        QVERIFY(editor->isVisible());
        QVERIFY(sticker->isEditing());
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!editor->isVisible());
        QVERIFY(sticker->annotations().isEmpty());
        QVERIFY(sticker->isSaved());
        QTest::keyClick(sticker, Qt::Key_Escape);
        QVERIFY(!sticker->isEditing());
        sticker->setEditing(true);
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
        editor->setPlainText(QStringLiteral("截图说明 ABC 123\n第二行"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(sticker->annotations().size(), qsizetype(1));
        QCOMPARE(sticker->annotations().last().text, QStringLiteral("截图说明 ABC 123\n第二行"));
        QVERIFY(!sticker->isSaved());
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 150});
        editor->setPlainText(QStringLiteral("   \n  "));
        stickerClick(*sticker, "stickerEditLineToolButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(1));
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 150});
        editor->setPlainText(QStringLiteral("切换提交"));
        stickerClick(*sticker, "stickerEditLineToolButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 150});
        editor->setPlainText(QStringLiteral("点击别处提交"));
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {200, 200});
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        editor->setPlainText(QStringLiteral("完成提交"));
        stickerClick(*sticker, "stickerEditDoneButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(4));
        QVERIFY(!editor->isVisible());
        QVERIFY(!sticker->isEditing());
        sticker->setEditing(true);
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
        editor->setPlainText(QStringLiteral("同值选项点击也提交"));
        auto* colors = sticker->findChild<QWidget*>(QStringLiteral("stickerEditColorPalette"));
        QVERIFY(colors);
        chooseAnnotationColor(*sticker, 0, true);
        QVERIFY(sticker->findChild<QPushButton*>("stickerEditColor0Button")->isChecked());
        QCOMPARE(sticker->annotations().size(), qsizetype(5));
        QVERIFY(!editor->isVisible());
    }
    void stickerHistoryStylesOutputAndDirtyState()
    {
        using namespace waibusnap;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage copied;
        StickerActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        StickerManager manager(nullptr, actions);
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {40, 50}, true).success);
        auto sticker = manager.windows().first();
        QCOMPARE(sticker->renderedImage().cacheKey(), image.cacheKey());
        sticker->setEditing(true);
        QTRY_VERIFY(sticker->isActiveWindow());
        stickerClick(*sticker, "stickerEditLineToolButton");
        for (int index = 0; index < 3; ++index)
        {
            chooseAnnotationColor(*sticker, index, true);
            chooseAnnotationWidth(*sticker, sampleLineWidths[index], true);
            stickerDrag(*sticker, {100, 100 + index * 30}, {220, 100 + index * 30});
            QCOMPARE(sticker->annotations().last().style.color, annotationColors()[index]);
            QCOMPARE(sticker->annotations().last().style.lineWidth, sampleLineWidths[index]);
        }
        const auto expected = renderAnnotatedSelection(image, sticker->annotations(), image.rect());
        stickerClick(*sticker, "stickerEditCopyButton");
        QCOMPARE(copied, expected);
        QCOMPARE(copied.size(), image.size());
        QCOMPARE(copied.devicePixelRatio(), qreal(1));
        QVERIFY(!sticker->isSaved());
        const QString path = directory.filePath(QStringLiteral("中文 4b.png"));
        QVERIFY(sticker->exportToPath(path));
        QVERIFY(sticker->isSaved());
        QCOMPARE(QImage(path).convertToFormat(expected.format()), expected);
        stickerDrag(*sticker, {100, 100}, {100, 100});
        QVERIFY(sticker->isSaved());
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        stickerClick(*sticker, "stickerEditFreehandToolButton");
        stickerDrag(*sticker, {100, 100}, {100, 100});
        QVERIFY(sticker->isSaved());
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        stickerClick(*sticker, "stickerEditLineToolButton");
        stickerClick(*sticker, "stickerEditUndoAnnotationButton");
        QVERIFY(!sticker->isSaved());
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        stickerClick(*sticker, "stickerEditRedoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        QCoreApplication::processEvents();
        standardKey(sticker, QKeySequence::Undo);
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        standardKey(sticker, QKeySequence::Redo);
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        stickerClick(*sticker, "stickerEditUndoAnnotationButton");
        stickerDrag(*sticker, {100, 200}, {220, 200});
        QVERIFY(!sticker->findChild<QPushButton*>(QStringLiteral("stickerEditRedoAnnotationButton"))
                     ->isEnabled());
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 250});
        auto* editor = sticker->findChild<AnnotationTextEdit*>();
        editor->setPlainText(QStringLiteral("文本自身撤销"));
        QTest::keyClicks(editor, "abc");
        standardKey(editor, QKeySequence::Undo);
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        QVERIFY(editor->isVisible());
        QTest::keyClick(editor, Qt::Key_Escape);
        stickerClick(*sticker, "stickerEditLineToolButton");
        for (int index = 0; index < 22; ++index)
            stickerDrag(*sticker, {100, 100}, {200, 100});
        const qsizetype count = sticker->annotations().size();
        for (int index = 0; index < 22; ++index)
            stickerClick(*sticker, "stickerEditUndoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), count - AnnotationHistory::capacity);
        for (int index = 0; index < 20; ++index)
            stickerClick(*sticker, "stickerEditRedoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), count);
        sticker->setEditing(false);
        const auto retained = sticker->renderedImage();
        standardKey(sticker, QKeySequence::Undo);
        QCOMPARE(sticker->renderedImage(), retained);
        sticker->setScale(4, sticker->pos());
        stickerClick(*sticker, "stickerCopyButton");
        QCOMPARE(copied, retained);
        QVERIFY(sticker->exportToPath(path));
        QVERIFY(sticker->isSaved());
        sticker->setEditing(true);
        stickerClick(*sticker, "stickerEditUndoAnnotationButton");
        QVERIFY(!sticker->isSaved());
    }
    void stickerPendingTextCommitsBeforeOutputsAndNewDrawing()
    {
        using namespace waibusnap;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage copied;
        StickerActions actions;
        actions.copyImage = [&](const QImage& output)
        {
            copied = output;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        { return directory.filePath(QStringLiteral("文本.png")); };
        StickerManager manager(nullptr, actions);
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {80, 80}, true).success);
        auto sticker = manager.windows().first();
        sticker->setEditing(true);
        stickerClick(*sticker, "stickerEditTextToolButton");
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        for (int index = 0; index < 3; ++index)
        {
            sizes->setCurrentIndex(index);
            QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
            auto* editor = sticker->findChild<AnnotationTextEdit*>();
            editor->setPlainText(QStringLiteral("待提交中文\n第二行"));
            if (index == 0)
                stickerClick(*sticker, "stickerEditCopyButton");
            else if (index == 1)
                QVERIFY(sticker->saveImage());
            else
            {
                stickerClick(*sticker, "stickerEditLineToolButton");
                stickerDrag(*sticker, {100, 200}, {200, 200});
            }
            const auto& text = sticker->annotations()[index];
            QCOMPARE(text.type, AnnotationType::Text);
            QCOMPARE(text.style.textSize, annotationTextSizes[index]);
            QCOMPARE(text.text, QStringLiteral("待提交中文\n第二行"));
            if (index == 0)
            {
                QCOMPARE(copied, sticker->renderedImage());
                QVERIFY(!sticker->isSaved());
            }
            if (index == 1)
            {
                QVERIFY(sticker->isSaved());
                QCOMPARE(QImage(directory.filePath(QStringLiteral("文本.png")))
                             .convertToFormat(sticker->renderedImage().format()),
                         sticker->renderedImage());
            }
        }
        QCOMPARE(sticker->annotations().size(), qsizetype(4));
        QVERIFY(!sticker->isSaved());
    }
    void stickerDefaultConfirmationDialogsCancel()
    {
        using namespace waibusnap;
        StickerManager manager;
        QImage image(200, 200, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {80, 80}).success);
        auto sticker = manager.windows().first();
        QStringList labels;
        const auto cancelDialog = [&]
        {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            for (auto* button : dialog->buttons())
                labels.append(button->text());
            dialog->reject();
        };
        QTimer::singleShot(0, this, cancelDialog);
        QVERIFY(!sticker->close());
        QVERIFY(labels.contains(QStringLiteral("取消")));
        QVERIFY(labels.contains(QStringLiteral("保存")));
        QVERIFY(labels.contains(QStringLiteral("放弃")));
        labels.clear();
        QTimer::singleShot(0, this, cancelDialog);
        QVERIFY(!manager.resolveUnsavedForQuit());
        QVERIFY(labels.contains(QStringLiteral("取消退出")));
        QVERIFY(labels.contains(QStringLiteral("逐张保存")));
        QVERIFY(labels.contains(QStringLiteral("全部放弃")));
        QVERIFY(sticker && sticker->isVisible());
    }
    void stickerCloseReminderDialog_data()
    {
        QTest::addColumn<int>("choice");
        QTest::addColumn<bool>("checked");
        QTest::addColumn<int>("saveKind");
        QTest::newRow("checked-discard") << 2 << true << 0;
        QTest::newRow("unchecked-discard") << 2 << false << 0;
        QTest::newRow("checked-cancel") << 0 << true << 0;
        QTest::newRow("checked-escape") << 3 << true << 0;
        QTest::newRow("checked-dialog-close") << 4 << true << 0;
        QTest::newRow("checked-save") << 1 << true << 0;
        QTest::newRow("checked-panel-cancel") << 1 << true << 1;
        QTest::newRow("checked-save-failure") << 1 << true << 2;
    }
    void stickerCloseReminderDialog()
    {
        using namespace waibusnap;
        // Qt 控件事件无法操作 NSAlert 的独立复选框；此处验证 QWidget 确认框链路。
        // 生产仍沿用原生面板，系统复选框的点击与焦点留给真机验收。
        const bool oldNativeDialogs = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        const auto restoreNativeDialogs = qScopeGuard(
            [oldNativeDialogs]
            { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, oldNativeDialogs); });
        QFETCH(int, choice);
        QFETCH(bool, checked);
        QFETCH(int, saveKind);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        int writes = 0, panels = 0, confirmations = 0;
        StickerActions actions;
        actions.loadCloseReminder = [&] { return settings.loadStickerCloseReminder(); };
        actions.saveCloseReminder = [&](bool enabled)
        {
            ++writes;
            return settings.saveStickerCloseReminder(enabled);
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++panels;
            return saveKind == 1
                       ? QString()
                       : temporary.filePath(saveKind == 2 ? QStringLiteral("不存在/失败.png")
                                                          : QStringLiteral("保存.png"));
        };
        StickerManager manager(nullptr, actions);
        QVERIFY(manager.create(sampleFrame().pixels, {80, 90}).success);
        QVERIFY(manager.create(sampleFrame().pixels, {180, 190}).success);
        const auto first = manager.windows().first();
        const auto second = manager.windows().last();
        QTimer response;
        response.setSingleShot(true);
        connect(&response, &QTimer::timeout, this,
                [&]
                {
                    auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    QVERIFY(dialog);
                    ++confirmations;
                    auto* check = dialog->checkBox();
                    QVERIFY(check && !check->isChecked());
                    QCOMPARE(check->text(), QStringLiteral("不再提醒"));
                    QVERIFY(dialog->defaultButton());
                    QCOMPARE(dialog->defaultButton()->text(), QStringLiteral("取消"));
                    check->setChecked(checked);
                    if (choice == 3)
                        QTest::keyClick(dialog, Qt::Key_Escape);
                    else if (choice == 4)
                        dialog->close();
                    else
                    {
                        const QString label = choice == 0   ? QStringLiteral("取消")
                                              : choice == 1 ? QStringLiteral("保存")
                                                            : QStringLiteral("放弃");
                        for (auto* button : dialog->buttons())
                            if (button->text() == label)
                                button->click();
                    }
                });
        response.start(0);
        const bool closes = choice == 2 || (choice == 1 && saveKind == 0);
        QCOMPARE(first->close(), closes);
        response.stop();
        QCOMPARE(confirmations, 1);
        const bool disabled = choice == 2 && checked;
        QCOMPARE(writes, disabled ? 1 : 0);
        QCOMPARE(AppSettings(path).loadStickerCloseReminder(), !disabled);
        QCOMPARE(panels, choice == 1 ? 1 : 0);
        QCOMPARE(manager.count(), closes ? 1 : 2);
        QVERIFY(second && second->isVisible() && !second->isSaved());
        if (!closes)
            QVERIFY(first && first->isVisible() && !first->isSaved());
        // 同一管理器中原已存在的另一张窗口立即读取新偏好。
        choice = 0;
        response.start(0);
        QCOMPARE(second->close(), disabled);
        response.stop();
        QCOMPARE(confirmations, disabled ? 1 : 2);
        QCOMPARE(writes, disabled ? 1 : 0);
    }
    void stickerCloseReminderWriteFailureStillConfirms()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        // INI 目标为目录，触发真实同步失败。
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        QVERIFY(QDir().mkpath(path));
        AppSettings settings(path);
        int confirmations = 0;
        StickerActions actions;
        actions.loadCloseReminder = [&] { return settings.loadStickerCloseReminder(); };
        actions.saveCloseReminder = [&](bool enabled)
        { return settings.saveStickerCloseReminder(enabled); };
        actions.confirmClose = [&](QWidget*)
        {
            ++confirmations;
            return StickerCloseConfirmation{StickerCloseDecision::Discard, true};
        };
        StickerManager manager(nullptr, actions);
        for (int index = 0; index < 2; ++index)
        {
            QVERIFY(manager.create(sampleFrame().pixels, {80, 90}).success);
            QVERIFY(manager.windows().first()->close());
            QVERIFY(settings.loadStickerCloseReminder());
        }
        QCOMPARE(confirmations, 2);
        QCOMPARE(manager.count(), 0);
    }
    void controllerStickerCloseReminderSettingsAndClipboard()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RunOptions options;
        options.settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        int confirmations = 0;
        StickerActions actions;
        actions.confirmClose = [&](QWidget*)
        {
            ++confirmations;
            return StickerCloseConfirmation{StickerCloseDecision::Discard, true};
        };
        AutostartStub startup;
        ApplicationController controller(*qApp, options, actions,
                                         {[] { return sampleFrame().pixels; }}, startup.actions());
        QVERIFY(controller.stickers().create(sampleFrame().pixels, {80, 90}).success);
        clipboardAction(controller)->trigger();
        QVERIFY(controller.stickers().create(sampleFrame().pixels, {180, 190}).success);
        auto screenshot = controller.stickers().windows().first();
        auto clipboard = controller.stickers().windows()[1];
        auto remaining = controller.stickers().windows().last();
        QVERIFY(screenshot->close());
        QVERIFY(!AppSettings(options.settingsFile).loadStickerCloseReminder());
        QVERIFY(clipboard->close());
        QCOMPARE(confirmations, 1);
        QCOMPARE(controller.stickers().count(), 1);
        bool handled = false;
        QTimer::singleShot(
            0, &controller,
            [&]
            {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
                tabs->setCurrentWidget(
                    dialog->findChild<QWidget*>(QStringLiteral("settingsStickerPage")));
                auto* check =
                    dialog->findChild<QCheckBox*>(QStringLiteral("stickerCloseReminderCheck"));
                QVERIFY(check && !check->isChecked());
                QTest::mouseClick(check, Qt::LeftButton, Qt::NoModifier,
                                  QPoint(8, check->height() / 2));
                QVERIFY(check->isChecked());
                dialog->findChild<QPushButton*>(QStringLiteral("saveSettingsButton"))->click();
                QCOMPARE(dialog->result(), int(QDialog::Accepted));
                handled = true;
            });
        controller.menu()->actions().at(3)->trigger();
        QVERIFY(handled);
        QVERIFY(AppSettings(options.settingsFile).loadStickerCloseReminder());
        QVERIFY(remaining && remaining->isVisible() && !remaining->isSaved());
        QVERIFY(remaining->close());
        QCOMPARE(confirmations, 2);
    }
    void stickerCloseReminderSettingsFailureAndRetry()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        QVERIFY(settings.saveStickerCloseReminder(false).isEmpty());
        QVERIFY(QFile::rename(path, path + QStringLiteral(".old")));
        QVERIFY(QDir().mkpath(path));
        SaveSettingsActions actions;
        actions.stickerCloseReminder = false;
        actions.saveStickerCloseReminder = [&](bool enabled)
        { return settings.saveStickerCloseReminder(enabled); };
        AutostartStub startup;
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions, startup.actions());
        dialog.show();
        dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(3);
        auto* check = dialog.findChild<QCheckBox*>(QStringLiteral("stickerCloseReminderCheck"));
        check->setChecked(true);
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"));
        save->click();
        QVERIFY(dialog.isVisible());
        QVERIFY(check->isChecked());
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"))
                    ->text()
                    .contains(QStringLiteral("提醒设置写入失败")));
        QVERIFY(settings.loadStickerCloseReminder());
        QVERIFY(QDir(path).removeRecursively());
        QVERIFY(QFile::rename(path + QStringLiteral(".old"), path));
        save->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(AppSettings(path).loadStickerCloseReminder());
    }
    void stickerCloseDecisionsAndSaveRetries_data()
    {
        QTest::addColumn<int>("decision");
        QTest::addColumn<int>("pathKind");
        QTest::addColumn<bool>("saved");
        QTest::addColumn<bool>("closes");
        QTest::newRow("cancel") << 0 << 0 << false << false;
        QTest::newRow("discard") << 2 << 0 << false << true;
        QTest::newRow("save") << 1 << 0 << false << true;
        QTest::newRow("panel-cancel") << 1 << 1 << false << false;
        QTest::newRow("write-failure") << 1 << 2 << false << false;
        QTest::newRow("already-saved") << 0 << 0 << true << true;
    }
    void stickerCloseDecisionsAndSaveRetries()
    {
        using namespace waibusnap;
        QFETCH(int, decision);
        QFETCH(int, pathKind);
        QFETCH(bool, saved);
        QFETCH(bool, closes);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("关闭.png"));
        int confirmations = 0;
        int panels = 0;
        bool nestedPreserved = false;
        bool nestedQuit = true;
        StickerManager* current = nullptr;
        StickerActions actions;
        actions.confirmClose = [&](QWidget* window)
        {
            ++confirmations;
            window->close();
            nestedPreserved = window->isVisible();
            nestedQuit = current->resolveUnsavedForQuit();
            return static_cast<StickerCloseDecision>(decision);
        };
        actions.chooseSavePath = [&](QWidget* window, const QString&)
        {
            ++panels;
            window->close();
            nestedPreserved = window->isVisible();
            nestedQuit = current->resolveUnsavedForQuit();
            return pathKind == 1   ? QString()
                   : pathKind == 2 ? directory.filePath(QStringLiteral("不存在/图.png"))
                                   : path;
        };
        StickerManager manager(nullptr, actions);
        current = &manager;
        QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {40, 50}, saved).success);
        auto sticker = manager.windows().first();
        QCOMPARE(sticker->close(), closes);
        QCOMPARE(confirmations, saved ? 0 : 1);
        if (!saved)
        {
            QVERIFY(nestedPreserved);
            QVERIFY(!nestedQuit);
        }
        QCOMPARE(panels, !saved && decision == 1 ? 1 : 0);
        QCOMPARE(QFileInfo::exists(path), !saved && decision == 1 && pathKind == 0);
        if (closes)
        {
            QCOMPARE(manager.count(), 0);
            QTRY_VERIFY(!sticker);
        }
        else
        {
            QCOMPARE(manager.count(), 1);
            QVERIFY(sticker->isVisible());
            QVERIFY(!sticker->isSaved());
            if (decision == 1)
                QVERIFY(
                    sticker->findChild<QLabel*>(QStringLiteral("stickerStatus"))
                        ->text()
                        .contains(pathKind == 1 ? QStringLiteral("取消") : QStringLiteral("失败")));
            decision = 1;
            pathKind = 0;
            QVERIFY(sticker->close());
            QVERIFY(QFileInfo::exists(path));
            QTRY_VERIFY(!sticker);
        }
    }
    void stickerCloseSaveFailureRestoresEditingToolbar()
    {
        using namespace waibusnap;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        bool valid = false;
        int confirmations = 0;
        StickerActions actions;
        actions.confirmClose = [&](QWidget*)
        {
            ++confirmations;
            return StickerCloseDecision::Save;
        };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            return valid ? directory.filePath(QStringLiteral("重试.png"))
                         : directory.filePath(QStringLiteral("不存在/失败.png"));
        };
        StickerManager manager(nullptr, actions);
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {80, 80}, true).success);
        auto sticker = manager.windows().first();
        sticker->setEditing(true);
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
        sticker->findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("失败后保留"));
        QVERIFY(!sticker->close());
        QVERIFY(sticker && sticker->isEditing());
        QCOMPARE(sticker->annotations().size(), qsizetype(1));
        QVERIFY(!sticker->isSaved());
        auto* toolbar = sticker->findChild<QWidget*>(QStringLiteral("stickerEditToolbar"));
        auto* status = sticker->findChild<QLabel*>(QStringLiteral("stickerStatus"));
        QVERIFY(toolbar->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("失败")));
        if (toolbar->isVisible())
            QVERIFY(status->geometry().bottom() < toolbar->y());
        valid = true;
        stickerClick(*sticker, "stickerEditSaveButton");
        QVERIFY(sticker->isSaved());
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("重试.png"))));
        QVERIFY(sticker->close());
        QCOMPARE(confirmations, 1);
        QTRY_VERIFY(!sticker);
    }
    void stickerQuitSummaryOrderFailureAndReentrancy_data()
    {
        QTest::addColumn<int>("decision");
        QTest::addColumn<int>("failure");
        QTest::newRow("cancel") << 0 << 0;
        QTest::newRow("save-all") << 1 << 0;
        QTest::newRow("second-cancel") << 1 << 1;
        QTest::newRow("second-failure") << 1 << 2;
        QTest::newRow("discard-all") << 2 << 0;
    }
    void stickerQuitSummaryOrderFailureAndReentrancy()
    {
        using namespace waibusnap;
        QFETCH(int, decision);
        QFETCH(int, failure);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        int summaries = 0;
        int panels = 0;
        int closes = 0;
        int summaryCount = 0;
        bool nestedResolve = true;
        bool nestedClose = true;
        StickerManager* current = nullptr;
        QVector<QWidget*> order;
        StickerActions actions;
        actions.loadCloseReminder = [] { return false; };
        actions.confirmQuit = [&](int count)
        {
            ++summaries;
            summaryCount = count;
            nestedResolve = current->resolveUnsavedForQuit();
            nestedClose = current->windows().first()->close();
            return static_cast<StickerQuitDecision>(decision);
        };
        actions.confirmClose = [&](QWidget*)
        {
            ++closes;
            return StickerCloseDecision::Cancel;
        };
        actions.chooseSavePath = [&](QWidget* window, const QString&)
        {
            order.append(window);
            ++panels;
            nestedResolve = current->resolveUnsavedForQuit();
            if (panels == 2 && failure)
                return failure == 1 ? QString()
                                    : directory.filePath(QStringLiteral("不存在/失败.png"));
            return directory.filePath(QStringLiteral("%1.png").arg(panels));
        };
        StickerManager manager(nullptr, actions);
        current = &manager;
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {40, 50}).success);
        QVERIFY(manager.create(image, {80, 90}, true).success);
        QVERIFY(manager.create(image, {120, 130}).success);
        const auto windows = manager.windows();
        windows.first()->setEditing(true);
        stickerClick(*windows.first(), "stickerEditTextToolButton");
        QTest::mouseClick(windows.first(), Qt::LeftButton, Qt::NoModifier, {100, 100});
        windows.first()->findChild<AnnotationTextEdit*>()->setPlainText(
            QStringLiteral("退出前提交"));
        const bool result = manager.resolveUnsavedForQuit();
        QCOMPARE(result, decision != 0 && failure == 0);
        QCOMPARE(summaries, 1);
        QCOMPARE(summaryCount, 2);
        QVERIFY(!nestedResolve);
        QVERIFY(!nestedClose);
        QCOMPARE(closes, 0);
        QCOMPARE(manager.count(), 3);
        QVERIFY(!windows.first()->isEditing());
        QCOMPARE(windows.first()->annotations().size(), qsizetype(1));
        for (const auto& window : windows)
            QVERIFY(window && window->isVisible());
        if (decision == 1)
        {
            QCOMPARE(panels, 2);
            QCOMPARE(order[0], windows.first().data());
            QCOMPARE(order[1], windows.last().data());
            QVERIFY(windows.first()->isSaved());
            QCOMPARE(windows.last()->isSaved(), failure == 0);
            QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("1.png"))));
            QCOMPARE(QFileInfo::exists(directory.filePath(QStringLiteral("2.png"))), failure == 0);
        }
        else
        {
            QCOMPARE(panels, 0);
            QVERIFY(!windows.first()->isSaved());
        }
        if (!result)
        {
            // 中止后仍可编辑、拖动、保存，且已保存项保持已保存。
            stickerDrag(*windows.last(), {80, 80}, {110, 100});
            QCOMPARE(windows.last()->pos(), QPoint(150, 150));
            windows.last()->setEditing(true);
            QVERIFY(windows.last()->isEditing());
        }
        manager.closeAll();
        QCOMPARE(closes, 0);
        for (const auto& window : windows)
            QVERIFY(!window);
    }
    void stickerQuitSavedAndControllerCancellation()
    {
        using namespace waibusnap;
        QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        int summaries = 0;
        StickerActions actions;
        actions.confirmQuit = [&](int)
        {
            ++summaries;
            return StickerQuitDecision::Cancel;
        };
        StickerManager manager(nullptr, actions);
        QVERIFY(manager.resolveUnsavedForQuit());
        QVERIFY(manager.create(image, {40, 50}, true).success);
        QVERIFY(manager.resolveUnsavedForQuit());
        QCOMPARE(summaries, 0);
        ApplicationController* current = nullptr;
        actions.confirmQuit = [&](int)
        {
            ++summaries;
            current->quit();
            return StickerQuitDecision::Cancel;
        };
        ApplicationController controller(*qApp, {}, actions);
        current = &controller;
        QVERIFY(controller.stickers().create(image, {80, 90}).success);
        const auto windows = controller.stickers().windows();
        controller.quit();
        QCOMPARE(summaries, 1);
        QCOMPARE(controller.stickers().count(), 1);
        QVERIFY(windows.first() && windows.first()->isVisible());
        controller.quit();
        QCOMPARE(summaries, 2);
        QVERIFY(windows.first());
    }
    void shapeToolbarButtonAndBar()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 200});
        auto* shape = button(overlay, "shapeButton");
        auto* bar = overlay.findChild<QWidget*>(QStringLiteral("shapeBar"));
        QVERIFY(shape && bar && shape->isCheckable());
        QCOMPARE(shape->iconSize(), QSize(18, 18));
        QVERIFY(!shape->icon().isNull());
        QVERIFY(!shape->isChecked() && !bar->isVisible());
        QCOMPARE(shape->toolTip(), QStringLiteral("选择异形形状（当前：自由）"));
        QTest::mouseClick(shape, Qt::LeftButton);
        QVERIFY(shape->isChecked() && bar->isVisible());
        QCOMPARE(bar->findChildren<QPushButton*>().size(), 8);
        QVERIFY(button(overlay, "shapeFreeButton")->isChecked());
        QCOMPARE(button(overlay, "shapeFreeButton")->toolTip(), QStringLiteral("自由（矩形输出）"));
        const QRect original = overlay.selection();
        for (int i = 0; i < 6; ++i)
        {
            auto* option =
                button(overlay, qPrintable(QStringLiteral("shapeOption%1Button").arg(i)));
            QCOMPARE(option->text(), captureShapeLabel(static_cast<CaptureShape>(i)));
            QTest::mouseClick(option, Qt::LeftButton);
            QVERIFY(option->isChecked());
            QCOMPARE(overlay.selection(), original);
            QVERIFY(shape->toolTip().contains(option->text()));
            QCOMPARE(overlay.selectionSizeText(),
                     option->text() + QStringLiteral(" · 300 × 200 像素"));
            QTest::mouseClick(option, Qt::LeftButton);
            QVERIFY(button(overlay, "shapeFreeButton")->isChecked());
        }
        auto* white = button(overlay, "shapeWhiteBackgroundButton");
        QVERIFY(white && white->isCheckable() && !white->isChecked());
        QTest::mouseClick(white, Qt::LeftButton);
        QVERIFY(white->isChecked());
        QTest::mouseClick(shape, Qt::LeftButton);
        QVERIFY(!bar->isVisible());
        SelectionOverlay next(annotationFrame());
        next.show();
        dragLogical(next, {100, 100}, {250, 200});
        QVERIFY(!button(next, "shapeButton")->isChecked());
        QVERIFY(!button(next, "shapeWhiteBackgroundButton")->isChecked());
        QVERIFY(button(next, "shapeFreeButton")->isChecked());
    }
    void shapeRatioExclusion()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 200});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        const QRect ratioSelection = overlay.selection();
        QTest::mouseClick(button(overlay, "shapeButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "shapeOption2Button"), Qt::LeftButton);
        QCOMPARE(overlay.selection(), ratioSelection);
        QVERIFY(!button(overlay, "ratioButton")->isEnabled());
        QVERIFY(!button(overlay, "ratioButton")->isChecked());
        QVERIFY(!overlay.findChild<QWidget*>(QStringLiteral("ratioBar"))->isVisible());
        QVERIFY(button(overlay, "ratioFreeButton")->isChecked());
        QVERIFY(!button(overlay, "ratioOption1Button")->isChecked());
        QCOMPARE(button(overlay, "ratioButton")->toolTip(),
                 QStringLiteral("形状截图使用模板比例锁定"));
        QTest::mouseClick(button(overlay, "shapeFreeButton"), Qt::LeftButton);
        QVERIFY(button(overlay, "ratioButton")->isEnabled());
        QCOMPARE(overlay.selection(), ratioSelection);
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QVERIFY(overlay.findChild<QWidget*>(QStringLiteral("ratioBar"))->isVisible());
        QVERIFY(button(overlay, "ratioFreeButton")->isChecked());
    }
    void shapeShiftRatioLock_data()
    {
        QTest::addColumn<bool>("useTemplate");
        QTest::addColumn<qreal>("scale");
        for (bool useTemplate : {false, true})
            for (qreal scale : {1.0, 1.25, 2.0})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(useTemplate).arg(scale)))
                    << useTemplate << scale;
    }
    void shapeShiftRatioLock()
    {
        using namespace waibusnap;
        QFETCH(bool, useTemplate);
        QFETCH(qreal, scale);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage mask(90, 60, QImage::Format_ARGB32);
        mask.fill(Qt::white);
        QVERIFY(mask.save(temporary.filePath(QStringLiteral("wide.png"))));
        OverlayActions actions;
        actions.ratios.quick = AspectRatio{9, 16};
        actions.shapeTemplates = {{QStringLiteral("wide.png"), QStringLiteral("宽模板"), true}};
        actions.shapeTemplateDir = temporary.path();
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = scale;
        frame.pixels = QImage(qRound(800 * scale), qRound(600 * scale), QImage::Format_RGB32);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(scale);
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 180});
        QTest::mouseClick(button(overlay, "shapeButton"), Qt::LeftButton);
        QTest::mouseClick(
            button(overlay, useTemplate ? "shapeTemplate0Button" : "shapeOption2Button"),
            Qt::LeftButton);
        const AspectRatio expected = useTemplate ? AspectRatio{3, 2} : AspectRatio{1, 1};
        ratioMouse(overlay, QEvent::MouseButtonPress, {420, 100});
        ratioMouse(overlay, QEvent::MouseMove, {580, 160});
        const QRect normal = overlay.selection();
        QTest::keyPress(&overlay, Qt::Key_Shift);
        verifySelectionRatio(overlay.selection(), expected, frame.pixels.size());
        QVERIFY(overlay.selectionSizeText().startsWith(QStringLiteral("⇧ ")));
        QTest::keyRelease(&overlay, Qt::Key_Shift);
        QCOMPARE(overlay.selection(), normal);
        ratioMouse(overlay, QEvent::MouseMove, {580, 160}, Qt::ShiftModifier);
        verifySelectionRatio(overlay.selection(), expected, frame.pixels.size());
        ratioMouse(overlay, QEvent::MouseButtonRelease, {580, 160}, Qt::ShiftModifier);
        QVERIFY(!overlay.selectionSizeText().startsWith(QStringLiteral("⇧ ")));
        const QRect initial = overlay.selection();
        const QPoint corner(qRound((initial.x() + initial.width()) / scale),
                            qRound((initial.y() + initial.height()) / scale));
        ratioMouse(overlay, QEvent::MouseButtonPress, corner);
        ratioMouse(overlay, QEvent::MouseMove, corner + QPoint(20, 10));
        const QRect resized = overlay.selection();
        QTest::keyPress(&overlay, Qt::Key_Shift);
        verifySelectionRatio(overlay.selection(), expected, frame.pixels.size());
        QTest::keyRelease(&overlay, Qt::Key_Shift);
        QCOMPARE(overlay.selection(), resized);
        ratioMouse(overlay, QEvent::MouseMove, corner + QPoint(20, 10), Qt::ShiftModifier);
        verifySelectionRatio(overlay.selection(), expected, frame.pixels.size());
        ratioMouse(overlay, QEvent::MouseMove, corner + QPoint(20, 10));
        QCOMPARE(overlay.selection(), resized);
        ratioMouse(overlay, QEvent::MouseButtonRelease, corner + QPoint(20, 10));
        // 键盘 Shift 扩边保持逐像素语义，不锁定形状比例。
        overlay.activateWindow();
        overlay.setFocus();
        QTRY_VERIFY(overlay.hasFocus());
        const QRect before = overlay.selection();
        QTest::keyClick(&overlay, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(overlay.selection().size(), QSize(before.width() + 1, before.height()));
    }
    void shapeOutputPixels_data()
    {
        QTest::addColumn<int>("shape");
        QTest::addColumn<bool>("white");
        for (int shape = 0; shape < 6; ++shape)
            for (bool white : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(shape).arg(white)))
                    << shape << white;
    }
    void shapeOutputPixels()
    {
        using namespace waibusnap;
        QFETCH(int, shape);
        QFETCH(bool, white);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage copied, pinned;
        OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{false, {}};
        };
        actions.pinImage = [&](const QImage& image)
        {
            pinned = image;
            return ImageOutputResult{false, {}};
        };
        actions.loadSavePreferences = [&]
        { return SavePreferences{temporary.path(), ImageFormat::Jpeg}; };
        SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 200});
        QTest::mouseClick(button(overlay, "shapeButton"), Qt::LeftButton);
        QTest::mouseClick(
            button(overlay, qPrintable(QStringLiteral("shapeOption%1Button").arg(shape))),
            Qt::LeftButton);
        if (white)
            QTest::mouseClick(button(overlay, "shapeWhiteBackgroundButton"), Qt::LeftButton);
        const QString path = temporary.filePath(QStringLiteral("explicit.jpg"));
        QVERIFY(overlay.exportToPath(path));
        QVERIFY(!QFileInfo::exists(path));
        const QImage saved(temporary.filePath(QStringLiteral("explicit.png")));
        QCOMPARE(saved.size(), QSize(300, 200));
        QCOMPARE(saved.devicePixelRatio(), 1.0);
        QCOMPARE(saved.pixelColor(0, 0).alpha(), white ? 255 : 0);
        if (white)
            QCOMPARE(saved.pixelColor(0, 0), QColor(Qt::white));
        QCOMPARE(saved.pixelColor(150, 100).alpha(), 255);
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "pinButton"), Qt::LeftButton);
        QCOMPARE(copied.convertToFormat(QImage::Format_ARGB32),
                 saved.convertToFormat(QImage::Format_ARGB32));
        QCOMPARE(pinned.convertToFormat(QImage::Format_ARGB32),
                 saved.convertToFormat(QImage::Format_ARGB32));
        QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
        const auto files = QDir(temporary.path()).entryList({QStringLiteral("*.png")}, QDir::Files);
        QCOMPARE(files.size(), 2);
        QVERIFY(QDir(temporary.path()).entryList({QStringLiteral("*.jpg")}, QDir::Files).isEmpty());
        QVERIFY(overlay.findChild<QLabel*>(QStringLiteral("outputStatus"))
                    ->text()
                    .contains(QStringLiteral("形状截图以 PNG 保存")));
    }
    void shapeTemplateRoundTrip()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
        QVERIFY(settings.loadShapeTemplates().isEmpty());
        QImage mask(90, 60, QImage::Format_ARGB32);
        mask.fill(Qt::transparent);
        mask.setPixelColor(45, 30, QColor(255, 255, 255, 128));
        const QString input = temporary.filePath(QStringLiteral("圆环.png"));
        QVERIFY(mask.save(input));
        auto imported = settings.importShapeTemplates({input, input});
        QVERIFY(imported.error.isEmpty());
        QCOMPARE(imported.imported.size(), 2);
        QCOMPARE(imported.imported[1].file, QStringLiteral("圆环-2.png"));
        QVERIFY(settings.saveShapeTemplates(imported.imported).isEmpty());
        QCOMPARE(settings.loadShapeTemplates().size(), 2);
        mask.fill(Qt::transparent);
        const QString empty = temporary.filePath(QStringLiteral("empty.png"));
        QVERIFY(mask.save(empty));
        QCOMPARE(settings.importShapeTemplates({empty}).error,
                 QStringLiteral("模板没有任何可见像素，已忽略。"));
        QFile broken(temporary.filePath(QStringLiteral("broken.png")));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("invalid");
        broken.close();
        QCOMPARE(settings.importShapeTemplates({broken.fileName()}).error,
                 QStringLiteral("模板导入失败：不是有效的 PNG。"));
        const QString fake = temporary.filePath(QStringLiteral("fake.png"));
        mask.fill(Qt::white);
        QVERIFY(mask.save(fake, "BMP"));
        QCOMPARE(settings.importShapeTemplates({fake}).error,
                 QStringLiteral("模板导入失败：不是有效的 PNG。"));
        QImage huge(5000, 100, QImage::Format_RGB32);
        huge.fill(Qt::white);
        const QString large = temporary.filePath(QStringLiteral("large.png"));
        QVERIFY(huge.save(large));
        auto resized = settings.importShapeTemplates({large});
        QVERIFY(resized.error.isEmpty());
        QImage down(QDir(settings.shapeTemplateDir()).filePath(resized.imported[0].file));
        QCOMPARE(down.width(), 4096);
        QVERIFY(std::abs(down.height() - 4096.0 / 50) <= 1);
        int saves = 0;
        SaveSettingsActions actions;
        actions.shapeTemplates = settings.loadShapeTemplates();
        actions.shapeTemplateDir = settings.shapeTemplateDir();
        actions.importShapeTemplates = [&](const QStringList& paths)
        { return settings.importShapeTemplates(paths); };
        actions.chooseShapeTemplatePaths = [&](QWidget*) { return QStringList{large}; };
        actions.saveShapeTemplates = [&](const QVector<ShapeTemplateEntry>& entries)
        {
            ++saves;
            return settings.saveShapeTemplates(entries);
        };
        AutostartActions startup{[] { return AutostartState{}; }, [](bool) { return QString(); }};
        SettingsDialog dialog(
            QKeySequence(Qt::Key_F1), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions, startup);
        dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(2);
        dialog.show();
        QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("shapeTemplateList")));
        QApplication::processEvents();
        auto* saveControl = dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"));
        QVERIFY(dialog.rect().contains(
            QRect(saveControl->mapTo(&dialog, QPoint()), saveControl->size())));
        const QString evidence = qEnvironmentVariable("WAIBUSNAP_SHAPE_EVIDENCE_DIR");
        if (!evidence.isEmpty())
        {
            QApplication::processEvents();
            QVERIFY(
                dialog.grab().save(QDir(evidence).filePath(QStringLiteral("shape-settings.png"))));
        }
        auto* emptyNotice = dialog.findChild<QLabel*>(QStringLiteral("shapeTemplateEmptyNotice"));
        QVERIFY(emptyNotice && !emptyNotice->isVisible());
        dialog.findChild<QCheckBox*>(QStringLiteral("shapeTemplateToggle0"))->setChecked(false);
        dialog.findChild<QPushButton*>(QStringLiteral("shapeTemplateDelete1"))->click();
        // 删除暂存，取消前不得删文件或写配置。
        QVERIFY(QFileInfo::exists(
            QDir(settings.shapeTemplateDir()).filePath(imported.imported[1].file)));
        dialog.findChild<QPushButton*>(QStringLiteral("shapeTemplateImportButton"))->click();
        QCOMPARE(saves, 0);
        dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"))->click();
        QCOMPARE(saves, 1);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        const auto loaded = settings.loadShapeTemplates();
        QCOMPARE(loaded.size(), 2);
        QVERIFY(!loaded[0].enabled);
        QVERIFY(loaded[1].enabled);
        QVERIFY(!QFileInfo::exists(
            QDir(settings.shapeTemplateDir()).filePath(imported.imported[1].file)));
        actions.shapeTemplates = loaded;
        SettingsDialog cancel(
            QKeySequence(Qt::Key_F1), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions, startup);
        cancel.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(2);
        cancel.show();
        cancel.findChild<QPushButton*>(QStringLiteral("shapeTemplateDelete0"))->click();
        cancel.findChild<QPushButton*>(QStringLiteral("shapeTemplateImportButton"))->click();
        QVERIFY(QFileInfo::exists(
            QDir(settings.shapeTemplateDir()).filePath(QStringLiteral("large-3.png"))));
        cancel.findChild<QPushButton*>(QStringLiteral("cancelSettingsButton"))->click();
        QCOMPARE(saves, 1);
        QCOMPARE(settings.loadShapeTemplates().size(), 2);
        QVERIFY(QFileInfo::exists(QDir(settings.shapeTemplateDir()).filePath(loaded[0].file)));
        QVERIFY(settings.removeShapeTemplate(loaded[0].file).isEmpty());
        QCOMPARE(settings.loadShapeTemplates().size(), 1);
        QVERIFY(!QFileInfo::exists(QDir(settings.shapeTemplateDir()).filePath(loaded[0].file)));
        QVERIFY(QFile::remove(QDir(settings.shapeTemplateDir()).filePath(loaded[1].file)));
        QString notice;
        QVERIFY(settings.loadShapeTemplates(&notice).isEmpty());
        QVERIFY(!notice.isEmpty());
        QFile stored(temporary.filePath(QStringLiteral("settings.ini")));
        QVERIFY(stored.open(QIODevice::ReadOnly));
        const QByteArray beforeRead = stored.readAll();
        stored.close();
        settings.loadShapeTemplates();
        QVERIFY(stored.open(QIODevice::ReadOnly));
        QCOMPARE(stored.readAll(), beforeRead);
        SaveSettingsActions failure;
        failure.saveShapeTemplates = [](const QVector<ShapeTemplateEntry>&)
        { return QStringLiteral("模板写入失败"); };
        SettingsDialog failed(
            QKeySequence(Qt::Key_F1), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            failure, startup);
        failed.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(2);
        failed.show();
        QVERIFY(failed.findChild<QLabel*>(QStringLiteral("shapeTemplateEmptyNotice"))->isVisible());
        failed.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"))->click();
        QVERIFY(failed.isVisible());
        QCOMPARE(failed.findChild<QLabel*>(QStringLiteral("settingsStatus"))->text(),
                 QStringLiteral("模板写入失败"));
        QVERIFY(failed.findChild<QLabel*>(QStringLiteral("settingsStatus"))->isVisible());
    }
    void shapeBarLayoutAndPreview_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("options");
        for (int width : {320, 480, 597, 598, 650, 651, 800})
            for (bool options : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(width).arg(options)))
                    << width << options;
    }
    void shapeBarLayoutAndPreview()
    {
        using namespace waibusnap;
        QFETCH(int, width);
        QFETCH(bool, options);
        auto frame = annotationFrame();
        frame.display.logicalGeometry = {0, 0, width, 600};
        frame.display.availableLogicalGeometry = {0, 25, width, 550};
        frame.pixels = QImage(width * 2, 1200, QImage::Format_RGB32);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(2);
        OverlayActions actions;
        actions.recognizer = std::make_shared<OcrStub>();
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        dragLogical(overlay, {40, 70}, {width - 30, 280});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "shapeButton"), Qt::LeftButton);
        if (options)
            clickAnnotationTool(overlay, "rectangleToolButton");
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* ratio = overlay.findChild<QWidget*>(QStringLiteral("ratioBar"));
        auto* shape = overlay.findChild<QWidget*>(QStringLiteral("shapeBar"));
        const QRect bounds(0, 25, width, 550);
        QVERIFY(bounds.contains(toolbar->geometry()));
        QVERIFY(bounds.contains(shape->geometry()));
        QCOMPARE(shape->x(), toolbar->x());
        QCOMPARE(shape->y(), ratio->y() + ratio->height() + 6);
        QVERIFY(shape->width() <= toolbar->width());
        for (auto* control : shape->findChildren<QPushButton*>())
            QVERIFY(shape->rect().contains(control->geometry()));
        const QRect selection = overlay.selection();
        QTest::mouseClick(shape, Qt::LeftButton, Qt::NoModifier, {2, 2});
        QCOMPARE(overlay.selection(), selection);
        QCOMPARE(shape->cursor().shape(), Qt::ArrowCursor);
        QTest::mouseClick(button(overlay, "shapeOption2Button"), Qt::LeftButton);
        QImage painted(overlay.size(), QImage::Format_ARGB32);
        overlay.render(&painted);
        QCOMPARE(painted.pixelColor(width / 2, 170), QColor(Qt::white));
        QCOMPARE(painted.pixelColor(50, 110), QColor(180, 180, 180));
        if (options)
            clickAnnotationTool(overlay, "rectangleToolButton");
        overlay.activateWindow();
        overlay.setFocus();
        QTRY_VERIFY(overlay.hasFocus());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(!overlay.magnifierRect().intersects(shape->geometry()));
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QVERIFY(bounds.contains(toolbar->geometry()));
        QVERIFY(bounds.contains(shape->geometry()));
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        if (status->isVisible())
            QVERIFY(!status->geometry().intersects(shape->geometry()));
        const QString evidence = qEnvironmentVariable("WAIBUSNAP_SHAPE_EVIDENCE_DIR");
        if (width == 800 && options && !evidence.isEmpty())
        {
            QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
            QVERIFY(
                overlay.grab().save(QDir(evidence).filePath(QStringLiteral("shape-overlay.png"))));
        }
    }
    void shapeTemplateOutputAndOcr()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage mask(300, 200, QImage::Format_ARGB32);
        mask.fill(Qt::transparent);
        {
            QPainter painter(&mask);
            painter.fillRect(QRect(40, 40, 220, 120), QColor(255, 255, 255, 128));
        }
        QVERIFY(mask.save(temporary.filePath(QStringLiteral("alpha.png"))));
        auto engine = std::make_shared<OcrStub>();
        OverlayActions actions;
        actions.recognizer = engine;
        actions.shapeTemplates = {{QStringLiteral("alpha.png"), QStringLiteral("半透明模板"), true},
                                  {QStringLiteral("alpha.png"), QStringLiteral("停用模板"), false}};
        actions.shapeTemplateDir = temporary.path();
        SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 200});
        clickAnnotationTool(overlay, "coverToolButton");
        dragLogical(overlay, {110, 110}, {240, 190});
        QTest::mouseClick(button(overlay, "shapeButton"), Qt::LeftButton);
        QVERIFY(button(overlay, "shapeTemplate0Button"));
        QVERIFY(!button(overlay, "shapeTemplate1Button"));
        QTest::mouseClick(button(overlay, "shapeTemplate0Button"), Qt::LeftButton);
        const auto annotated = renderAnnotatedSelection(annotationFrame().pixels,
                                                        overlay.annotations(), overlay.selection());
        QVERIFY(overlay.exportToPath(temporary.filePath(QStringLiteral("output.png"))));
        const QImage saved(temporary.filePath(QStringLiteral("output.png")));
        QCOMPARE(saved.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(saved.pixelColor(150, 100).alpha(), 128);
        QCOMPARE(saved.convertToFormat(QImage::Format_ARGB32),
                 applyShapeMask(annotated, mask, false).convertToFormat(QImage::Format_ARGB32));
        QImage painted(overlay.size(), QImage::Format_ARGB32);
        overlay.render(&painted);
        QCOMPARE(painted.pixelColor(105, 105), QColor(180, 180, 180));
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QCOMPARE(engine->input(), annotated);
        QCOMPARE(engine->input().pixelColor(0, 0).alpha(), 255);
    }
    void shapeStickerTransparency()
    {
        using namespace waibusnap;
        QImage image(50, 40, QImage::Format_ARGB32);
        image.fill(Qt::white);
        StickerWindow opaque(image, {0, 0}, true);
        QVERIFY(opaque.testAttribute(Qt::WA_OpaquePaintEvent));
        QVERIFY(!opaque.testAttribute(Qt::WA_TranslucentBackground));
        image.setPixelColor(49, 39, Qt::transparent);
        StickerWindow transparent(image, {0, 0}, true);
        QVERIFY(!transparent.testAttribute(Qt::WA_OpaquePaintEvent));
        QVERIFY(transparent.testAttribute(Qt::WA_TranslucentBackground));
        transparent.show();
        transparent.setEditing(true);
        transparent.setScale(2, transparent.mapToGlobal(QPoint(10, 10)));
        transparent.hide();
        transparent.show();
        QVERIFY(transparent.testAttribute(Qt::WA_TranslucentBackground));
        QCOMPARE(transparent.renderedImage().pixelColor(49, 39).alpha(), 0);
        QTemporaryDir temporary;
        QVERIFY(transparent.exportToPath(temporary.filePath(QStringLiteral("sticker.png"))));
        QCOMPARE(
            QImage(temporary.filePath(QStringLiteral("sticker.png"))).pixelColor(49, 39).alpha(),
            0);
    }
    void ratioToolbarButtonAndBar()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        auto* ratio = button(overlay, "ratioButton");
        auto* bar = overlay.findChild<QWidget*>(QStringLiteral("ratioBar"));
        QVERIFY(ratio && bar);
        QVERIFY(ratio->isCheckable());
        QVERIFY(!ratio->isChecked());
        QVERIFY(!bar->isVisible());
        QCOMPARE(ratio->iconSize(), QSize(18, 18));
        QCOMPARE(bar->findChildren<QPushButton*>().size(), 9);
        QCOMPARE(ratio->toolTip(),
                 QStringLiteral("选择截图比例（当前：自由）；按住 ⇧ 临时使用 1:1"));
        QTest::mouseClick(ratio, Qt::LeftButton);
        QVERIFY(ratio->isChecked() && bar->isVisible());
        QVERIFY(button(overlay, "ratioFreeButton")->isChecked());
        QCOMPARE(button(overlay, "ratioFreeButton")->toolTip(),
                 QStringLiteral("自由（不限制比例）"));
        auto* wide = button(overlay, "ratioOption1Button");
        QCOMPARE(wide->text(), QStringLiteral("16:9"));
        QCOMPARE(wide->toolTip(), QStringLiteral("使用 16:9"));
        QTest::mouseClick(wide, Qt::LeftButton);
        QVERIFY(wide->isChecked() && !button(overlay, "ratioFreeButton")->isChecked());
        QTest::mouseClick(wide, Qt::LeftButton);
        QVERIFY(!wide->isChecked() && button(overlay, "ratioFreeButton")->isChecked());
        QTest::mouseClick(wide, Qt::LeftButton);
        QTest::mouseClick(ratio, Qt::LeftButton);
        QVERIFY(!ratio->isChecked() && !bar->isVisible());
        QVERIFY(overlay.selectionSizeText().startsWith(QStringLiteral("16:9 · ")));
        const QRect selection = overlay.selection();
        clickAnnotationTool(overlay, "rectangleToolButton");
        QTest::mouseClick(ratio, Qt::LeftButton);
        QVERIFY(overlay.activeTool() == AnnotationType::Rectangle);
        QCOMPARE(overlay.selection(), selection);
        QTest::mouseClick(button(overlay, "cancelButton"), Qt::LeftButton);
        QVERIFY(!bar->isVisible());
        SelectionOverlay next(annotationFrame());
        next.show();
        dragLogical(next, {100, 100}, {300, 250});
        QVERIFY(!next.selectionSizeText().contains(QStringLiteral(" · ")));
        QVERIFY(!button(next, "ratioButton")->isChecked());
    }
    void ratioConstrainsDragAndResize_data()
    {
        QTest::addColumn<qreal>("scale");
        for (qreal scale : {1.0, 1.25, 1.5, 2.0})
            QTest::newRow(qPrintable(QString::number(scale))) << scale;
    }
    void ratioConstrainsDragAndResize()
    {
        using namespace waibusnap;
        QFETCH(qreal, scale);
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = scale;
        frame.pixels = mosaicTestImage(QSize(800, 600) * scale);
        frame.pixels.setDevicePixelRatio(scale);
        SelectionOverlay overlay(frame);
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 180});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        dragLogical(overlay, {420, 100}, {590, 200});
        verifySelectionRatio(overlay.selection(), {16, 9}, frame.pixels.size());
        QCOMPARE(overlay.selection().topLeft(), QPoint(qRound(420 * scale), qRound(100 * scale)));
        const QRect initial = overlay.selection();
        QPoint corner(qRound((initial.x() + initial.width()) / scale),
                      qRound((initial.y() + initial.height()) / scale));
        dragLogical(overlay, corner, corner + QPoint(30, 15));
        verifySelectionRatio(overlay.selection(), {16, 9}, frame.pixels.size());
        QCOMPARE(overlay.selection().topLeft(), initial.topLeft());
        const QRect beforeEdge = overlay.selection();
        const QPoint edge(qRound((beforeEdge.x() + beforeEdge.width()) / scale),
                          qRound((beforeEdge.y() + beforeEdge.height() / 2.0) / scale));
        dragLogical(overlay, edge, edge + QPoint(30, 80));
        verifySelectionRatio(overlay.selection(), {16, 9}, frame.pixels.size());
        QCOMPARE(overlay.selection().x(), beforeEdge.x());
        QVERIFY(std::abs(selectionCenter(overlay.selection()).y() -
                         selectionCenter(beforeEdge).y()) <= 0.5);
        QCOMPARE(overlay.selection().width(), beforeEdge.width() + qRound(30 * scale));
    }
    void ratioQuickRatioWithShift_data()
    {
        QTest::addColumn<bool>("active");
        QTest::addColumn<bool>("quick");
        for (bool active : {false, true})
            for (bool quick : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(active).arg(quick)))
                    << active << quick;
    }
    void ratioQuickRatioWithShift()
    {
        using namespace waibusnap;
        QFETCH(bool, active);
        QFETCH(bool, quick);
        OverlayActions actions;
        if (!quick)
            actions.ratios.quick.reset();
        SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        dragLogical(overlay, {100, 100}, {250, 180});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        if (active)
            QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        ratioMouse(overlay, QEvent::MouseButtonPress, {420, 100});
        ratioMouse(overlay, QEvent::MouseMove, {580, 160});
        const QRect normal = overlay.selection();
        QTest::keyPress(&overlay, Qt::Key_Shift);
        if (quick)
        {
            QCOMPARE(overlay.selection().width(), overlay.selection().height());
            QVERIFY(overlay.selectionSizeText().startsWith(QStringLiteral("⇧ 1:1 · ")));
        }
        else
        {
            QCOMPARE(overlay.selection().size(), QSize(320, 120));
            QVERIFY(!overlay.selectionSizeText().contains(QStringLiteral(" · ")));
        }
        QTest::keyRelease(&overlay, Qt::Key_Shift);
        QCOMPARE(overlay.selection(), normal);
        ratioMouse(overlay, QEvent::MouseMove, {580, 160}, Qt::ShiftModifier);
        if (quick)
            QCOMPARE(overlay.selection().width(), overlay.selection().height());
        else
            QCOMPARE(overlay.selection().size(), QSize(320, 120));
        ratioMouse(overlay, QEvent::MouseButtonRelease, {580, 160}, Qt::ShiftModifier);
        QVERIFY(!overlay.selectionSizeText().startsWith(QStringLiteral("⇧")));
        // 调整时 Shift 同样使用按下快照，松开后恢复原来拖动结果。
        const QRect initial = overlay.selection();
        const QPoint corner((initial.x() + initial.width()) / 2,
                            (initial.y() + initial.height()) / 2);
        ratioMouse(overlay, QEvent::MouseButtonPress, corner);
        ratioMouse(overlay, QEvent::MouseMove, corner + QPoint(20, 10));
        const QRect resized = overlay.selection();
        QTest::keyPress(&overlay, Qt::Key_Shift);
        if (quick)
            QCOMPARE(overlay.selection().width(), overlay.selection().height());
        QCOMPARE(overlay.selection().topLeft(), initial.topLeft());
        QTest::keyRelease(&overlay, Qt::Key_Shift);
        QCOMPARE(overlay.selection(), resized);
        ratioMouse(overlay, QEvent::MouseButtonRelease, corner + QPoint(20, 10));
    }
    void ratioSelectionImmediateAdjust()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        const QRect initial = overlay.selection();
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        const QRect adjusted = overlay.selection();
        verifySelectionRatio(adjusted, {16, 9}, {1600, 1200});
        QVERIFY(std::abs(selectionCenter(initial).x() - selectionCenter(adjusted).x()) <= 0.5);
        QVERIFY(std::abs(selectionCenter(initial).y() - selectionCenter(adjusted).y()) <= 0.5);
        QCOMPARE(adjusted.width(), initial.width());
        QTest::mouseClick(button(overlay, "ratioFreeButton"), Qt::LeftButton);
        QCOMPARE(overlay.selection(), adjusted);
        QVERIFY(!overlay.selectionSizeText().contains(QStringLiteral(" · ")));
    }
    void ratioSizeTextFormats()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        QCOMPARE(overlay.selectionSizeText(), QStringLiteral("400 × 300 像素"));
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        QCOMPARE(overlay.selectionSizeText(), QStringLiteral("16:9 · 400 × 225 像素"));
        ratioMouse(overlay, QEvent::MouseButtonPress, {420, 100});
        ratioMouse(overlay, QEvent::MouseMove, {580, 160}, Qt::ShiftModifier);
        QCOMPARE(overlay.selectionSizeText(), QStringLiteral("⇧ 1:1 · 320 × 320 像素"));
        QTest::keyRelease(&overlay, Qt::Key_Shift);
        QCOMPARE(overlay.selectionSizeText(), QStringLiteral("16:9 · 320 × 180 像素"));
        ratioMouse(overlay, QEvent::MouseButtonRelease, {580, 160});
    }
    void ratioKeyboardAndWindowSnap()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame(), {}, {{400, 100, 200, 200}});
        overlay.show();
        overlay.activateWindow();
        dragLogical(overlay, {100, 100}, {300, 250});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        QTRY_VERIFY(overlay.hasFocus());
        const auto size = overlay.selection().size();
        const QPoint origin = overlay.selection().topLeft();
        QTest::keyClick(&overlay, Qt::Key_Right);
        QCOMPARE(overlay.selection().size(), size);
        QCOMPARE(overlay.selection().topLeft(), origin + QPoint(1, 0));
        QTest::keyClick(&overlay, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(overlay.selection().width(), size.width() + 1);
        verifySelectionRatio(overlay.selection(), {16, 9}, {1600, 1200});
        QTest::keyClick(&overlay, Qt::Key_Right, Qt::AltModifier);
        QCOMPARE(overlay.selection().size(), size);
        ratioMouse(overlay, QEvent::MouseButtonPress, {450, 150}, Qt::ShiftModifier);
        ratioMouse(overlay, QEvent::MouseButtonRelease, {450, 150}, Qt::ShiftModifier);
        QCOMPARE(overlay.selection(), largestInscribedRatioRect({800, 200, 400, 400}, {16, 9}));
    }
    void ratioBarLayoutAndPanelGestures_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("options");
        for (int width : {320, 480, 565, 566, 597, 598, 618, 619, 650, 651, 800})
            for (bool options : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(width).arg(options)))
                    << width << options;
    }
    void ratioBarLayoutAndPanelGestures()
    {
        using namespace waibusnap;
        QFETCH(int, width);
        QFETCH(bool, options);
        auto frame = annotationFrame();
        frame.display.logicalGeometry = {0, 0, width, 600};
        frame.display.availableLogicalGeometry = {0, 25, width, 550};
        frame.pixels = mosaicTestImage({width * 2, 1200});
        frame.pixels.setDevicePixelRatio(2);
        OverlayActions actions;
        actions.recognizer = std::make_shared<OcrStub>();
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        dragLogical(overlay, {0, 0}, {width, 600});
        if (options)
            clickAnnotationTool(overlay, "rectangleToolButton");
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* optionBar = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        auto* bar = overlay.findChild<QWidget*>(QStringLiteral("ratioBar"));
        QVERIFY(bar->isVisible());
        QVERIFY(bar->width() <= toolbar->width());
        QCOMPARE(bar->y(), toolbar->y() + toolbar->height() + 6 +
                               (options ? 42 + optionBar->height() + 6 : 0));
        QCOMPARE(bar->x(), toolbar->x());
        const QRect bounds(0, 25, width, 550);
        QVERIFY(bounds.contains(toolbar->geometry()));
        QVERIFY(bounds.contains(bar->geometry()));
        if (options)
            QVERIFY(bounds.contains(optionBar->geometry()));
        for (auto* control : bar->findChildren<QPushButton*>())
        {
            QCOMPARE(control->height(), 28);
            QVERIFY(bar->rect().contains(control->geometry()));
        }
        if (width == 320)
            QVERIFY(bar->height() > 36);
        const QRect selected = overlay.selection();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, {2, 2});
        QCOMPARE(overlay.selection(), selected);
        QCOMPARE(bar->cursor().shape(), Qt::ArrowCursor);
        if (options)
        {
            clickAnnotationTool(overlay, "textToolButton");
            QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {40, 40});
            auto* edit =
                overlay.findChild<AnnotationTextEdit*>(QStringLiteral("annotationTextEditor"));
            QVERIFY(edit && edit->isVisible());
            edit->setPlainText(QStringLiteral("比例面板提交文本"));
            QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, {2, 2});
            QVERIFY(!edit->isVisible());
            QCOMPARE(overlay.annotations().last().text, QStringLiteral("比例面板提交文本"));
            clickAnnotationTool(overlay, "textToolButton");
        }
        overlay.activateWindow();
        overlay.setFocus();
        QTRY_VERIFY(overlay.hasFocus());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(overlay.magnifierVisible());
        QVERIFY(!overlay.magnifierRect().intersects(bar->geometry()));
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QVERIFY(bar->isVisible());
        QTest::mouseClick(button(overlay, "ratioOption1Button"), Qt::LeftButton);
        QVERIFY(overlay.isTextMode());
        QTRY_VERIFY(!overlay.isRecognizingText());
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        if (status->isVisible())
        {
            QVERIFY(!status->geometry().intersects(toolbar->geometry()));
            QVERIFY(!status->geometry().intersects(bar->geometry()));
        }
    }
    void ratioAllDisabledOnlyFree()
    {
        using namespace waibusnap;
        OverlayActions actions;
        actions.ratios = {{}, defaultCaptureRatios(), {}};
        SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        auto* bar = overlay.findChild<QWidget*>(QStringLiteral("ratioBar"));
        QCOMPARE(bar->findChildren<QPushButton*>().size(), 1);
        QVERIFY(button(overlay, "ratioFreeButton")->isChecked());
        QVERIFY(!button(overlay, "ratioButton")->toolTip().contains(QStringLiteral("按住")));
        ratioMouse(overlay, QEvent::MouseButtonPress, {420, 100}, Qt::ShiftModifier);
        ratioMouse(overlay, QEvent::MouseButtonRelease, {580, 160}, Qt::ShiftModifier);
        QCOMPARE(overlay.selection().size(), QSize(320, 120));
        QVERIFY(bar->isVisible());
    }
    void ratioSettingsDialogRoundTrip()
    {
        using namespace waibusnap;
        AutostartStub autostart;
        CaptureRatioSettings saved;
        int saves = 0;
        SaveSettingsActions actions;
        actions.saveRatios = [&](const CaptureRatioSettings& ratios)
        {
            ++saves;
            saved = ratios;
            return QString();
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions, autostart.actions());
        dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(2);
        dialog.show();
        const auto findButton = [&dialog](const char* name)
        { return dialog.findChild<QPushButton*>(QString::fromLatin1(name)); };
        auto* quick = dialog.findChild<QComboBox*>(QStringLiteral("quickRatioCombo"));
        auto* wide = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ratioWidthInput"));
        auto* high = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ratioHeightInput"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"));
        auto* list = dialog.findChild<QWidget*>(QStringLiteral("ratioList"));
        auto* scroll = qobject_cast<QScrollArea*>(list->parentWidget()->parentWidget());
        QVERIFY(scroll);
        QVERIFY(!scroll->geometry().intersects(wide->geometry()));
        auto* group = scroll->parentWidget();
        const QRect groupRect(group->mapTo(&dialog, QPoint()), group->size());
        auto* saveButton = findButton("saveSettingsButton");
        const QRect saveRect(saveButton->mapTo(&dialog, QPoint()), saveButton->size());
        QVERIFY(dialog.rect().contains(groupRect));
        QVERIFY(!groupRect.intersects(saveRect));
        QCOMPARE(quick->count(), 8);
        QCOMPARE(quick->currentText(), QStringLiteral("1:1"));
        QCOMPARE(wide->minimum(), 0.01);
        QCOMPARE(wide->maximum(), 999.99);
        QCOMPARE(wide->decimals(), 2);
        QCOMPARE(wide->singleStep(), 0.1);
        wide->setValue(32);
        high->setValue(18);
        QTest::mouseClick(findButton("ratioAddButton"), Qt::LeftButton);
        QCOMPARE(status->text(), QStringLiteral("该比例已存在。"));
        wide->setValue(5);
        high->setValue(4);
        QTest::mouseClick(findButton("ratioAddButton"), Qt::LeftButton);
        QCOMPARE(quick->count(), 9);
        wide->setValue(7);
        high->setValue(4);
        QTest::mouseClick(findButton("ratioAddButton"), Qt::LeftButton);
        QCOMPARE(quick->count(), 10);
        // 模拟未完成的输入，避免 spinbox 在失焦时先自动纠正为合法值。
        wide->findChild<QLineEdit*>()->setText(QStringLiteral(""));
        findButton("ratioAddButton")->click();
        QCOMPARE(status->text(), QStringLiteral("比例数值需为 0.01–999.99。"));
        wide->setValue(1);
        dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle0"))->setChecked(false);
        QCOMPARE(quick->currentText(), QStringLiteral("16:9"));
        quick->setCurrentText(QStringLiteral("3:4"));
        dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle4"))->setChecked(false);
        QCOMPARE(quick->currentText(), QStringLiteral("16:9"));
        QTest::mouseClick(findButton("ratioDelete1"), Qt::LeftButton);
        QCOMPARE(quick->currentText(), QStringLiteral("9:16"));
        // 5:4、7:4 的添加顺序以及各自启停在恢复默认时保持。
        dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle7"))->setChecked(false);
        QTest::mouseClick(findButton("ratioRestoreButton"), Qt::LeftButton);
        QCOMPARE(quick->count(), 9);
        QVERIFY(dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle0"))->isChecked());
        QVERIFY(dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle1"))->isChecked());
        QVERIFY(!dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle8"))->isChecked());
        quick->setCurrentText(QStringLiteral("7:4"));
        QCOMPARE(saves, 0);
        QTest::mouseClick(findButton("saveSettingsButton"), Qt::LeftButton);
        QCOMPARE(saves, 1);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(serializeRatioList(saved.enabled),
                 QStringLiteral("1:1,16:9,9:16,4:3,3:4,3:2,2:3,2.35:1,7:4"));
        QCOMPARE(serializeRatioList(saved.disabled), QStringLiteral("5:4"));
        QCOMPARE(formatAspectRatio(*saved.quick), QStringLiteral("7:4"));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        QCOMPARE(serializeRatioList(settings.loadCaptureRatios().enabled),
                 serializeRatioList(defaultCaptureRatios()));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(settings.saveHotkey(QKeySequence(Qt::Key_F2)).isEmpty());
        QVERIFY(settings.saveCaptureRatios(saved).isEmpty());
        const auto loaded = settings.loadCaptureRatios();
        QCOMPARE(serializeRatioList(loaded.enabled), serializeRatioList(saved.enabled));
        QCOMPARE(serializeRatioList(loaded.disabled), serializeRatioList(saved.disabled));
        QCOMPARE(formatAspectRatio(*loaded.quick), QStringLiteral("7:4"));
        QCOMPARE(settings.loadHotkey().sequence, QKeySequence(Qt::Key_F2));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[capture]\nratio.enabled=bad,1:1,16:9,32:18,1.001:1\nratio.disabled=2:2,3:4,0:"
                   "1\nratio.quick=3:4\n");
        file.close();
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray damaged = file.readAll();
        file.close();
        const auto repaired = settings.loadCaptureRatios();
        QCOMPARE(serializeRatioList(repaired.enabled), QStringLiteral("1:1,16:9"));
        QCOMPARE(serializeRatioList(repaired.disabled), QStringLiteral("3:4"));
        QCOMPARE(formatAspectRatio(*repaired.quick), QStringLiteral("1:1"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), damaged);
        file.close();
        QVERIFY(settings.saveCaptureRatios({{}, {{1, 1}}, {}}).isEmpty());
        QVERIFY(settings.loadCaptureRatios().enabled.isEmpty());
        QVERIFY(!settings.loadCaptureRatios().quick);
        const auto blocked = temporary.filePath(QStringLiteral("blocked"));
        QFile blocker(blocked);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QVERIFY(AppSettings(QDir(blocked).filePath(QStringLiteral("settings.ini")))
                    .saveCaptureRatios(saved)
                    .contains(QStringLiteral("重启后可能无法保留")));
    }
    void ratioSettingsCancelEmptyAndSaveFailure()
    {
        using namespace waibusnap;
        AutostartStub autostart;
        int saves = 0;
        SaveSettingsActions actions;
        actions.ratios = {{}, {{16, 9}}, {}};
        actions.saveRatios = [&](const CaptureRatioSettings&)
        {
            ++saves;
            return QStringLiteral("比例写入失败");
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions, autostart.actions());
        dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(2);
        dialog.show();
        auto* quick = dialog.findChild<QComboBox*>(QStringLiteral("quickRatioCombo"));
        auto* empty = dialog.findChild<QLabel*>(QStringLiteral("ratioEmptyNotice"));
        QVERIFY(!quick->isEnabled() && empty->isVisible());
        dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle0"))->setChecked(true);
        QVERIFY(quick->isEnabled() && !empty->isVisible());
        QCOMPARE(quick->currentText(), QStringLiteral("16:9"));
        QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                          Qt::LeftButton);
        QCOMPARE(saves, 1);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"))->text(),
                 QStringLiteral("比例写入失败"));
        QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("cancelSettingsButton")),
                          Qt::LeftButton);
        QCOMPARE(saves, 1);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        SettingsDialog cancel(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions, autostart.actions());
        cancel.show();
        cancel.findChild<QCheckBox*>(QStringLiteral("ratioToggle0"))->setChecked(true);
        QTest::keyClick(&cancel, Qt::Key_Escape);
        QCOMPARE(saves, 1);
        QCOMPARE(cancel.result(), int(QDialog::Rejected));
    }
    void toolbarStaysReachableWithSystemChrome_data()
    {
        QTest::addColumn<QRect>("available");
        QTest::addColumn<QRect>("region");
        QTest::addColumn<int>("placement");
        QTest::addColumn<qreal>("dpr");
        const QRect bounds[] = {
            {0, 25, 800, 575}, {0, 0, 800, 530}, {40, 25, 720, 505}, {0, 0, 800, 600}};
        const QRect regions[] = {{100, 40, 200, 100}, {100, 480, 200, 120}, {0, 0, 800, 600}};
        for (int chrome = 0; chrome < 4; ++chrome)
            for (int place = 0; place < 3; ++place)
                for (qreal scale : {1.0, 2.0})
                    QTest::newRow(qPrintable(QStringLiteral("chrome-%1-placement-%2-dpr-%3")
                                                 .arg(chrome)
                                                 .arg(place)
                                                 .arg(scale)))
                        << bounds[chrome] << regions[place] << place << scale;
    }
    void toolbarStaysReachableWithSystemChrome()
    {
        QFETCH(QRect, available);
        QFETCH(QRect, region);
        QFETCH(int, placement);
        QFETCH(qreal, dpr);
        using namespace waibusnap;
        auto frame = annotationFrame();
        // 负坐标副屏，确保可用区确实从全局换算到覆盖层本地。
        frame.display.logicalGeometry.moveTopLeft({-800, -100});
        frame.display.availableLogicalGeometry = available.translated(-800, -100);
        frame.display.devicePixelRatio = dpr;
        frame.pixels = QImage(QSize(800, 600) * dpr, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(dpr);
        OverlayActions actions;
        actions.copyImage = [](const QImage&)
        { return ImageOutputResult{false, QStringLiteral("测试失败提示")}; };
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        dragLogical(overlay, region.topLeft(),
                    region.topLeft() + QPoint(region.width(), region.height()));
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* options = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        QVERIFY(toolbar && options);
        for (bool withOptions : {false, true})
        {
            if (withOptions)
                clickAnnotationTool(overlay, "rectangleToolButton");
            const int clusterHeight = toolbar->height() + (withOptions ? 116 : 0);
            const int expectedY = placement == 0   ? region.y() + region.height() + 10
                                  : placement == 1 ? region.y() - clusterHeight - 10
                                                   : available.bottom() + 1 - clusterHeight - 10;
            QCOMPARE(toolbar->y(), expectedY);
            QVERIFY(available.contains(toolbar->geometry()));
            if (withOptions)
            {
                QVERIFY(options->isVisible());
                QCOMPARE(options->pos(), toolbar->pos() + QPoint(0, toolbar->height() + 48));
                QVERIFY(available.contains(options->geometry()));
            }
            QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
            auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
            QVERIFY(status && status->isVisible());
            QVERIFY(available.contains(status->geometry()));
            QVERIFY(!status->geometry().intersects(toolbar->geometry()));
            if (withOptions)
                QVERIFY(!status->geometry().intersects(options->geometry()));
        }
        clickAnnotationTool(overlay, "rectangleToolButton");
        overlay.activateWindow();
        overlay.setFocus();
        QTRY_VERIFY(overlay.hasFocus());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(overlay.magnifierVisible());
        QVERIFY(available.contains(overlay.magnifierRect()));
        for (auto* panel :
             {toolbar, options,
              static_cast<QWidget*>(overlay.findChild<QLabel*>(QStringLiteral("outputStatus")))})
            if (panel->isVisible())
                QVERIFY(!panel->geometry().intersects(overlay.magnifierRect()));
    }
    void annotationPaletteReachableAndOutput_data()
    {
        QTest::addColumn<bool>("sticker");
        QTest::newRow("overlay-320") << false;
        QTest::newRow("sticker-320") << true;
    }
    void annotationPaletteReachableAndOutput()
    {
        QFETCH(bool, sticker);
        using namespace waibusnap;
        auto frame = annotationFrame();
        frame.display.logicalGeometry = {0, 0, 320, 600};
        frame.display.availableLogicalGeometry = frame.display.logicalGeometry;
        frame.display.devicePixelRatio = 1;
        frame.pixels = QImage(320, 600, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(QColor(110, 110, 110));
        QImage copied, pinned;
        OverlayActions overlayActions;
        overlayActions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{false, QStringLiteral("保留会话")};
        };
        overlayActions.pinImage = [&](const QImage& image)
        {
            pinned = image;
            return ImageOutputResult{true, {}};
        };
        SelectionOverlay overlay(frame, overlayActions);
        StickerManager manager;
        // 用当前屏幕 DPR 生成 320 逻辑像素贴图，兼容独立 2x 子进程。
        const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
        QImage source(QSize(320, 600) * dpr, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(110, 110, 110));
        QWidget* canvas = &overlay;
        StickerWindow* window = nullptr;
        if (sticker)
        {
            QVERIFY(manager.create(source, {20, 30}, true).success);
            window = manager.windows().first();
            window->setEditing(true);
            canvas = window;
            stickerClick(*window, "stickerEditLineToolButton");
        }
        else
        {
            overlay.show();
            dragLogical(overlay, {0, 0}, {320, 600});
            clickAnnotationTool(overlay, "lineToolButton");
        }
        const QString prefix =
            sticker ? QStringLiteral("stickerEditColor") : QStringLiteral("annotationColor");
        auto* palette = canvas->findChild<QWidget*>(prefix + QStringLiteral("Palette"));
        QVERIFY(palette && palette->isVisible());
        QCOMPARE(palette->size(), QSize(164, 68));
        QVERIFY(canvas->rect().contains(QRect(palette->mapTo(canvas, QPoint()), palette->size())));
        const auto buttons = palette->findChildren<QPushButton*>();
        QCOMPARE(buttons.size(), qsizetype(annotationColors().size()));
        QVERIFY(buttons[0]->isChecked());
        for (int index = 0; index < buttons.size(); ++index)
        {
            auto* color = buttons[index];
            QVERIFY(color->isVisible() && color->isEnabled());
            QVERIFY(palette->rect().contains(color->geometry()));
            QCOMPARE(color->accessibleName(), color->toolTip());
            QVERIFY(color->toolTip().contains(annotationColorNames()[index]));
            QTest::mouseClick(color, Qt::LeftButton);
            for (int other = 0; other < buttons.size(); ++other)
                QCOMPARE(buttons[other]->isChecked(), other == index);
            QVERIFY(color->accessibleName().contains(QStringLiteral("已选")));
            const QPoint first(20, 100 + index * 20), last(250, first.y());
            if (sticker)
                stickerDrag(*window, first, last);
            else
                dragLogical(overlay, first, last);
            const auto& annotations = sticker ? window->annotations() : overlay.annotations();
            QCOMPARE(annotations.last().style.color, annotationColors()[index]);
            QCOMPARE(annotations.last().style.lineWidth, 4);
        }
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("九色.png"));
        QVERIFY(sticker ? window->exportToPath(path) : overlay.exportToPath(path));
        const QImage saved(path);
        const qreal factor = sticker ? dpr : 1;
        for (int index = 0; index < buttons.size(); ++index)
            QCOMPARE(saved.pixelColor(qRound(100 * factor), qRound((100 + index * 20) * factor)),
                     annotationColors()[index]);
        if (sticker)
        {
            compareImagePixels(saved, window->renderedImage());
            auto* menu = window->findChild<QMenu*>("stickerEditMenu");
            auto* colors = menu->actions()[5]->menu();
            QCOMPARE(colors->actions().size(), qsizetype(annotationColors().size()));
            colors->actions()[6]->trigger();
            QVERIFY(buttons[6]->isChecked());
            QVERIFY(colors->actions()[6]->isChecked());
            window->setSaved(true);
        }
        else
        {
            QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
            compareImagePixels(copied, saved);
            QTest::mouseClick(button(overlay, "pinButton"), Qt::LeftButton);
            compareImagePixels(pinned, saved);
        }
        OcrDialog dialog;
        const auto* colors = dialog.findChild<QComboBox*>("ocrColorCombo");
        QCOMPARE(colors->count(), int(annotationColors().size()));
        QCOMPARE(colors->itemData(0).value<QColor>(), QColor(Qt::black));
        for (const QColor& color : annotationColors())
            QVERIFY(colors->findData(color) >= 0);
    }
    void annotationWheelBothEditors_data()
    {
        QTest::addColumn<bool>("sticker");
        QTest::addColumn<qreal>("factor");
        for (bool sticker : {false, true})
            for (qreal factor : {qreal(1), qreal(1.5), qreal(2)})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(sticker).arg(factor)))
                    << sticker << factor;
    }
    void annotationWheelBothEditors()
    {
        QFETCH(bool, sticker);
        QFETCH(qreal, factor);
        using namespace waibusnap;
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = factor;
        frame.pixels = QImage(QSize(800, 600) * factor, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(factor);
        OverlayActions actions;
        actions.recognizer = std::make_shared<OcrStub>();
        SelectionOverlay overlay(frame, actions);
        StickerManager manager;
        QWidget* canvas = &overlay;
        StickerWindow* window = nullptr;
        if (sticker)
        {
            QImage source(1200, 1000, QImage::Format_ARGB32_Premultiplied);
            source.fill(Qt::white);
            QVERIFY(manager.create(source, {40, 50}, true).success);
            window = manager.windows().first();
            window->setScale(factor, window->pos());
            window->setEditing(true);
            canvas = window;
            stickerClick(*window, "stickerEditFreehandToolButton");
        }
        else
        {
            overlay.show();
            dragLogical(overlay, {50, 50}, {700, 500});
            clickAnnotationTool(overlay, "freehandToolButton");
        }
        auto* label =
            canvas->findChild<QLabel*>(sticker ? "stickerEditWidthLabel" : "annotationWidthLabel");
        QVERIFY(label && label->isVisible());
        QCOMPARE(label->text(), QStringLiteral("线宽 4px"));
        const QRect geometry = canvas->geometry();
        const QRect selection = overlay.selection();
        annotationWheel(*canvas, 60);
        QCOMPARE(label->text(), QStringLiteral("线宽 4px"));
        annotationWheel(*canvas, 60);
        QCOMPARE(label->text(), QStringLiteral("线宽 5px"));
        annotationWheel(*canvas, -240);
        QCOMPARE(label->text(), QStringLiteral("线宽 3px"));
        annotationWheel(*canvas, 0, {130, 130}, 30);
        QCOMPARE(label->text(), QStringLiteral("线宽 5px"));
        annotationWheel(*canvas, 9600);
        QCOMPARE(label->text(), QStringLiteral("线宽 24px"));
        annotationWheel(*canvas, -120);
        QCOMPARE(label->text(), QStringLiteral("线宽 23px"));
        annotationWheel(*canvas, -9600);
        QCOMPARE(label->text(), QStringLiteral("线宽 1px"));
        annotationWheel(*canvas, 120);
        QCOMPARE(label->text(), QStringLiteral("线宽 2px"));
        QCOMPARE(canvas->geometry(), geometry);
        QCOMPARE(overlay.selection(), selection);
        QVERIFY(sticker ? window->annotations().isEmpty() : overlay.annotations().isEmpty());
        QVERIFY(sticker ? window->isSaved() : !overlay.isSelectionSaved());
        // 绘制未松开时调粗，草稿及松开后的输出立即采用新值。
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, {130, 130});
        if (sticker)
        {
            QMouseEvent move(QEvent::MouseMove, QPointF(210, 130),
                             canvas->mapToGlobal(QPoint(210, 130)), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(canvas, &move);
        }
        else
            ratioMouse(overlay, QEvent::MouseMove, {210, 130});
        const QImage beforePreview = canvas->grab(QRect(120, 120, 110, 30)).toImage();
        annotationWheel(*canvas, 720);
        QCOMPARE(label->text(), QStringLiteral("线宽 8px"));
        QVERIFY(canvas->grab(QRect(120, 120, 110, 30)).toImage() != beforePreview);
        QVERIFY(sticker ? window->annotations().isEmpty() : overlay.annotations().isEmpty());
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, {210, 130});
        auto annotations = sticker ? window->annotations() : overlay.annotations();
        QCOMPARE(annotations.size(), qsizetype(1));
        QCOMPARE(annotations.last().style.lineWidth, 8);
        const QString tools[] = {"rectangleToolButton", "ellipseToolButton", "lineToolButton",
                                 "arrowToolButton"};
        int expectedWidth = 8;
        for (const auto& tool : tools)
        {
            const QString name =
                sticker ? QStringLiteral("stickerEdit") + tool[0].toUpper() + tool.mid(1) : tool;
            clickAnnotationTool(*canvas, qPrintable(name));
            annotationWheel(*canvas, 120);
            if (sticker)
                stickerDrag(*window, {130, 180}, {210, 200});
            else
                dragLogical(overlay, {130, 180}, {210, 200});
            annotations = sticker ? window->annotations() : overlay.annotations();
            QCOMPARE(annotations.last().style.lineWidth, ++expectedWidth);
        }
        // 控件及画布外事件不修改线宽，不触发贴图缩放。
        const QString previous = label->text();
        auto* color = canvas->findChild<QPushButton*>(sticker ? "stickerEditColor0Button"
                                                              : "annotationColor0Button");
        annotationWheel(*color, 120, color->rect().center());
        annotationWheel(*canvas, 120, {-10, -10});
        QCOMPARE(label->text(), previous);
        QCOMPARE(canvas->geometry(), geometry);
        const QString excluded[] = {"textToolButton", "coverToolButton", "mosaicToolButton"};
        for (const auto& tool : excluded)
        {
            const QString name =
                sticker ? QStringLiteral("stickerEdit") + tool[0].toUpper() + tool.mid(1) : tool;
            clickAnnotationTool(*canvas, qPrintable(name));
            QVERIFY(!label->isVisible());
            const qreal scale = sticker ? window->scale() : 1;
            annotationWheel(*canvas, 120);
            QCOMPARE(label->text(), previous);
            if (sticker)
                QVERIFY(window->scale() > scale);
            if (tool == QStringLiteral("textToolButton"))
            {
                QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, {130, 180});
                auto* editor = canvas->findChild<AnnotationTextEdit*>();
                QVERIFY(editor && editor->isVisible());
                editor->setPlainText(QStringLiteral("字号不受滚轮影响"));
                QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
            }
            else if (sticker)
                stickerDrag(*window, {130, 180}, {210, 200});
            else
                dragLogical(overlay, {130, 180}, {210, 200});
            annotations = sticker ? window->annotations() : overlay.annotations();
            QCOMPARE(annotations.last().style.lineWidth, expectedWidth);
            QCOMPARE(annotations.last().style.textSize, 28);
            QCOMPARE(annotations.last().style.color, annotationColors().front());
        }
        if (!sticker)
        {
            clickAnnotationTool(overlay, "ocrToolButton");
            annotationWheel(overlay, 120);
            QCOMPARE(label->text(), previous);
        }
        else
            window->setSaved(true);
    }
    void annotationGroupDefaultsAndMemory()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        auto* bar = overlay.findChild<QWidget*>(QStringLiteral("annotationToolBar"));
        QVERIFY(bar && !bar->isVisible());
        const char* groups[] = {"geometryToolButton", "directionToolButton", "concealToolButton"};
        const char* children[] = {"ellipseToolButton", "lineToolButton", "coverToolButton"};
        const AnnotationType defaults[] = {AnnotationType::Rectangle, AnnotationType::Arrow,
                                           AnnotationType::Mosaic};
        const AnnotationType variants[] = {AnnotationType::Ellipse, AnnotationType::Line,
                                           AnnotationType::Cover};
        const char* labels[] = {"椭圆", "直线", "遮盖"};
        QTest::mouseClick(button(overlay, "geometryToolButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "annotationColor2Button"), Qt::LeftButton);
        chooseAnnotationWidth(overlay, 8);
        QTest::mouseClick(button(overlay, "geometryToolButton"), Qt::LeftButton);
        for (int index = 0; index < 3; ++index)
        {
            auto* primary = button(overlay, groups[index]);
            const QImage oldIcon = primary->icon().pixmap(18, 18).toImage();
            QTest::mouseClick(primary, Qt::LeftButton);
            QVERIFY(overlay.activeTool() == defaults[index]);
            QVERIFY(bar->isVisible());
            auto* child = button(overlay, children[index]);
            QVERIFY(child->isVisible());
            QTest::mouseClick(child, Qt::LeftButton);
            QVERIFY(overlay.activeTool() == variants[index]);
            QVERIFY(primary->toolTip().contains(QString::fromUtf8(labels[index])));
            QCOMPARE(primary->accessibleName(), primary->toolTip());
            QVERIFY(primary->icon().pixmap(18, 18).toImage() != oldIcon);
            dragLogical(overlay, {120, 120}, {200, 180});
            QCOMPARE(overlay.annotations().size(), qsizetype(index + 1));
            QCOMPARE(overlay.annotations().last().type, variants[index]);
            QCOMPARE(overlay.annotations().last().style.color, annotationColors()[2]);
            QCOMPARE(overlay.annotations().last().style.lineWidth, 8);
            QTest::mouseClick(child, Qt::LeftButton);
            QVERIFY(overlay.activeTool() == variants[index]);
            QTest::mouseClick(primary, Qt::LeftButton);
            QVERIFY(!overlay.activeTool());
            QVERIFY(!bar->isVisible());
        }
        const auto history = overlay.annotations();
        QTemporaryDir temporary;
        QVERIFY(overlay.exportToPath(temporary.filePath(QStringLiteral("分组保存.png"))));
        for (int index = 0; index < 3; ++index)
        {
            QTest::mouseClick(button(overlay, groups[index]), Qt::LeftButton);
            QVERIFY(overlay.activeTool() == variants[index]);
            compareAnnotations(overlay.annotations(), history);
            QVERIFY(overlay.isSelectionSaved());
        }
        clickAnnotationTool(overlay, "freehandToolButton");
        QVERIFY(!bar->isVisible());
        clickAnnotationTool(overlay, "textToolButton");
        QVERIFY(!bar->isVisible());
    }
    void annotationGroupPanelGesturesAndPlacement_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<qreal>("dpr");
        for (int width : {320, 480, 800})
            for (qreal dpr : {qreal(1), qreal(1.5), qreal(2)})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(width).arg(dpr)))
                    << width << dpr;
    }
    void annotationGroupPanelGesturesAndPlacement()
    {
        QFETCH(int, width);
        QFETCH(qreal, dpr);
        using namespace waibusnap;
        auto frame = annotationFrame();
        frame.display.logicalGeometry = {0, 0, width, 600};
        frame.display.availableLogicalGeometry = {0, 25, width, 505};
        frame.display.devicePixelRatio = dpr;
        frame.pixels = QImage(QSize(width, 600) * dpr, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(dpr);
        SelectionOverlay overlay(frame);
        overlay.show();
        dragLogical(overlay, {0, 0}, {width, 600});
        QTest::mouseClick(button(overlay, "ratioButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "shapeButton"), Qt::LeftButton);
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 150});
        overlay.findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("分组提交"));
        QTest::mouseClick(button(overlay, "geometryToolButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        const QRect selection = overlay.selection();
        auto* sub = overlay.findChild<QWidget*>(QStringLiteral("annotationToolBar"));
        const QString panels[] = {QStringLiteral("selectionToolbar"),
                                  QStringLiteral("annotationToolBar"),
                                  QStringLiteral("annotationOptionsBar"),
                                  QStringLiteral("ratioBar"), QStringLiteral("shapeBar")};
        QRect previous;
        for (const auto& name : panels)
        {
            auto* panel = overlay.findChild<QWidget*>(name);
            QVERIFY(panel && panel->isVisible());
            QVERIFY(frame.display.availableLogicalGeometry.contains(panel->geometry()));
            QVERIFY(!previous.intersects(panel->geometry()));
            previous = panel->geometry();
        }
        QTest::mouseClick(sub, Qt::LeftButton, Qt::NoModifier, {1, 1});
        QCOMPARE(overlay.selection(), selection);
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {60, 150});
        QVERIFY(sub->isVisible() && !sub->isEnabled());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {100, 200});
        QVERIFY(sub->isEnabled());
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
    }
    void stickerAnnotationGroupsAndMenu()
    {
        using namespace waibusnap;
        StickerManager manager;
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QVERIFY(manager.create(image, {40, 50}, true).success);
        auto sticker = manager.windows().first();
        sticker->setEditing(true);
        auto* menu = sticker->findChild<QMenu*>(QStringLiteral("stickerEditMenu"));
        auto* bar = sticker->findChild<QWidget*>(QStringLiteral("stickerEditToolBar"));
        QVERIFY(menu && bar && !bar->isVisible());
        const int groupSlots[] = {0, 1, 4};
        const AnnotationType defaults[] = {AnnotationType::Rectangle, AnnotationType::Arrow,
                                           AnnotationType::Mosaic};
        const AnnotationType variants[] = {AnnotationType::Ellipse, AnnotationType::Line,
                                           AnnotationType::Cover};
        for (int index = 0; index < 3; ++index)
        {
            auto* action = menu->actions()[groupSlots[index]];
            auto* primary = sticker->findChild<QPushButton*>(action->data().toString());
            QTest::mouseClick(primary, Qt::LeftButton);
            QVERIFY(sticker->activeTool() == defaults[index]);
            QVERIFY(bar->isVisible());
            QVERIFY(sticker->rect().contains(bar->geometry()));
            auto* variant = action->menu()->actions()[index == 2 ? 0 : index == 1 ? 0 : 1];
            variant->trigger();
            QVERIFY(sticker->activeTool() == variants[index]);
            QVERIFY(action->isChecked() && variant->isChecked());
            QCOMPARE(action->text(), annotationToolName(variants[index]));
            QCOMPARE(primary->accessibleName(), primary->toolTip());
            stickerDrag(*sticker, {50, 100}, {100, 150});
            QCOMPARE(sticker->annotations().size(), qsizetype(index + 1));
            QCOMPARE(sticker->annotations().last().type, variants[index]);
            QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, {1, 1});
            QCOMPARE(sticker->annotations().size(), qsizetype(index + 1));
            QTest::mouseClick(primary, Qt::LeftButton);
            QVERIFY(!sticker->activeTool());
            QVERIFY(!bar->isVisible());
        }
        const auto history = sticker->annotations();
        QSignalSpy shown(menu, &QMenu::aboutToShow);
        QContextMenuEvent context(QContextMenuEvent::Mouse, {100, 100},
                                  sticker->mapToGlobal(QPoint(100, 100)));
        QApplication::sendEvent(sticker, &context);
        QCOMPARE(shown.size(), 1);
        menu->hide();
        compareAnnotations(sticker->annotations(), history);
        sticker->setEditing(false);
        QVERIFY(!bar->isVisible());
        sticker->setEditing(true);
        for (int index = 0; index < 3; ++index)
        {
            auto* primary = sticker->findChild<QPushButton*>(
                menu->actions()[groupSlots[index]]->data().toString());
            QTest::mouseClick(primary, Qt::LeftButton);
            QVERIFY(sticker->activeTool() == variants[index]);
            compareAnnotations(sticker->annotations(), history);
        }
        sticker->setSaved(true);
    }
    void toolbarCompactIconsAndTooltips()
    {
        using namespace waibusnap;
        OverlayActions actions;
        actions.recognizer = std::make_shared<OcrStub>();
        SelectionOverlay overlay(annotationFrame(), actions);
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* options = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        QVERIFY(toolbar && options && !options->isVisible());
        QCOMPARE(toolbar->height(), 44);
        for (auto* control : toolbar->findChildren<QPushButton*>())
        {
            QVERIFY(!control->icon().isNull());
            QVERIFY(control->text().isEmpty());
            QVERIFY(!control->toolTip().isEmpty());
            QCOMPARE(control->accessibleName(), control->toolTip());
            QCOMPARE(control->size(), QSize(28, 28));
            QCOMPARE(control->iconSize(), QSize(18, 18));
            if (control->isVisible())
                QCOMPARE(control->y(), 8);
        }
        QCOMPARE(button(overlay, "saveButton")->toolTip(), QStringLiteral("保存（快速保存）"));
        QCOMPARE(button(overlay, "cancelButton")->toolTip(), QStringLiteral("取消（Esc）"));
        for (int index = 0; index < 19; ++index)
        {
            const auto icon = toolbarIcon(static_cast<ToolbarIcon>(index), annotationColors()[0]);
            for (auto mode : {QIcon::Normal, QIcon::Disabled})
                for (int scale : {1, 2})
                {
                    const QPixmap pixmap = icon.pixmap(QSize(18, 18), qreal(scale), mode);
                    QCOMPARE(pixmap.size(), QSize(18, 18) * scale);
                    QCOMPARE(pixmap.devicePixelRatio(), qreal(scale));
                    const QImage pixels = pixmap.toImage();
                    bool drawn = false;
                    for (int y = 0; y < pixels.height(); ++y)
                        for (int x = 0; x < pixels.width(); ++x)
                            drawn |= pixels.pixelColor(x, y).alpha() > 0;
                    QVERIFY(drawn);
                }
        }
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        auto* extract = button(overlay, "extractAllTextButton");
        QVERIFY(extract->isVisible());
        QVERIFY(toolbar->rect().contains(extract->geometry()));
        QVERIFY(extract->x() > button(overlay, "ocrToolButton")->x());
        QVERIFY(extract->x() < button(overlay, "undoAnnotationButton")->x());
        QCOMPARE(toolbar->height(), 44);
        QVERIFY(!options->isVisible());
        QVERIFY(!button(overlay, "undoAnnotationButton")->isEnabled());
        QVERIFY(!button(overlay, "redoAnnotationButton")->isEnabled());
    }
    void toolbarOptionsFollowActiveTool()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {100, 100}, {300, 250});
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* options = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        const char* names[] = {"rectangleToolButton", "ellipseToolButton",  "lineToolButton",
                               "arrowToolButton",     "freehandToolButton", "textToolButton",
                               "coverToolButton",     "mosaicToolButton",   "ocrToolButton"};
        QVERIFY(!options->isVisible());
        for (int tool = 0; tool < 9; ++tool)
        {
            clickAnnotationTool(overlay, names[tool]);
            QCOMPARE(options->isVisible(), tool < 7);
            for (int index = 0; index < 3; ++index)
            {
                auto* color = button(
                    overlay, qPrintable(QStringLiteral("annotationColor%1Button").arg(index)));
                auto* width = overlay.findChild<QLabel*>("annotationWidthLabel");
                auto* size = button(
                    overlay, qPrintable(QStringLiteral("annotationTextSize%1Button").arg(index)));
                QCOMPARE(color->isVisible(), tool < 7);
                QCOMPARE(width->isVisible(), tool < 5);
                QCOMPARE(size->isVisible(), tool == 5);
                if (color->isVisible())
                {
                    QVERIFY(options->rect().contains(color->geometry()));
                    QTest::mouseClick(color, Qt::LeftButton);
                    QVERIFY(color->isChecked());
                }
            }
            QCOMPARE(toolbar->height(), 44);
            if (tool < 7)
            {
                QCOMPARE(options->height(), 68);
                QVERIFY(button(overlay, "annotationColor2Button")->isChecked());
                QVERIFY(!button(overlay, "annotationColor0Button")->isChecked());
                QVERIFY(!button(overlay, "annotationColor1Button")->isChecked());
            }
            clickAnnotationTool(overlay, names[tool]);
            QVERIFY(!options->isVisible());
            QVERIFY(!overlay.activeTool());
            QVERIFY(!overlay.isTextMode());
        }
        clickAnnotationTool(overlay, "lineToolButton");
        chooseAnnotationWidth(overlay, 8);
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(button(overlay, "annotationTextSize0Button"), Qt::LeftButton);
        clickAnnotationTool(overlay, "lineToolButton");
        QCOMPARE(overlay.findChild<QLabel*>("annotationWidthLabel")->text(),
                 QStringLiteral("线宽 8px"));
        QVERIFY(button(overlay, "annotationColor2Button")->isChecked());
        clickAnnotationTool(overlay, "textToolButton");
        QVERIFY(button(overlay, "annotationTextSize0Button")->isChecked());
    }
    void toolbarStatusPillIndependent_data()
    {
        QTest::addColumn<bool>("fullScreen");
        QTest::addColumn<bool>("longMessage");
        for (bool full : {false, true})
            for (bool longText : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("full-%1-long-%2").arg(full).arg(longText)))
                    << full << longText;
    }
    void toolbarStatusPillIndependent()
    {
        QFETCH(bool, fullScreen);
        QFETCH(bool, longMessage);
        using namespace waibusnap;
        auto frame = annotationFrame();
        frame.display.availableLogicalGeometry = {0, 25, 800, 505};
        OverlayActions actions;
        const QString message = longMessage ? QStringLiteral("很长的输出错误说明 ").repeated(30)
                                            : QStringLiteral("测试状态提示");
        actions.copyImage = [message](const QImage&) { return ImageOutputResult{false, message}; };
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        dragLogical(overlay, fullScreen ? QPoint(0, 0) : QPoint(100, 100),
                    fullScreen ? QPoint(800, 600) : QPoint(300, 200));
        clickAnnotationTool(overlay, "rectangleToolButton");
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* options = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        const QSize toolbarSize = toolbar->size();
        const QSize optionsSize = options->size();
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        auto* pill = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QCOMPARE(pill->parentWidget(), &overlay);
        QVERIFY(pill->isVisible());
        QCOMPARE(pill->toolTip(), pill->text());
        QCOMPARE(toolbar->size(), toolbarSize);
        QCOMPARE(options->size(), optionsSize);
        QVERIFY(pill->width() <= toolbar->width());
        QVERIFY(pill->height() >= 26);
        QVERIFY(pill->height() >= pill->heightForWidth(pill->width()));
        if (longMessage)
            QVERIFY(pill->height() > 26);
        QCOMPARE(pill->geometry().right(), toolbar->geometry().right());
        QVERIFY(frame.display.availableLogicalGeometry.contains(pill->geometry()));
        QVERIFY(!pill->geometry().intersects(toolbar->geometry()));
        QVERIFY(!pill->geometry().intersects(options->geometry()));
        if (fullScreen)
            QCOMPARE(pill->geometry().bottom() + 11, toolbar->y());
        else
            QCOMPARE(pill->y(), options->geometry().bottom() + 11);
        pill->hide();
        QCOMPARE(toolbar->size(), toolbarSize);
        QCOMPARE(options->size(), optionsSize);
    }
    void toolbarStatusPillTimesOut()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {0, 0}, {800, 600});
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        clickAnnotationTool(overlay, "mosaicToolButton");
        auto* pill = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(pill->isVisible());
        const QRect geometry = toolbar->geometry();
        QTRY_VERIFY_WITH_TIMEOUT(!pill->isVisible(), 3500);
        QCOMPARE(toolbar->geometry(), geometry);
    }
    void fullScreenToolbarUsesInsideBottom()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        dragLogical(overlay, {0, 0}, {800, 600});
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        QVERIFY(toolbar && toolbar->isVisible());
        QCOMPARE(toolbar->geometry().bottom(), overlay.rect().bottom() - 10);
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
    }
    void coverOverlayToolbarFitsNarrowScreens_data()
    {
        QTest::addColumn<int>("screenWidth");
        for (int width : {320, 480, 533, 534, 559, 560, 565, 566, 586, 587, 597, 598,
                          618, 619, 629, 630, 639, 640, 650, 651, 682, 683, 720, 800})
            QTest::newRow(qPrintable(QString::number(width))) << width;
    }
    void coverOverlayToolbarFitsNarrowScreens()
    {
        QFETCH(int, screenWidth);
        using namespace waibusnap;
        auto frame = annotationFrame();
        frame.display.logicalGeometry.setWidth(screenWidth);
        frame.pixels = QImage(screenWidth * 2, 1200, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(2);
        OverlayActions actions;
        actions.recognizer = std::make_shared<OcrStub>();
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        drag(overlay, {100, 100}, {250, 200});
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* first = button(overlay, "geometryToolButton");
        auto* privacy = button(overlay, "concealToolButton");
        QVERIFY(toolbar && first && privacy);
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
        QVERIFY(toolbar->rect().contains(privacy->geometry()));
        QVERIFY(privacy->isVisible());
        QCOMPARE(privacy->y() == first->y(), screenWidth > 320);
        for (auto* control : toolbar->findChildren<QPushButton*>())
            if (!control->isHidden())
            {
                QVERIFY(control->isVisible());
                QVERIFY(toolbar->rect().contains(control->geometry()));
            }
        QCOMPARE(toolbar->height(), screenWidth >= 534 ? 44 : screenWidth > 320 ? 76 : 140);
        clickAnnotationTool(overlay, "coverToolButton");
        QVERIFY(overlay.activeTool() == AnnotationType::Cover);
        clickAnnotationTool(overlay, "mosaicToolButton");
        QVERIFY(overlay.activeTool() == AnnotationType::Mosaic);
        auto* sub = overlay.findChild<QWidget*>(QStringLiteral("annotationToolBar"));
        QVERIFY(sub->isVisible());
        QVERIFY(overlay.rect().contains(sub->geometry()));
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status->isVisible());
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
        QTest::mouseClick(button(overlay, "ocrToolButton"), Qt::LeftButton);
        QTRY_VERIFY(!overlay.isRecognizingText());
        QVERIFY(button(overlay, "extractAllTextButton")->isVisible());
        QCOMPARE(toolbar->height(), screenWidth >= 587 ? 44 : screenWidth > 320 ? 76 : 140);
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
        for (auto* control : toolbar->findChildren<QPushButton*>())
        {
            QVERIFY(control->isVisible());
            QVERIFY(toolbar->rect().contains(control->geometry()));
        }
    }
    void coverOverlayGesturesHistoryAndOutput_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("normal") << qreal(1);
        QTest::newRow("fractional") << qreal(1.5);
        QTest::newRow("retina") << qreal(2);
    }
    void coverOverlayGesturesHistoryAndOutput()
    {
        QFETCH(qreal, dpr);
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("覆盖层遮盖.png"));
        QImage copied;
        OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&) { return path; };
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = dpr;
        frame.pixels = QImage(QSize(800, 600) * dpr, QImage::Format_ARGB32_Premultiplied);
        frame.pixels.fill(Qt::white);
        frame.pixels.setDevicePixelRatio(dpr);
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "rectangleToolButton");
        drag(overlay, {110, 110}, {240, 220});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {125, 125});
        overlay.findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("敏感 ABC 123"));
        auto* cover = button(overlay, "coverToolButton");
        QVERIFY(cover);
        QVERIFY(!cover->icon().isNull());
        QVERIFY(!cover->toolTip().isEmpty());
        QVERIFY(!cover->accessibleName().isEmpty());
        clickAnnotationTool(overlay, "coverToolButton");
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(overlay.activeTool() == AnnotationType::Cover);
        auto* options = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        auto* widths = overlay.findChild<QLabel*>("annotationWidthLabel");
        auto* sizes = button(overlay, "annotationTextSize0Button");
        auto* colors = button(overlay, "annotationColor0Button");
        QVERIFY(!widths->isVisible());
        QVERIFY(!sizes->isVisible());
        QVERIFY(colors->isVisible());
        clickAnnotationTool(overlay, "coverToolButton");
        QVERIFY(!overlay.activeTool());
        QVERIFY(!cover->isChecked());
        QVERIFY(!options->isVisible());
        clickAnnotationTool(overlay, "coverToolButton");
        QVERIFY(overlay.exportToPath(path));
        QVERIFY(overlay.isSelectionSaved());
        const QImage before(path);
        const QRect selection = overlay.selection();
        for (const QPoint end : {QPoint(115, 115), QPoint(115, 180), QPoint(225, 115)})
        {
            drag(overlay, {115, 115}, end);
            QCOMPARE(overlay.annotations().size(), qsizetype(2));
            QVERIFY(overlay.isSelectionSaved());
        }
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {115, 115});
        QTest::mouseMove(&overlay, {225, 180});
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(overlay.isSelectionSaved());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {225, 180});
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        QCOMPARE(overlay.annotations().last().type, AnnotationType::Cover);
        QCOMPARE(overlay.annotations().last().style.color, annotationColors()[0]);
        QCOMPARE(overlay.annotations().last().first, QPointF(115, 115) * dpr);
        QCOMPARE(overlay.selection(), selection);
        QVERIFY(!overlay.isSelectionSaved());
        const QRect covered = QRectF(QPointF(115, 115) * dpr, QPointF(225, 180) * dpr)
                                  .toAlignedRect()
                                  .translated(-selection.topLeft());
        QVERIFY(overlay.exportToPath(path));
        const QImage saved(path);
        compareCoveredPixels(saved, before, covered, annotationColors()[0]);
        QVERIFY(overlay.isSelectionSaved());
        QTest::mouseClick(button(overlay, "undoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(!overlay.isSelectionSaved());
        QCOMPARE(renderAnnotatedSelection(frame.pixels, overlay.annotations(), selection)
                     .convertToFormat(before.format()),
                 before);
        QTest::mouseClick(button(overlay, "redoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        QCOMPARE(renderAnnotatedSelection(frame.pixels, overlay.annotations(), selection)
                     .convertToFormat(saved.format()),
                 saved);
        QTest::mouseClick(button(overlay, "annotationColor2Button"), Qt::LeftButton);
        drag(overlay, {225, 180}, {115, 115});
        QCOMPARE(overlay.annotations().last().style.color, annotationColors()[2]);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        compareCoveredPixels(copied, before, covered, annotationColors()[2]);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 10);
        QVERIFY(overlay.annotations().isEmpty());
    }
    void coverStickerGesturesHistoryAndOutput_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::newRow("normal") << qreal(1);
        QTest::newRow("fractional") << qreal(0.75);
        QTest::newRow("enlarged") << qreal(2);
    }
    void coverStickerGesturesHistoryAndOutput()
    {
        QFETCH(qreal, scale);
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("贴图遮盖.png"));
        QImage copied;
        StickerActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.confirmClose = [](QWidget*) { return StickerCloseDecision::Discard; };
        QImage image(1200, 1000, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        StickerManager manager(nullptr, actions);
        QVERIFY(manager.create(image, {40, 50}, true).success);
        auto sticker = manager.windows().first();
        sticker->setScale(scale, sticker->pos());
        sticker->setEditing(true);
        const QRect geometry = sticker->geometry();
        const qreal factor = sticker->windowHandle()->screen()->devicePixelRatio() / scale;
        stickerClick(*sticker, "stickerEditRectangleToolButton");
        stickerDrag(*sticker, {80, 80}, {240, 200});
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
        sticker->findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("敏感 ABC 123"));
        auto* cover =
            sticker->findChild<QPushButton*>(QStringLiteral("stickerEditCoverToolButton"));
        QVERIFY(cover);
        QVERIFY(cover->text().isEmpty());
        QVERIFY(cover->accessibleName().contains(QStringLiteral("遮盖")));
        stickerClick(*sticker, "stickerEditCoverToolButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        QVERIFY(sticker->activeTool() == AnnotationType::Cover);
        auto* widths = sticker->findChild<QLabel*>(QStringLiteral("stickerEditWidthLabel"));
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        auto* colors = sticker->findChild<QWidget*>(QStringLiteral("stickerEditColorPalette"));
        QVERIFY(!widths->isVisible());
        QVERIFY(!sizes->isEnabled());
        QVERIFY(colors->isEnabled());
        stickerClick(*sticker, "stickerEditCoverToolButton");
        QVERIFY(!sticker->activeTool());
        QVERIFY(!cover->isChecked());
        QVERIFY(!widths->isVisible());
        stickerClick(*sticker, "stickerEditCoverToolButton");
        QVERIFY(sticker->exportToPath(path));
        const QImage before(path);
        QVERIFY(sticker->isSaved());
        for (const QPoint end : {QPoint(85, 85), QPoint(85, 170), QPoint(220, 85)})
        {
            stickerDrag(*sticker, {85, 85}, end);
            QCOMPARE(sticker->annotations().size(), qsizetype(2));
            QVERIFY(sticker->isSaved());
        }
        stickerDrag(*sticker, {85, 85}, {220, 170});
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        QCOMPARE(sticker->annotations().last().type, AnnotationType::Cover);
        QCOMPARE(sticker->annotations().last().style.color, annotationColors()[0]);
        QCOMPARE(sticker->annotations().last().first, QPointF(85, 85) * factor);
        QCOMPARE(sticker->geometry(), geometry);
        QVERIFY(!sticker->isSaved());
        const QRect covered =
            QRectF(QPointF(85, 85) * factor, QPointF(220, 170) * factor).toAlignedRect();
        QVERIFY(sticker->exportToPath(path));
        const QImage saved(path);
        compareCoveredPixels(saved, before, covered, annotationColors()[0]);
        QVERIFY(sticker->isSaved());
        stickerClick(*sticker, "stickerEditUndoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        QVERIFY(!sticker->isSaved());
        QCOMPARE(sticker->renderedImage().convertToFormat(before.format()), before);
        stickerClick(*sticker, "stickerEditRedoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        QCOMPARE(sticker->renderedImage().convertToFormat(saved.format()), saved);
        chooseAnnotationColor(*sticker, 2, true);
        stickerDrag(*sticker, {220, 170}, {85, 85});
        QCOMPARE(sticker->annotations().last().style.color, annotationColors()[2]);
        stickerClick(*sticker, "stickerEditCopyButton");
        compareCoveredPixels(copied, before, covered, annotationColors()[2]);
        QVERIFY(!sticker->isSaved());
        QVERIFY(sticker->isEditing());
    }
    void mosaicOverlayGesturesHistoryAndOutput_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("normal") << qreal(1);
        QTest::newRow("fractional") << qreal(1.5);
        QTest::newRow("retina") << qreal(2);
    }
    void mosaicOverlayGesturesHistoryAndOutput()
    {
        QFETCH(qreal, dpr);
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("覆盖层马赛克.png"));
        QImage copied;
        OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&) { return path; };
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = dpr;
        frame.pixels = mosaicTestImage(QSize(800, 600) * dpr);
        frame.pixels.setDevicePixelRatio(dpr);
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "rectangleToolButton");
        drag(overlay, {110, 110}, {240, 220});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {125, 125});
        overlay.findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("敏感 ABC 123"));
        auto* mosaic = button(overlay, "mosaicToolButton");
        QVERIFY(mosaic);
        QVERIFY(!mosaic->icon().isNull());
        QVERIFY(!mosaic->toolTip().isEmpty());
        QVERIFY(!mosaic->accessibleName().isEmpty());
        const auto notice = QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖");
        QCOMPARE(mosaic->toolTip(), notice);
        QCOMPARE(mosaic->accessibleName(), notice);
        clickAnnotationTool(overlay, "mosaicToolButton");
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(overlay.activeTool() == AnnotationType::Mosaic);
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), notice);
        status->hide();
        status->setText(QStringLiteral("保留反馈"));
        auto* options = overlay.findChild<QWidget*>(QStringLiteral("annotationOptionsBar"));
        auto* widths = overlay.findChild<QLabel*>("annotationWidthLabel");
        auto* sizes = button(overlay, "annotationTextSize0Button");
        auto* colors = button(overlay, "annotationColor0Button");
        QVERIFY(!widths->isVisible());
        QVERIFY(!sizes->isVisible());
        QVERIFY(!colors->isVisible());
        clickAnnotationTool(overlay, "mosaicToolButton");
        QVERIFY(!overlay.activeTool());
        QVERIFY(!mosaic->isChecked());
        QVERIFY(!options->isVisible());
        QVERIFY(!options->isVisible());
        clickAnnotationTool(overlay, "mosaicToolButton");
        QCOMPARE(status->text(), QStringLiteral("保留反馈"));
        QVERIFY(!status->isVisible());
        QVERIFY(overlay.exportToPath(path));
        QVERIFY(overlay.isSelectionSaved());
        const QImage before(path);
        const QRect selection = overlay.selection();
        for (const QPoint end : {QPoint(115, 115), QPoint(115, 180), QPoint(225, 115)})
        {
            drag(overlay, {115, 115}, end);
            QCOMPARE(overlay.annotations().size(), qsizetype(2));
            QVERIFY(overlay.isSelectionSaved());
        }
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {115, 115});
        QTest::mouseMove(&overlay, {225, 180});
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(overlay.isSelectionSaved());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {225, 180});
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        QCOMPARE(overlay.annotations().last().type, AnnotationType::Mosaic);
        QCOMPARE(overlay.annotations().last().style.color, annotationColors()[0]);
        QCOMPARE(overlay.annotations().last().first, QPointF(115, 115) * dpr);
        QCOMPARE(overlay.selection(), selection);
        QVERIFY(!overlay.isSelectionSaved());
        const QRect covered = QRectF(QPointF(115, 115) * dpr, QPointF(225, 180) * dpr)
                                  .toAlignedRect()
                                  .translated(-selection.topLeft());
        QVERIFY(overlay.exportToPath(path));
        const QImage saved(path);
        compareMosaicPixels(saved, before, covered);
        QVERIFY(overlay.isSelectionSaved());
        QTest::mouseClick(button(overlay, "undoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(!overlay.isSelectionSaved());
        QCOMPARE(renderAnnotatedSelection(frame.pixels, overlay.annotations(), selection)
                     .convertToFormat(before.format()),
                 before);
        QTest::mouseClick(button(overlay, "redoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        QCOMPARE(renderAnnotatedSelection(frame.pixels, overlay.annotations(), selection)
                     .convertToFormat(saved.format()),
                 saved);
        // 参数条在马赛克模式隐藏；经颜色工具设置后切回，验证参数保留且不影响像素化。
        clickAnnotationTool(overlay, "coverToolButton");
        QTest::mouseClick(button(overlay, "annotationColor2Button"), Qt::LeftButton);
        clickAnnotationTool(overlay, "mosaicToolButton");
        drag(overlay, {225, 180}, {115, 115});
        QCOMPARE(overlay.annotations().last().style.color, annotationColors()[2]);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        compareMosaicPixels(copied, before, covered);
        QCOMPARE(copied.convertToFormat(saved.format()), saved);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 10);
        QVERIFY(overlay.annotations().isEmpty());
    }
    void mosaicStickerGesturesHistoryAndOutput_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::newRow("normal") << qreal(1);
        QTest::newRow("fractional") << qreal(0.75);
        QTest::newRow("enlarged") << qreal(2);
    }
    void mosaicStickerGesturesHistoryAndOutput()
    {
        QFETCH(qreal, scale);
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("贴图马赛克.png"));
        QImage copied;
        StickerActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.confirmClose = [](QWidget*) { return StickerCloseDecision::Discard; };
        QImage image = mosaicTestImage({1200, 1000});
        StickerManager manager(nullptr, actions);
        QVERIFY(manager.create(image, {40, 50}, true).success);
        auto sticker = manager.windows().first();
        sticker->setScale(scale, sticker->pos());
        sticker->setEditing(true);
        const QRect geometry = sticker->geometry();
        const qreal factor = sticker->windowHandle()->screen()->devicePixelRatio() / scale;
        stickerClick(*sticker, "stickerEditRectangleToolButton");
        stickerDrag(*sticker, {80, 80}, {240, 200});
        stickerClick(*sticker, "stickerEditTextToolButton");
        QTest::mouseClick(sticker, Qt::LeftButton, Qt::NoModifier, {100, 100});
        sticker->findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("敏感 ABC 123"));
        auto* mosaic =
            sticker->findChild<QPushButton*>(QStringLiteral("stickerEditMosaicToolButton"));
        QVERIFY(mosaic);
        QVERIFY(mosaic->text().isEmpty());
        const auto notice = QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖");
        QCOMPARE(mosaic->toolTip(), notice);
        QCOMPARE(mosaic->accessibleName(), notice);
        stickerClick(*sticker, "stickerEditMosaicToolButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        QVERIFY(sticker->activeTool() == AnnotationType::Mosaic);
        auto* status = sticker->findChild<QLabel*>(QStringLiteral("stickerStatus"));
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), notice);
        status->hide();
        status->setText(QStringLiteral("保留反馈"));
        auto* widths = sticker->findChild<QLabel*>(QStringLiteral("stickerEditWidthLabel"));
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        auto* colors = sticker->findChild<QWidget*>(QStringLiteral("stickerEditColorPalette"));
        QVERIFY(!widths->isVisible());
        QVERIFY(!sizes->isEnabled());
        QVERIFY(!colors->isEnabled());
        stickerClick(*sticker, "stickerEditMosaicToolButton");
        QVERIFY(!sticker->activeTool());
        QVERIFY(!mosaic->isChecked());
        QVERIFY(!widths->isVisible());
        QVERIFY(colors->isEnabled());
        stickerClick(*sticker, "stickerEditMosaicToolButton");
        QCOMPARE(status->text(), QStringLiteral("保留反馈"));
        QVERIFY(!status->isVisible());
        QVERIFY(sticker->exportToPath(path));
        const QImage before(path);
        QVERIFY(sticker->isSaved());
        for (const QPoint end : {QPoint(85, 85), QPoint(85, 170), QPoint(220, 85)})
        {
            stickerDrag(*sticker, {85, 85}, end);
            QCOMPARE(sticker->annotations().size(), qsizetype(2));
            QVERIFY(sticker->isSaved());
        }
        stickerDrag(*sticker, {85, 85}, {220, 170});
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        QCOMPARE(sticker->annotations().last().type, AnnotationType::Mosaic);
        QCOMPARE(sticker->annotations().last().style.color, annotationColors()[0]);
        QCOMPARE(sticker->annotations().last().first, QPointF(85, 85) * factor);
        QCOMPARE(sticker->geometry(), geometry);
        QVERIFY(!sticker->isSaved());
        const QRect covered =
            QRectF(QPointF(85, 85) * factor, QPointF(220, 170) * factor).toAlignedRect();
        QVERIFY(sticker->exportToPath(path));
        const QImage saved(path);
        compareMosaicPixels(saved, before, covered);
        QVERIFY(sticker->isSaved());
        stickerClick(*sticker, "stickerEditUndoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        QVERIFY(!sticker->isSaved());
        QCOMPARE(sticker->renderedImage().convertToFormat(before.format()), before);
        stickerClick(*sticker, "stickerEditRedoAnnotationButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(3));
        QCOMPARE(sticker->renderedImage().convertToFormat(saved.format()), saved);
        stickerClick(*sticker, "stickerEditLineToolButton");
        chooseAnnotationColor(*sticker, 2, true);
        stickerClick(*sticker, "stickerEditMosaicToolButton");
        stickerDrag(*sticker, {220, 170}, {85, 85});
        QCOMPARE(sticker->annotations().last().style.color, annotationColors()[2]);
        stickerClick(*sticker, "stickerEditCopyButton");
        compareMosaicPixels(copied, before, covered);
        QCOMPARE(copied.convertToFormat(saved.format()), saved);
        QVERIFY(!sticker->isSaved());
        QVERIFY(sticker->isEditing());
    }
    void annotationToolsAreDiscoverableAndGesturesTakePriority()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        QVERIFY(!overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar")));
        QVERIFY(!overlay.findChild<AnnotationTextEdit*>());
        drag(overlay, {100, 100}, {300, 250});
        const QRect selection = overlay.selection();
        const char* names[] = {"rectangleToolButton", "ellipseToolButton",  "lineToolButton",
                               "arrowToolButton",     "freehandToolButton", "textToolButton",
                               "coverToolButton",     "mosaicToolButton"};
        const QString labels[] = {QStringLiteral("矩形"), QStringLiteral("椭圆"),
                                  QStringLiteral("直线"), QStringLiteral("箭头"),
                                  QStringLiteral("画笔"), QStringLiteral("文本"),
                                  QStringLiteral("遮盖"), QStringLiteral("马赛克")};
        for (int index = 0; index < 8; ++index)
        {
            clickAnnotationTool(overlay, names[index]);
            auto* tool = button(overlay, names[index]);
            QVERIFY(tool && tool->isVisible());
            QVERIFY(tool->text().isEmpty());
            QVERIFY(!tool->icon().isNull());
            QVERIFY(tool->accessibleName().contains(labels[index]));
            QVERIFY(tool->toolTip().contains(labels[index]));
            QVERIFY(overlay.activeTool() == static_cast<AnnotationType>(index));
            QVERIFY(tool->isChecked());
            if (index == 5)
            {
                QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 140});
                auto* editor = overlay.findChild<AnnotationTextEdit*>();
                QVERIFY(editor && editor->isVisible());
                editor->setPlainText(QStringLiteral("截图说明 ABC 123\n第二行"));
                QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
                QVERIFY(!editor->isVisible());
            }
            else
            {
                // 包括选区边角、内部与选区外，均优先绘图。
                drag(overlay, {100, 100}, {330, 280});
            }
            QCOMPARE(overlay.selection(), selection);
            QCOMPARE(overlay.annotations().size(), qsizetype(index + 1));
            QCOMPARE(overlay.annotations().last().type, static_cast<AnnotationType>(index));
            QCOMPARE(overlay.annotations().last().first,
                     QPointF(index == 5 ? 240 : 200, index == 5 ? 280 : 200));
            clickAnnotationTool(overlay, names[index]);
            QVERIFY(!overlay.activeTool());
            QVERIFY(!tool->isChecked());
        }
        const auto before = overlay.annotations().first();
        drag(overlay, {200, 180}, {220, 190});
        QCOMPARE(overlay.selection(), QRect(240, 220, 400, 300));
        QCOMPARE(overlay.annotations().first().first, before.first);
        QCOMPARE(overlay.annotations().first().last, before.last);
        drag(overlay, {320, 260}, {350, 280});
        QCOMPARE(overlay.annotations().first().first, before.first);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(overlay.annotations().isEmpty());
    }
    void annotationStylesUndoRedoAndDirtyState()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {300, 250});
        auto* undo = button(overlay, "undoAnnotationButton");
        auto* redo = button(overlay, "redoAnnotationButton");
        QVERIFY(undo && redo);
        for (int index = 0; index < 3; ++index)
        {
            QVERIFY(
                button(overlay, qPrintable(QStringLiteral("annotationColor%1Button").arg(index))));
            QVERIFY(overlay.findChild<QLabel*>("annotationWidthLabel"));
            QVERIFY(button(overlay,
                           qPrintable(QStringLiteral("annotationTextSize%1Button").arg(index))));
        }
        QVERIFY(!undo->isEnabled());
        QVERIFY(!redo->isEnabled());
        clickAnnotationTool(overlay, "lineToolButton");
        for (int index = 0; index < 3; ++index)
        {
            QTest::mouseClick(
                button(overlay, qPrintable(QStringLiteral("annotationColor%1Button").arg(index))),
                Qt::LeftButton);
            chooseAnnotationWidth(overlay, sampleLineWidths[index]);
            drag(overlay, {120, 150 + index * 20}, {280, 150 + index * 20});
            QCOMPARE(overlay.annotations().last().style.color, annotationColors()[index]);
            QCOMPARE(overlay.annotations().last().style.lineWidth, sampleLineWidths[index]);
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath(QStringLiteral("标注.png"));
        QVERIFY(overlay.exportToPath(path));
        const QImage saved(path);
        QVERIFY(overlay.isSelectionSaved());
        QTest::mouseClick(undo, Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(!overlay.isSelectionSaved());
        QVERIFY(redo->isEnabled());
        standardKey(&overlay, QKeySequence::Redo);
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        QVERIFY(!redo->isEnabled());
        QVERIFY(overlay.exportToPath(path));
        QCOMPARE(QImage(path), saved);
        standardKey(&overlay, QKeySequence::Undo);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QTest::mouseClick(redo, Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        QVERIFY(!overlay.isSelectionSaved());
        standardKey(&overlay, QKeySequence::Undo);
        drag(overlay, {120, 210}, {280, 210});
        QVERIFY(!redo->isEnabled());
        standardKey(&overlay, QKeySequence::Redo);
        QCOMPARE(overlay.annotations().size(), qsizetype(3));
        clickAnnotationTool(overlay, "textToolButton");
        QVERIFY(!overlay.findChild<QLabel*>("annotationWidthLabel")->isVisible());
        QVERIFY(button(overlay, "annotationTextSize0Button")->isVisible());
        QTest::mouseClick(button(overlay, "annotationTextSize2Button"), Qt::LeftButton);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 120});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        editor->setPlainText(QStringLiteral("ABC"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(overlay.annotations().last().style.textSize, annotationTextSizes[2]);
    }
    void annotationTextTransparentBothEditors_data()
    {
        QTest::addColumn<bool>("stickerEditor");
        QTest::addColumn<qreal>("factor");
        for (qreal dpr : {1.0, 1.5, 2.0})
            QTest::newRow(qPrintable(QStringLiteral("overlay-%1").arg(dpr))) << false << dpr;
        for (qreal scale : {0.75, 1.0, 2.0})
            QTest::newRow(qPrintable(QStringLiteral("sticker-%1").arg(scale))) << true << scale;
    }
    void annotationTextTransparentBothEditors()
    {
        using namespace waibusnap;
        QFETCH(bool, stickerEditor);
        QFETCH(qreal, factor);
        auto frame = annotationFrame();
        if (!stickerEditor)
        {
            frame.display.devicePixelRatio = factor;
            frame.pixels = QImage(qRound(800 * factor), qRound(600 * factor),
                                  QImage::Format_ARGB32_Premultiplied);
            frame.pixels.fill(Qt::white);
            frame.pixels.setDevicePixelRatio(factor);
        }
        StickerManager manager;
        SelectionOverlay overlay(frame);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        StickerWindow* sticker = nullptr;
        QWidget* surface = &overlay;
        if (stickerEditor)
        {
            QVERIFY(manager.create(annotationFrame().pixels, {40, 50}, true).success);
            sticker = manager.windows().first();
            sticker->setScale(factor, sticker->geometry().center());
            sticker->setEditing(true);
            surface = sticker;
            stickerClick(*sticker, "stickerEditTextToolButton");
        }
        else
        {
            overlay.show();
            drag(overlay, {40, 40}, {700, 500});
            clickAnnotationTool(overlay, "textToolButton");
        }
        chooseAnnotationColor(*surface, 5, stickerEditor);
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, {100, 100});
        auto* editor = surface->findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        QCOMPARE(editor->palette().color(QPalette::Base).alpha(), 0);
        QCOMPARE(editor->viewport()->palette().color(QPalette::Base).alpha(), 0);
        QCOMPARE(editor->frameShape(), QFrame::NoFrame);
        QCOMPARE(editor->palette().color(QPalette::Text), annotationColors()[5]);
        QCOMPARE(editor->width(), 240);
        const int initialHeight = editor->height();
        QVERIFY(initialHeight < QFontMetricsF(editor->font()).height() * 2);
        const QPoint anchor = editor->pos();
        editor->setPlainText(QString(200, 'W'));
        QCOMPARE(editor->width(), 400);
        QCOMPARE(editor->height(), initialHeight);
        editor->setPlainText(QStringLiteral("第一行\n第二行\n第三行"));
        QVERIFY(editor->height() > initialHeight * 2);
        QCOMPARE(editor->width(), 240);
        const QString text = QString(100, 'W') + '\n' + QStringLiteral("多行原像素\n").repeated(30);
        editor->setPlainText(text);
        QCOMPARE(editor->height(), 240);
        QCOMPARE(editor->width(), 400);
        QCOMPARE(editor->pos(), anchor);
        QVERIFY(surface->rect().contains(editor->geometry()));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(!editor->isVisible());
        const auto annotations = [&]() -> const QVector<Annotation>&
        { return stickerEditor ? sticker->annotations() : overlay.annotations(); };
        QCOMPARE(annotations().size(), qsizetype(1));
        QCOMPARE(annotations().first().text, text);
        const qreal physicalFactor =
            stickerEditor ? sticker->windowHandle()->screen()->devicePixelRatio() / sticker->scale()
                          : factor;
        QCOMPARE(annotations().first().first, QPointF(anchor) * physicalFactor);
        QCOMPARE(annotations().first().style.color, annotationColors()[5]);
        QCOMPARE(annotations().first().style.textSize, 28);
        Annotation expected{
            AnnotationType::Text, {}, QPointF(anchor) * physicalFactor, {}, {}, text};
        expected.style.color = annotationColors()[5];
        const QImage source = stickerEditor ? sticker->image() : frame.pixels;
        const QRect crop = stickerEditor ? source.rect() : overlay.selection();
        QImage preview = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        preview.setDevicePixelRatio(1);
        {
            QPainter painter(&preview);
            paintAnnotations(painter, annotations(), source);
        }
        const QImage expectedImage = renderAnnotatedSelection(source, {expected}, crop);
        QCOMPARE(preview.copy(crop), expectedImage);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("透明多行.png"));
        QVERIFY(stickerEditor ? sticker->exportToPath(path) : overlay.exportToPath(path));
        QCOMPARE(QImage(path).convertToFormat(expectedImage.format()), expectedImage);

        // 组合期间扩宽、临时失焦和完成快捷键不能结束标注或丢失候选。
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, {100, 100});
        editor->setPlainText(QStringLiteral("原文"));
        editor->moveCursor(QTextCursor::End);
        QInputMethodEvent preedit(QStringLiteral("中文候选").repeated(50), {});
        QApplication::sendEvent(editor, &preedit);
        QVERIFY(editor->isComposing());
        QCOMPARE(editor->width(), 400);
        for (auto reason : {Qt::PopupFocusReason, Qt::ActiveWindowFocusReason})
        {
            QFocusEvent temporary(QEvent::FocusOut, reason);
            QApplication::sendEvent(editor, &temporary);
            QVERIFY(editor->isVisible() && editor->isComposing());
            QCOMPARE(annotations().size(), qsizetype(1));
        }
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(editor->isVisible() && editor->isComposing());
        QInputMethodEvent converted;
        converted.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(editor, &converted);
        QTest::keyClick(editor, Qt::Key_Return);
        editor->insertPlainText(QStringLiteral("下一行"));
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, {450, 350});
        QCOMPARE(annotations().size(), qsizetype(2));
        QCOMPARE(annotations().last().text, QStringLiteral("原文中文\n下一行"));
        QVERIFY(!editor->isComposing());
        editor->setPlainText(QStringLiteral("取消草稿"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!editor->isVisible());
        QCOMPARE(annotations().size(), qsizetype(2));
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, {100, 100});
        editor->setPlainText(QStringLiteral("工具切换提交"));
        clickAnnotationTool(*surface,
                            stickerEditor ? "stickerEditFreehandToolButton" : "freehandToolButton");
        QCOMPARE(annotations().size(), qsizetype(3));
        QVERIFY(!editor->isVisible());
        clickAnnotationTool(*surface,
                            stickerEditor ? "stickerEditTextToolButton" : "textToolButton");
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier,
                          surface->rect().bottomRight() - QPoint(1, 1));
        QVERIFY(surface->rect().contains(editor->geometry()));
        const QPoint edge = editor->pos();
        editor->setPlainText(QStringLiteral("边缘夹取"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(annotations().last().first, QPointF(edge) * physicalFactor);
        QCOMPARE(finished.count(), 0);
    }
    void textImeAndEscapeStayInCurrentLayer()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 140});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        QInputMethodEvent preedit(QStringLiteral("jie tu"), {});
        QApplication::sendEvent(editor, &preedit);
        QVERIFY(editor->isComposing());
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(editor->isVisible());
        QVERIFY(editor->toPlainText().isEmpty());
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.annotations().isEmpty());
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(editor->isVisible());
        QVERIFY(!editor->isComposing());
        QCOMPARE(finished.count(), 0);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("截图说明 ABC 123"));
        QApplication::sendEvent(editor, &commit);
        QTest::keyClick(editor, Qt::Key_Return);
        QInputMethodEvent second;
        second.setCommitString(QStringLiteral("第二行"));
        QApplication::sendEvent(editor, &second);
        QVERIFY(editor->toPlainText().contains(QStringLiteral("截图说明 ABC 123\n第二行")));
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!editor->isVisible());
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.annotations().isEmpty());
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 2);
    }
    void textCommitsBeforeSaveCopyAndOutsideClick()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath(QStringLiteral("文本.png"));
        QImage copied;
        OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&) { return path; };
        const auto frame = annotationFrame();
        SelectionOverlay overlay(frame, actions);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 120});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        editor->setPlainText(QStringLiteral("截图说明 ABC 123\n第二行"));
        QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QCOMPARE(overlay.annotations().last().text, QStringLiteral("截图说明 ABC 123\n第二行"));
        const QImage saved(path);
        QCOMPARE(saved,
                 renderAnnotatedSelection(frame.pixels, overlay.annotations(), overlay.selection())
                     .convertToFormat(saved.format()));
        QVERIFY(
            saved !=
            cropFrozenSelection(frame.pixels, overlay.selection()).convertToFormat(saved.format()));
        QVERIFY(overlay.isSelectionSaved());
        QVERIFY(overlay.isVisible());
        QCOMPARE(finished.count(), 0);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {130, 180});
        editor->setPlainText(QStringLiteral("第三条"));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {150, 200});
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(!overlay.isSelectionSaved());
        // 空编辑切换工具时不产生额外步骤。
        clickAnnotationTool(overlay, "lineToolButton");
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        drag(overlay, {120, 160}, {280, 160});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {160, 200});
        editor->setPlainText(QStringLiteral("复制前确认"));
        auto expectedAnnotations = overlay.annotations();
        Annotation expected{
            AnnotationType::Text,        expectedAnnotations.first().style, {320, 400}, {}, {},
            QStringLiteral("复制前确认")};
        expectedAnnotations.append(expected);
        const auto expectedImage =
            renderAnnotatedSelection(frame.pixels, expectedAnnotations, overlay.selection());
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        QCOMPARE(copied, expectedImage);
        QCOMPARE(copied.devicePixelRatio(), qreal(1));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 10);
        QVERIFY(overlay.annotations().isEmpty());
        QCOMPARE(QImage(path), saved);
    }
    void toolbarBackgroundCommitsAndClearsPreedit()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "textToolButton");
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 120});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->setPlainText(QStringLiteral("已确认文字"));
        QInputMethodEvent preedit(QStringLiteral("jie tu"), {});
        QApplication::sendEvent(editor, &preedit);
        QVERIFY(editor->isComposing());
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        QTest::mouseClick(toolbar, Qt::LeftButton, Qt::NoModifier, {2, 2});
        QVERIFY(!editor->isVisible());
        QVERIFY(!editor->isComposing());
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
        QCOMPARE(overlay.annotations().first().text, QStringLiteral("已确认文字"));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {150, 180});
        editor->setPlainText(QStringLiteral("下次输入"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QCOMPARE(overlay.annotations().last().text, QStringLiteral("下次输入"));
        QCOMPARE(finished.count(), 0);
    }
    void annotationSaveCopyHasPhysicalPixelsOnly()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.filePath(QStringLiteral("物理像素.png"));
        QImage copied;
        OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.chooseSavePath = [&](QWidget*, const QString&) { return path; };
        SelectionOverlay overlay(annotationFrame(), actions);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        overlay.show();
        drag(overlay, {100, 100}, {300, 250});
        clickAnnotationTool(overlay, "lineToolButton");
        drag(overlay, {90, 150}, {310, 150});
        QCOMPARE(overlay.selection(), QRect(200, 200, 400, 300));
        QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
        const QImage saved(path);
        QCOMPARE(saved.size(), QSize(400, 300));
        QCOMPARE(saved.devicePixelRatio(), qreal(1));
        for (int y = 0; y < saved.height(); ++y)
            for (int x = 0; x < saved.width(); ++x)
                QCOMPARE(saved.pixelColor(x, y),
                         y >= 98 && y <= 101 ? annotationColors()[0] : QColor(Qt::white));
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.isSelectionSaved());
        QTest::mouseClick(button(overlay, "undoAnnotationButton"), Qt::LeftButton);
        QVERIFY(!overlay.isSelectionSaved());
        QTest::mouseClick(button(overlay, "redoAnnotationButton"), Qt::LeftButton);
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        QCOMPARE(copied.convertToFormat(saved.format()), saved);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 10);
        QVERIFY(overlay.annotations().isEmpty());
    }
    void quickSaveDirectoryCollisionAndFormat_data()
    {
        QTest::addColumn<bool>("sticker");
        QTest::addColumn<bool>("jpeg");
        QTest::newRow("overlay-png") << false << false;
        QTest::newRow("overlay-jpeg") << false << true;
        QTest::newRow("sticker-png") << true << false;
        QTest::newRow("sticker-jpeg") << true << true;
    }
    void quickSaveDirectoryCollisionAndFormat()
    {
        using namespace waibusnap;
        QFETCH(bool, sticker);
        QFETCH(bool, jpeg);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("中文 快速保存"));
        QVERIFY(QDir().mkpath(directory));
        const auto format = jpeg ? ImageFormat::Jpeg : ImageFormat::Png;
        const QDateTime timestamp(QDate(2026, 10, 9), QTime(12, 0, 0, 123));
        const QString originalPath = suggestedImagePath(directory, format, timestamp);
        QFile original(originalPath);
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write("original"), qint64(8));
        original.close();
        int panels = 0, reads = 0;
        ImageSaveActions saveActions;
        saveActions.loadSavePreferences = [&]
        {
            ++reads;
            return SavePreferences{directory, format};
        };
        saveActions.saveTimestamp = [=] { return timestamp; };
        saveActions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++panels;
            return QString();
        };
        QImage expected;
        QString statusPath;
        if (sticker)
        {
            StickerActions actions;
            static_cast<ImageSaveActions&>(actions) = saveActions;
            StickerManager manager(nullptr, actions);
            QVERIFY(manager.create(sampleFrame().pixels, {30, 40}).success);
            const auto window = manager.windows().first();
            expected = window->renderedImage();
            QVERIFY(window->saveImage());
            QVERIFY(window->saveImage());
            QVERIFY(window->isSaved());
            QCOMPARE(manager.count(), 1);
            auto* status = window->findChild<QLabel*>(QStringLiteral("stickerStatus"));
            QVERIFY(status && status->isVisible());
            statusPath = QDir(directory).filePath(
                QFileInfo(originalPath).completeBaseName() +
                (jpeg ? QStringLiteral("_3.jpg") : QStringLiteral("_3.png")));
            QCOMPARE(status->text(), QStringLiteral("已保存：%1").arg(statusPath));
            QCOMPARE(status->toolTip(), status->text());
        }
        else
        {
            OverlayActions actions;
            static_cast<ImageSaveActions&>(actions) = saveActions;
            SelectionOverlay overlay(sampleFrame(), actions);
            QSignalSpy finished(&overlay, &SelectionOverlay::finished);
            overlay.show();
            drag(overlay, {20, 10}, {80, 60});
            expected = renderAnnotatedSelection(sampleFrame().pixels, {}, overlay.selection());
            QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
            QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
            QVERIFY(overlay.isSelectionSaved());
            QVERIFY(overlay.isVisible());
            QCOMPARE(finished.count(), 0);
            auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
            QVERIFY(status && status->isVisible());
            statusPath = QDir(directory).filePath(
                QFileInfo(originalPath).completeBaseName() +
                (jpeg ? QStringLiteral("_3.jpg") : QStringLiteral("_3.png")));
            QCOMPARE(status->text(), QStringLiteral("已保存：%1").arg(statusPath));
            QCOMPARE(status->toolTip(), status->text());
        }
        QCOMPARE(reads, 2);
        QCOMPARE(panels, 0);
        QCOMPARE(QDir(directory).entryList(QDir::Files | QDir::Hidden).size(), 3);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original"));
        const QStringList files = QDir(directory).entryList(QDir::Files);
        for (const auto& name : files)
        {
            const QString path = QDir(directory).filePath(name);
            if (path == originalPath)
                continue;
            QImageReader reader(path);
            QCOMPARE(reader.format(), jpeg ? QByteArray("jpeg") : QByteArray("png"));
            QCOMPARE(reader.size(), expected.size());
            const QImage loaded = reader.read();
            QCOMPARE(loaded.devicePixelRatio(), qreal(1));
            if (!jpeg)
                QCOMPARE(loaded, expected.convertToFormat(loaded.format()));
        }
    }
    void quickDirectoryAvailabilityAndCancelKeepsImage_data()
    {
        QTest::addColumn<bool>("sticker");
        QTest::addColumn<int>("kind");
        for (bool sticker : {false, true})
            for (int kind : {0, 1, 2, 3, 4})
                QTest::newRow(qPrintable(
                    QStringLiteral("%1-%2").arg(sticker ? "sticker" : "overlay").arg(kind)))
                    << sticker << kind;
    }
    void quickDirectoryAvailabilityAndCancelKeepsImage()
    {
        using namespace waibusnap;
        QFETCH(bool, sticker);
        QFETCH(int, kind);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QString directory = temporary.filePath(QStringLiteral("失效目录"));
        if (kind == 1)
        {
            QFile file(directory);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("original");
        }
        bool directlyWritable = false;
        if (kind == 2 || kind == 4)
        {
            QVERIFY(QDir().mkpath(directory));
            if (kind == 2)
                QVERIFY(QFile::setPermissions(directory,
                                              QFileDevice::ReadOwner | QFileDevice::ExeOwner));
            // Windows 的目录只读属性不限制创建文件，不能冒充拒绝写入的夹具。
            // 用真实文件探针建立预期，避免照抄保存决策的 QFileInfo 判断。
            QTemporaryFile probe(QDir(directory).filePath(QStringLiteral("权限探针_XXXXXX")));
            directlyWritable = probe.open();
            if (kind == 4)
                QVERIFY(directlyWritable);
        }
        if (kind == 3)
            directory.clear();
        int panels = 0;
        ImageSaveActions saveActions;
        saveActions.loadSavePreferences = [=]
        { return SavePreferences{directory, ImageFormat::Jpeg}; };
        saveActions.chooseSavePath = [&](QWidget*, const QString& suggestion)
        {
            ++panels;
            if (!suggestion.endsWith(QStringLiteral(".jpg")))
                return temporary.filePath(QStringLiteral("错误.png"));
            return QString();
        };
        if (sticker)
        {
            StickerActions actions;
            static_cast<ImageSaveActions&>(actions) = saveActions;
            StickerManager manager(nullptr, actions);
            QVERIFY(manager.create(sampleFrame().pixels, {30, 40}).success);
            const auto window = manager.windows().first();
            const QImage image = window->image();
            QCOMPARE(window->saveImage(), directlyWritable);
            QCOMPARE(window->isSaved(), directlyWritable);
            QVERIFY(window->isVisible());
            QCOMPARE(window->image(), image);
            QCOMPARE(manager.count(), 1);
        }
        else
        {
            OverlayActions actions;
            static_cast<ImageSaveActions&>(actions) = saveActions;
            SelectionOverlay overlay(sampleFrame(), actions);
            QSignalSpy finished(&overlay, &SelectionOverlay::finished);
            overlay.show();
            drag(overlay, {20, 10}, {80, 60});
            const QRect selection = overlay.selection();
            QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
            QCOMPARE(overlay.isSelectionSaved(), directlyWritable);
            QVERIFY(overlay.isVisible());
            QCOMPARE(overlay.selection(), selection);
            QCOMPARE(finished.count(), 0);
        }
        if (kind == 2)
            QVERIFY(QFile::setPermissions(directory, QFileDevice::ReadOwner |
                                                         QFileDevice::WriteOwner |
                                                         QFileDevice::ExeOwner));
        QCOMPARE(panels, directlyWritable ? 0 : 1);
        if (directlyWritable)
        {
            const QStringList files = QDir(directory).entryList(QDir::Files | QDir::Hidden);
            QCOMPARE(files.size(), 1);
            QImageReader reader(QDir(directory).filePath(files.first()));
            QCOMPARE(reader.format(), QByteArray("jpeg"));
            QCOMPARE(reader.size(), sticker ? sampleFrame().pixels.size() : QSize(120, 100));
        }
        QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("错误.png"))));
    }
    void quitSaveAllUsesQuickDirectoryWithoutCollisions_data()
    {
        QTest::addColumn<bool>("jpeg");
        QTest::newRow("png") << false;
        QTest::newRow("jpeg") << true;
    }
    void quitSaveAllUsesQuickDirectoryWithoutCollisions()
    {
        using namespace waibusnap;
        QFETCH(bool, jpeg);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto format = jpeg ? ImageFormat::Jpeg : ImageFormat::Png;
        const QDateTime timestamp(QDate(2026, 10, 9), QTime(12, 0));
        int panels = 0, reads = 0, confirms = 0;
        StickerActions actions;
        actions.loadSavePreferences = [&]
        {
            ++reads;
            return SavePreferences{temporary.path(), format};
        };
        actions.saveTimestamp = [=] { return timestamp; };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++panels;
            return QString();
        };
        actions.confirmQuit = [&](int count)
        {
            ++confirms;
            if (count != 3)
                return StickerQuitDecision::Cancel;
            return StickerQuitDecision::SaveAll;
        };
        StickerManager manager(nullptr, actions);
        QImage image(120, 80, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int i = 0; i < 3; ++i)
            QVERIFY(manager.create(image, {30 + i * 20, 40}).success);
        manager.hideAll();
        QVERIFY(manager.resolveUnsavedForQuit());
        QCOMPARE(panels, 0);
        QCOMPARE(reads, 3);
        QCOMPARE(confirms, 1);
        const QStringList files = QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden);
        QCOMPARE(files.size(), 3);
        for (const auto& name : files)
        {
            QImageReader reader(temporary.filePath(name));
            QCOMPARE(reader.format(), jpeg ? QByteArray("jpeg") : QByteArray("png"));
            QCOMPARE(reader.size(), image.size());
        }
        for (const auto& window : manager.windows())
            QVERIFY(window->isSaved());
    }
    void savePanelFormatsNormalizeSuffixAndDispatch()
    {
        using namespace waibusnap;
        QCOMPARE(savePanelFilePath(QStringLiteral("图.jpg"), ImageFormat::Png),
                 QStringLiteral("图.png"));
        QCOMPARE(savePanelFilePath(QStringLiteral("图.png"), ImageFormat::Jpeg),
                 QStringLiteral("图.jpg"));
        QCOMPARE(savePanelFilePath(QStringLiteral("图.jpeg"), ImageFormat::Jpeg),
                 QStringLiteral("图.jpeg"));
        QCOMPARE(savePanelFilePath(QStringLiteral("图.JPG"), ImageFormat::Jpeg),
                 QStringLiteral("图.JPG"));
        QCOMPARE(savePanelFilePath(QStringLiteral("图"), ImageFormat::Jpeg),
                 QStringLiteral("图.jpg"));
        QCOMPARE(savePanelFilePath(QStringLiteral("图.txt"), ImageFormat::Png),
                 QStringLiteral("图.png"));
        QVERIFY(savePanelFilePath({}, ImageFormat::Jpeg).isEmpty());
        QVERIFY(imageSaveFilters().contains(QStringLiteral("有损")));
        QVERIFY(imageSaveFilters().contains(QStringLiteral("*.jpg *.jpeg")));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QString path = temporary.filePath(QStringLiteral("选择.jpeg"));
        int panels = 0;
        OverlayActions actions;
        actions.chooseSavePath = [&](QWidget*, const QString& suggestion)
        {
            ++panels;
            if (panels == 1 && !suggestion.endsWith(QStringLiteral(".png")))
                return QString();
            return path;
        };
        SelectionOverlay overlay(sampleFrame(), actions);
        overlay.show();
        drag(overlay, {20, 10}, {80, 60});
        QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
        QImageReader jpeg(path);
        QCOMPARE(jpeg.format(), QByteArray("jpeg"));
        QCOMPARE(jpeg.size(), QSize(120, 100));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".png")));
        path = temporary.filePath(QStringLiteral("其他.txt"));
        QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
        QImageReader png(path + QStringLiteral(".png"));
        QCOMPARE(png.format(), QByteArray("png"));
        QCOMPARE(panels, 2);
    }
    void quickSaveWriteFailureDoesNotSwitchToPanel()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("保存中失效"));
        QVERIFY(QDir().mkpath(directory));
        int panels = 0;
        ImageSaveActions actions;
        actions.loadSavePreferences = [=] { return SavePreferences{directory, ImageFormat::Jpeg}; };
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++panels;
            return QString();
        };
        const auto target =
            chooseImageSaveTarget(nullptr, actions, [] { return true; }, [](const QString&) {});
        QCOMPARE(target.quickDirectory, directory);
        QVERIFY(QDir().rmdir(directory));
        const QImage image = sampleFrame().pixels;
        const auto output = saveImageToTarget(image, target);
        QVERIFY(!output.result.success);
        QVERIFY(!output.result.explanation.isEmpty());
        QVERIFY(output.path.isEmpty());
        QCOMPARE(image, sampleFrame().pixels);
        QCOMPARE(panels, 0);
        QVERIFY(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }
    void autostartToggleAndReopen_data()
    {
        QTest::addColumn<bool>("initial");
        QTest::newRow("enable") << false;
        QTest::newRow("disable") << true;
    }
    void autostartToggleAndReopen()
    {
        using namespace waibusnap;
        QFETCH(bool, initial);
        AutostartStub stub;
        stub.state.enabled = initial;
        int hotkeys = 0;
        SettingsDialog dialog(
            defaultScreenshotHotkey(),
            [&](const QKeySequence&)
            {
                ++hotkeys;
                return QString();
            },
            {}, nullptr, {}, stub.actions());
        dialog.show();
        auto* check = dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"));
        auto* help = dialog.findChild<QLabel*>(QStringLiteral("autostartHelp"));
        QVERIFY(check && help);
        QCOMPARE(check->text(), QStringLiteral("登录时自动启动"));
        QCOMPARE(check->isChecked(), initial);
        QVERIFY(help->text().contains(QStringLiteral("默认关闭")));
        QCOMPARE(stub.queries, 1);
        QVERIFY(stub.requests.isEmpty());
        check->setChecked(!initial);
        QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                          Qt::LeftButton);
        QCOMPARE(stub.requests, QVector<bool>{!initial});
        QCOMPARE(stub.state.enabled, !initial);
        QCOMPARE(stub.queries, 2);
        QCOMPARE(hotkeys, 0);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
        SettingsDialog reopened(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            {}, stub.actions());
        QCOMPARE(reopened.findChild<QCheckBox*>(QStringLiteral("autostartCheck"))->isChecked(),
                 !initial);
        QCOMPARE(stub.queries, 3);
        reopened.reject();
        QCOMPARE(stub.requests.size(), 1);
    }
    void autostartFailureKeepsStateAndRetries_data() { autostartToggleAndReopen_data(); }
    void autostartFailureKeepsStateAndRetries()
    {
        using namespace waibusnap;
        QFETCH(bool, initial);
        AutostartStub stub;
        stub.state.enabled = initial;
        stub.error = QStringLiteral("测试系统拒绝登录启动项，错误码 42");
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            {}, stub.actions());
        dialog.show();
        auto* check = dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"));
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"));
        check->setChecked(!initial);
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"))->text(), stub.error);
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"))->isVisible());
        QCOMPARE(stub.state.enabled, initial);
        QCOMPARE(check->isChecked(), !initial);
        QCOMPARE(stub.requests, QVector<bool>{!initial});
        stub.error.clear();
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(stub.requests, (QVector<bool>{!initial, !initial}));
        QCOMPARE(stub.state.enabled, !initial);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
    }
    void autostartUnchangedCancelAndEscape_data()
    {
        QTest::addColumn<bool>("initial");
        QTest::addColumn<int>("decision");
        QTest::newRow("unchanged-off") << false << 0;
        QTest::newRow("unchanged-on") << true << 0;
        QTest::newRow("cancel-enabling") << false << 1;
        QTest::newRow("cancel-disabling") << true << 1;
        QTest::newRow("escape-editor") << false << 2;
        QTest::newRow("escape-checkbox") << true << 3;
    }
    void autostartUnchangedCancelAndEscape()
    {
        using namespace waibusnap;
        QFETCH(bool, initial);
        QFETCH(int, decision);
        AutostartStub stub;
        stub.state.enabled = initial;
        int hotkeys = 0, preferences = 0;
        SaveSettingsActions saves;
        saves.savePreferences = [&](const SavePreferences&)
        {
            ++preferences;
            return QString();
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(),
            [&](const QKeySequence&)
            {
                ++hotkeys;
                return QString();
            },
            {}, nullptr, saves, stub.actions());
        dialog.show();
        auto* check = dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"));
        auto* editor = dialog.findChild<QKeySequenceEdit*>(QStringLiteral("hotkeyEditor"));
        if (decision != 0)
        {
            check->setChecked(!initial);
            editor->setKeySequence(QKeySequence(Qt::Key_F2));
            dialog.findChild<QComboBox*>(QStringLiteral("saveFormat"))->setCurrentIndex(1);
        }
        if (decision == 0)
            QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                              Qt::LeftButton);
        else if (decision == 1)
            QTest::mouseClick(
                dialog.findChild<QPushButton*>(QStringLiteral("cancelSettingsButton")),
                Qt::LeftButton);
        else
            QTest::keyClick(decision == 2 ? static_cast<QWidget*>(editor) : check, Qt::Key_Escape);
        QVERIFY(stub.requests.isEmpty());
        QCOMPARE(stub.state.enabled, initial);
        QCOMPARE(stub.queries, decision == 0 ? 2 : 1);
        QCOMPARE(hotkeys, 0);
        QCOMPARE(preferences, decision == 0 ? 1 : 0);
        QCOMPARE(dialog.result(), decision == 0 ? int(QDialog::Accepted) : int(QDialog::Rejected));
        QVERIFY(!dialog.isVisible());
    }
    void autostartSaveQueriesCurrentSystemState_data()
    {
        QTest::addColumn<bool>("externalEnabled");
        QTest::newRow("externally-enabled-no-write") << true;
        QTest::newRow("externally-disabled-needs-write") << false;
    }
    void autostartSaveQueriesCurrentSystemState()
    {
        using namespace waibusnap;
        QFETCH(bool, externalEnabled);
        AutostartStub stub;
        stub.state.enabled = !externalEnabled;
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            {}, stub.actions());
        dialog.show();
        dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"))->setChecked(true);
        stub.state.enabled = externalEnabled;
        QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                          Qt::LeftButton);
        QCOMPARE(stub.requests.size(), externalEnabled ? 0 : 1);
        QCOMPARE(stub.queries, 2);
        QVERIFY(stub.state.enabled);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void autostartApprovalAndNotice_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("pending");
        QTest::addColumn<QString>("notice");
        QTest::newRow("approval") << true << true << QString();
        QTest::newRow("notice-over-approval") << true << true << QStringLiteral("系统提示：<拒绝>");
        QTest::newRow("unavailable") << false << false << QStringLiteral("测试登录项服务不可用");
        QTest::newRow("enabled-notice") << true << false << QStringLiteral("测试路径需要重新确认");
    }
    void autostartApprovalAndNotice()
    {
        using namespace waibusnap;
        QFETCH(bool, enabled);
        QFETCH(bool, pending);
        QFETCH(QString, notice);
        AutostartStub stub;
        stub.state = {enabled, pending, notice};
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            {}, stub.actions());
        dialog.show();
        QCOMPARE(dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"))->isChecked(),
                 enabled);
        auto* help = dialog.findChild<QLabel*>(QStringLiteral("autostartHelp"));
        QVERIFY(help->isVisible());
        QCOMPARE(help->textFormat(), Qt::PlainText);
        if (!notice.isEmpty())
            QCOMPARE(help->text(), notice);
        else
        {
            QVERIFY(help->text().contains(QStringLiteral("等待批准")));
            QVERIFY(help->text().contains(QStringLiteral("登录项")));
        }
        QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                          Qt::LeftButton);
        QVERIFY(stub.requests.isEmpty());
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void autostartCombinedSaveOrderAndFailures_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("all-success") << 0;
        QTest::newRow("hotkey-failure") << 1;
        QTest::newRow("preferences-failure") << 2;
        QTest::newRow("autostart-failure") << 3;
    }
    void autostartCombinedSaveOrderAndFailures()
    {
        using namespace waibusnap;
        QFETCH(int, failure);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
        AutostartStub stub;
        const auto stubActions = stub.actions();
        QStringList order;
        int attempt = 1;
        SaveSettingsActions saves;
        saves.savePreferences = [&](const SavePreferences& preferences)
        {
            order.append(QStringLiteral("偏好"));
            return failure == 2 && attempt == 1 ? QStringLiteral("测试偏好错误")
                                                : settings.saveSavePreferences(preferences);
        };
        AutostartActions autostart = stubActions;
        autostart.setEnabled = [&](bool enabled)
        {
            order.append(QStringLiteral("启动"));
            stub.error = failure == 3 && attempt == 1 ? QStringLiteral("测试启动错误") : QString();
            return stubActions.setEnabled(enabled);
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(),
            [&](const QKeySequence& sequence)
            {
                order.append(QStringLiteral("热键"));
                return failure == 1 && attempt == 1 ? QStringLiteral("测试热键错误")
                                                    : settings.saveHotkey(sequence);
            },
            {}, nullptr, saves, autostart);
        dialog.show();
        dialog.findChild<QKeySequenceEdit*>(QStringLiteral("hotkeyEditor"))
            ->setKeySequence(QKeySequence(Qt::Key_F2));
        dialog.findChild<QComboBox*>(QStringLiteral("saveFormat"))->setCurrentIndex(1);
        dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"))->setChecked(true);
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"));
        QTest::mouseClick(save, Qt::LeftButton);
        const QStringList all = {QStringLiteral("热键"), QStringLiteral("偏好"),
                                 QStringLiteral("启动")};
        QCOMPARE(order, all.mid(0, failure == 0 ? 3 : failure));
        if (failure != 0)
        {
            QVERIFY(dialog.isVisible());
            QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"))->text(),
                     QStringLiteral("测试%1错误").arg(all.at(failure - 1)));
            QVERIFY(!stub.state.enabled);
            QCOMPARE(settings.loadHotkey().sequence,
                     failure == 1 ? defaultScreenshotHotkey() : QKeySequence(Qt::Key_F2));
            QCOMPARE(settings.loadSavePreferences().format,
                     failure <= 2 ? ImageFormat::Png : ImageFormat::Jpeg);
            attempt = 2;
            order.clear();
            QTest::mouseClick(save, Qt::LeftButton);
            // 前次热键成功后不重复注册；保存偏好可重复写，启动失败保留目标并重试。
            QCOMPARE(order, all.mid(failure == 1 ? 0 : 1));
        }
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
        QVERIFY(stub.state.enabled);
        QCOMPARE(stub.requests.size(), failure == 3 ? 2 : 1);
        QCOMPARE(settings.loadHotkey().sequence, QKeySequence(Qt::Key_F2));
        QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Jpeg);
        QFile file(temporary.filePath(QStringLiteral("settings.ini")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(!bytes.contains("autostart"));
        QVERIFY(!bytes.contains("startup"));
    }
    void controllerAutostartMenuSaveReopenAndCancel()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RunOptions options;
        options.settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        AutostartStub stub;
        ApplicationController controller(*qApp, options, {}, {}, stub.actions());
        QCOMPARE(stub.queries, 0);
        int handled = 0;
        QTimer::singleShot(
            0,
            [&]
            {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto* check = dialog->findChild<QCheckBox*>(QStringLiteral("autostartCheck"));
                QVERIFY(!check->isChecked());
                check->setChecked(true);
                QTest::mouseClick(
                    dialog->findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                    Qt::LeftButton);
                ++handled;
            });
        controller.menu()->actions().at(3)->trigger();
        QCOMPARE(handled, 1);
        QCOMPARE(stub.requests, QVector<bool>{true});
        QVERIFY(stub.state.enabled);
        QTimer::singleShot(
            0,
            [&]
            {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto* check = dialog->findChild<QCheckBox*>(QStringLiteral("autostartCheck"));
                QVERIFY(check->isChecked());
                check->setChecked(false);
                QTest::mouseClick(
                    dialog->findChild<QPushButton*>(QStringLiteral("cancelSettingsButton")),
                    Qt::LeftButton);
                ++handled;
            });
        controller.menu()->actions().at(3)->trigger();
        QCOMPARE(handled, 2);
        QCOMPARE(stub.requests, QVector<bool>{true});
        QCOMPARE(stub.queries, 3);
        QVERIFY(stub.state.enabled);
    }
    void saveSettingsWriteFailureKeepsDialogAndCanRetry()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        int writes = 0;
        AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
        SaveSettingsActions actions;
        actions.preferences = {temporary.path(), ImageFormat::Jpeg};
        actions.savePreferences = [&](const SavePreferences& preferences)
        {
            ++writes;
            return writes == 1 ? QStringLiteral("测试设置写入失败")
                               : settings.saveSavePreferences(preferences);
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, {}, nullptr,
            actions);
        dialog.show();
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"));
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(dialog.isVisible());
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"))
                    ->text()
                    .contains(QStringLiteral("写入失败")));
        QVERIFY(settings.loadSavePreferences().quickDirectory.isEmpty());
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(writes, 2);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(settings.loadSavePreferences().quickDirectory, temporary.path());
        QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Jpeg);
    }
    void saveSettingsEditCancelAndDirectoryInjection_data()
    {
        QTest::addColumn<int>("decision");
        QTest::newRow("save") << 0;
        QTest::newRow("cancel") << 1;
        QTest::newRow("escape") << 2;
    }
    void saveSettingsEditCancelAndDirectoryInjection()
    {
        using namespace waibusnap;
        QFETCH(int, decision);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
        int choices = 0, writes = 0, registrations = 0;
        SaveSettingsActions actions;
        actions.savePreferences = [&](const SavePreferences& preferences)
        {
            ++writes;
            return settings.saveSavePreferences(preferences);
        };
        actions.chooseDirectory = [&](QWidget* parent, const QString& current)
        {
            ++choices;
            if (!parent || !current.isEmpty())
                return QString();
            return choices == 1 ? QString() : temporary.path();
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(),
            [&](const QKeySequence&)
            {
                ++registrations;
                return QString();
            },
            {}, nullptr, actions);
        dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(1);
        dialog.show();
        auto* directory = dialog.findChild<QLineEdit*>(QStringLiteral("quickDirectory"));
        auto* format = dialog.findChild<QComboBox*>(QStringLiteral("saveFormat"));
        auto* notice = dialog.findChild<QLabel*>(QStringLiteral("jpegLossNotice"));
        auto* choose = dialog.findChild<QPushButton*>(QStringLiteral("chooseDirectoryButton"));
        QVERIFY(directory && format && notice && choose);
        QVERIFY(directory->isReadOnly());
        QVERIFY(directory->text().isEmpty());
        QCOMPARE(format->currentText(), QStringLiteral("PNG"));
        QVERIFY(!notice->isVisible());
        QTest::mouseClick(choose, Qt::LeftButton);
        QVERIFY(directory->text().isEmpty());
        QTest::mouseClick(choose, Qt::LeftButton);
        QCOMPARE(directory->text(), temporary.path());
        QCOMPARE(directory->toolTip(), temporary.path());
        format->setCurrentIndex(1);
        QVERIFY(notice->isVisible());
        QVERIFY(notice->text().contains(QStringLiteral("有损")));
        if (decision == 0)
            QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                              Qt::LeftButton);
        else if (decision == 1)
            QTest::mouseClick(
                dialog.findChild<QPushButton*>(QStringLiteral("cancelSettingsButton")),
                Qt::LeftButton);
        else
            QTest::keyClick(directory, Qt::Key_Escape);
        QCOMPARE(choices, 2);
        QCOMPARE(writes, decision == 0 ? 1 : 0);
        QCOMPARE(registrations, 0);
        QCOMPARE(dialog.result(), decision == 0 ? int(QDialog::Accepted) : int(QDialog::Rejected));
        QCOMPARE(settings.loadSavePreferences().quickDirectory,
                 decision == 0 ? temporary.path() : QString());
        QCOMPARE(settings.loadSavePreferences().format,
                 decision == 0 ? ImageFormat::Jpeg : ImageFormat::Png);
    }
    void controllerSavePreferencesApplyImmediatelyAndClearDirectory()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RunOptions options;
        options.settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        const QString directory = temporary.filePath(QStringLiteral("快速目录"));
        QVERIFY(QDir().mkpath(directory));
        int panels = 0;
        StickerActions actions;
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            ++panels;
            return QString();
        };
        ApplicationController controller(*qApp, options, actions);
        QVERIFY(controller.stickers().create(sampleFrame().pixels, {30, 40}).success);
        const auto window = controller.stickers().windows().first();
        QVERIFY(!window->saveImage());
        QCOMPARE(panels, 1);
        bool handled = false;
        QTimer::singleShot(
            0,
            [&]
            {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                dialog->findChild<QLineEdit*>(QStringLiteral("quickDirectory"))->setText(directory);
                dialog->findChild<QComboBox*>(QStringLiteral("saveFormat"))->setCurrentIndex(1);
                QTest::mouseClick(
                    dialog->findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                    Qt::LeftButton);
                handled = true;
            });
        controller.menu()->actions().at(3)->trigger();
        QVERIFY(handled);
        QVERIFY(window->saveImage());
        QCOMPARE(panels, 1);
        const auto files = QDir(directory).entryList(QDir::Files);
        QCOMPARE(files.size(), 1);
        QImageReader reader(QDir(directory).filePath(files.first()));
        QCOMPARE(reader.format(), QByteArray("jpeg"));
        QCOMPARE(reader.size(), sampleFrame().pixels.size());
        handled = false;
        QTimer::singleShot(
            0,
            [&]
            {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("quickDirectory"))->text(),
                         directory);
                dialog->findChild<QTabWidget*>(QStringLiteral("settingsTabs"))->setCurrentIndex(1);
                QTest::mouseClick(
                    dialog->findChild<QPushButton*>(QStringLiteral("clearDirectoryButton")),
                    Qt::LeftButton);
                QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("quickDirectory"))
                            ->text()
                            .isEmpty());
                QVERIFY(dialog->findChild<QLineEdit*>(QStringLiteral("quickDirectory"))
                            ->toolTip()
                            .isEmpty());
                QTest::mouseClick(
                    dialog->findChild<QPushButton*>(QStringLiteral("saveSettingsButton")),
                    Qt::LeftButton);
                handled = true;
            });
        controller.menu()->actions().at(3)->trigger();
        QVERIFY(handled);
        QVERIFY(!window->saveImage());
        QCOMPARE(panels, 2);
        QVERIFY(window->isSaved());
        QVERIFY(AppSettings(options.settingsFile).loadSavePreferences().quickDirectory.isEmpty());
    }
    void settingsTabsReachability_data()
    {
        QTest::addColumn<bool>("longNotice");
        QTest::newRow("normal") << false;
        QTest::newRow("long-notice") << true;
    }
    void settingsTabsReachability()
    {
        using namespace waibusnap;
        QFETCH(bool, longNotice);
        AutostartStub startup;
        startup.state.pendingApproval = true;
        SaveSettingsActions actions;
        for (int index = 0; index < 20; ++index)
            actions.shapeTemplates.append({QStringLiteral("template%1.png").arg(index),
                                           QStringLiteral("测试模板 %1").arg(index), true});
        const QString notice =
            longNotice ? QStringLiteral("测试状态：设置未保存，请检查目录或系统授权。 ").repeated(8)
                       : QStringLiteral("测试状态：设置未保存。");
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const QKeySequence&) { return QString(); }, notice,
            nullptr, actions, startup.actions());
        dialog.show();
        auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->currentIndex(), 0);
        const QStringList titles = {QStringLiteral("通用"), QStringLiteral("保存"),
                                    QStringLiteral("截图"), QStringLiteral("贴图")};
        const QStringList pages = {
            QStringLiteral("settingsGeneralPage"), QStringLiteral("settingsSavePage"),
            QStringLiteral("settingsCapturePage"), QStringLiteral("settingsStickerPage")};
        const QList<QStringList> controls = {
            {QStringLiteral("hotkeyEditor"), QStringLiteral("hotkeyHelp"),
             QStringLiteral("autostartCheck"), QStringLiteral("autostartHelp")},
            {QStringLiteral("quickDirectory"), QStringLiteral("chooseDirectoryButton"),
             QStringLiteral("clearDirectoryButton"), QStringLiteral("saveFormat"),
             QStringLiteral("jpegLossNotice")},
            {QStringLiteral("ratioList"), QStringLiteral("ratioWidthInput"),
             QStringLiteral("ratioHeightInput"), QStringLiteral("ratioAddButton"),
             QStringLiteral("ratioRestoreButton"), QStringLiteral("quickRatioCombo"),
             QStringLiteral("shapeTemplateList"), QStringLiteral("shapeTemplateImportButton")},
            {QStringLiteral("stickerCloseReminderCheck"),
             QStringLiteral("stickerCloseReminderHelp")}};
        dialog.findChild<QComboBox*>(QStringLiteral("saveFormat"))->setCurrentIndex(1);
        // 最小宽度下，长状态提示和两个列表仍不挤占公共底栏。
        dialog.resize(dialog.minimumSize());
        for (int page = 0; page < 4; ++page)
        {
            QCOMPARE(tabs->tabText(page), titles[page]);
            QCOMPARE(tabs->widget(page)->objectName(), pages[page]);
            QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier,
                              tabs->tabBar()->tabRect(page).center());
            QCOMPARE(tabs->currentIndex(), page);
            QApplication::processEvents();
            if (!longNotice)
                QVERIFY(dialog.height() <= 760);
            QVERIFY(tabs->tabBar()->tabRect(page).width() >=
                    tabs->tabBar()->fontMetrics().horizontalAdvance(titles[page]));
            for (const auto& name : controls[page])
            {
                auto* control = dialog.findChild<QWidget*>(name);
                QVERIFY2(control, qPrintable(name));
                QVERIFY(tabs->widget(page)->isAncestorOf(control));
                QVERIFY2(control->isVisible(), qPrintable(name));
                QVERIFY(!control->visibleRegion().isEmpty());
            }
            for (const auto& name :
                 {QStringLiteral("settingsStatus"), QStringLiteral("saveSettingsButton"),
                  QStringLiteral("cancelSettingsButton")})
            {
                auto* control = dialog.findChild<QWidget*>(name);
                QVERIFY(control && control->isVisible());
                QVERIFY(!tabs->isAncestorOf(control));
                const QRect rect(control->mapTo(&dialog, QPoint()), control->size());
                QVERIFY(dialog.rect().contains(rect));
                const QRect tabRect(tabs->mapTo(&dialog, QPoint()), tabs->size());
                QVERIFY(!tabRect.intersects(rect));
            }
        }
        tabs->setCurrentIndex(2);
        // 动态列表的末行可通过原滚动区到达。
        for (const auto& name :
             {QStringLiteral("ratioDelete7"), QStringLiteral("shapeTemplateDelete19")})
        {
            auto* control = dialog.findChild<QPushButton*>(name);
            QVERIFY(control);
            auto* scroll =
                qobject_cast<QScrollArea*>(control->parentWidget()->parentWidget()->parentWidget());
            QVERIFY(scroll);
            scroll->ensureWidgetVisible(control);
            QApplication::processEvents();
            QVERIFY(control->isVisible());
            QVERIFY(scroll->viewport()->rect().contains(
                QRect(control->mapTo(scroll->viewport(), QPoint()), control->size())));
            auto* group = scroll->parentWidget();
            for (auto* sibling :
                 group->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
            {
                if (sibling == scroll || !sibling->isVisible())
                    continue;
                QVERIFY(group->rect().contains(sibling->geometry()));
                QVERIFY2(
                    !scroll->geometry().intersects(sibling->geometry()),
                    qPrintable(QStringLiteral("%1 / %2：列表 %3,%4,%5,%6；控件 %7,%8,%9,%10")
                                   .arg(QString::fromLatin1(sibling->metaObject()->className()),
                                        sibling->objectName())
                                   .arg(scroll->x())
                                   .arg(scroll->y())
                                   .arg(scroll->width())
                                   .arg(scroll->height())
                                   .arg(sibling->x())
                                   .arg(sibling->y())
                                   .arg(sibling->width())
                                   .arg(sibling->height())));
                if (auto* label = qobject_cast<QLabel*>(sibling); label && label->wordWrap())
                    QVERIFY(label->height() >= label->heightForWidth(label->width()));
            }
        }
        tabs->setCurrentIndex(3);
        QTest::keyClick(tabs, Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(tabs->currentIndex(), 0);
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("autostartHelp"))
                    ->text()
                    .contains(QStringLiteral("等待批准")));
        QCOMPARE(startup.queries, 1);
        QVERIFY(startup.requests.isEmpty());
        const QString evidence = qEnvironmentVariable("WAIBUSNAP_SETTINGS_EVIDENCE_DIR");
        if (!evidence.isEmpty())
            for (int page = 0; page < 4; ++page)
            {
                tabs->setCurrentIndex(page);
                QApplication::processEvents();
                QVERIFY(dialog.grab().save(QDir(evidence).filePath(
                    QStringLiteral("settings-%1-%2.png")
                        .arg(longNotice ? QStringLiteral("long") : QStringLiteral("normal"))
                        .arg(page))));
            }
    }
    void settingsTabsDraftAndPersistence_data()
    {
        QTest::addColumn<int>("decision");
        QTest::addColumn<int>("lastPage");
        for (int page = 0; page < 4; ++page)
        {
            QTest::newRow(qPrintable(QStringLiteral("save-page-%1").arg(page))) << 0 << page;
            QTest::newRow(qPrintable(QStringLiteral("cancel-page-%1").arg(page))) << 1 << page;
            QTest::newRow(qPrintable(QStringLiteral("escape-page-%1").arg(page))) << 2 << page;
        }
    }
    void settingsTabsDraftAndPersistence()
    {
        using namespace waibusnap;
        QFETCH(int, decision);
        QFETCH(int, lastPage);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        const auto original = defaultScreenshotHotkey();
        QVERIFY(settings.saveHotkey(original).isEmpty());
        const QString maskPath = temporary.filePath(QStringLiteral("模板.png"));
        QImage mask(4, 4, QImage::Format_ARGB32);
        mask.fill(Qt::white);
        QVERIFY(mask.save(maskPath));
        const auto imported = settings.importShapeTemplates({maskPath});
        QVERIFY(imported.error.isEmpty());
        QVERIFY(settings.saveShapeTemplates(imported.imported).isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray before = file.readAll();
        file.close();
        QStringList writes;
        SaveSettingsActions actions;
        actions.shapeTemplates = settings.loadShapeTemplates();
        actions.shapeTemplateDir = settings.shapeTemplateDir();
        actions.chooseDirectory = [&](QWidget*, const QString&) { return temporary.path(); };
        actions.savePreferences = [&](const SavePreferences& preferences)
        {
            writes.append(QStringLiteral("保存"));
            return settings.saveSavePreferences(preferences);
        };
        actions.saveRatios = [&](const CaptureRatioSettings& ratios)
        {
            writes.append(QStringLiteral("比例"));
            return settings.saveCaptureRatios(ratios);
        };
        actions.saveShapeTemplates = [&](const QVector<ShapeTemplateEntry>& entries)
        {
            writes.append(QStringLiteral("模板"));
            return settings.saveShapeTemplates(entries);
        };
        actions.saveStickerCloseReminder = [&](bool enabled)
        {
            writes.append(QStringLiteral("贴图"));
            return settings.saveStickerCloseReminder(enabled);
        };
        AutostartStub startup;
        startup.state = {true, true, {}};
        auto autostart = startup.actions();
        const auto setEnabled = autostart.setEnabled;
        autostart.setEnabled = [&](bool enabled)
        {
            writes.append(QStringLiteral("启动"));
            return setEnabled(enabled);
        };
        SettingsDialog dialog(
            original,
            [&](const QKeySequence& sequence)
            {
                writes.append(QStringLiteral("热键"));
                return settings.saveHotkey(sequence);
            },
            {}, nullptr, actions, autostart);
        dialog.show();
        auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
        QVERIFY(tabs);
        auto* editor = dialog.findChild<QKeySequenceEdit*>(QStringLiteral("hotkeyEditor"));
        editor->setFocus();
        QTest::keyClick(editor, Qt::Key_F2);
        QCOMPARE(editor->keySequence(), QKeySequence(Qt::Key_F2));
        auto* check = dialog.findChild<QCheckBox*>(QStringLiteral("autostartCheck"));
        check->setChecked(false);
        tabs->setCurrentIndex(1);
        dialog.findChild<QPushButton*>(QStringLiteral("chooseDirectoryButton"))->click();
        auto* format = dialog.findChild<QComboBox*>(QStringLiteral("saveFormat"));
        format->setCurrentIndex(1);
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("jpegLossNotice"))->isVisible());
        tabs->setCurrentIndex(2);
        auto* wide = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ratioWidthInput"));
        auto* high = dialog.findChild<QDoubleSpinBox*>(QStringLiteral("ratioHeightInput"));
        wide->setValue(5);
        high->setValue(4);
        dialog.findChild<QPushButton*>(QStringLiteral("ratioAddButton"))->click();
        dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle0"))->setChecked(false);
        auto* quick = dialog.findChild<QComboBox*>(QStringLiteral("quickRatioCombo"));
        quick->setCurrentText(QStringLiteral("5:4"));
        auto* shape = dialog.findChild<QCheckBox*>(QStringLiteral("shapeTemplateToggle0"));
        shape->setChecked(false);
        wide->setValue(7);
        tabs->setCurrentIndex(3);
        auto* reminder = dialog.findChild<QCheckBox*>(QStringLiteral("stickerCloseReminderCheck"));
        QVERIFY(reminder && reminder->isChecked());
        reminder->setChecked(false);
        for (int page : {0, 1, 2, 3, 0, 2, 1, lastPage})
            tabs->setCurrentIndex(page);
        QCOMPARE(editor->keySequence(), QKeySequence(Qt::Key_F2));
        QVERIFY(!check->isChecked());
        QCOMPARE(dialog.findChild<QLineEdit*>(QStringLiteral("quickDirectory"))->text(),
                 temporary.path());
        QCOMPARE(format->currentIndex(), 1);
        QCOMPARE(wide->value(), 7.0);
        QCOMPARE(high->value(), 4.0);
        QCOMPARE(quick->currentText(), QStringLiteral("5:4"));
        QVERIFY(!dialog.findChild<QCheckBox*>(QStringLiteral("ratioToggle0"))->isChecked());
        QVERIFY(!shape->isChecked());
        QVERIFY(!reminder->isChecked());
        QVERIFY(writes.isEmpty());
        QCOMPARE(startup.queries, 1);
        if (decision == 2)
        {
            QWidget* focus = lastPage == 0   ? static_cast<QWidget*>(editor)
                             : lastPage == 1 ? static_cast<QWidget*>(format)
                             : lastPage == 2 ? static_cast<QWidget*>(wide)
                                             : static_cast<QWidget*>(reminder);
            focus->setFocus();
            QTest::keyClick(focus, Qt::Key_Escape);
        }
        else
            QTest::mouseClick(dialog.findChild<QPushButton*>(
                                  decision == 0 ? QStringLiteral("saveSettingsButton")
                                                : QStringLiteral("cancelSettingsButton")),
                              Qt::LeftButton);
        QVERIFY(!dialog.isVisible());
        QCOMPARE(dialog.result(), decision == 0 ? int(QDialog::Accepted) : int(QDialog::Rejected));
        if (decision == 0)
        {
            QCOMPARE(writes, (QStringList{QStringLiteral("热键"), QStringLiteral("保存"),
                                          QStringLiteral("启动"), QStringLiteral("比例"),
                                          QStringLiteral("模板"), QStringLiteral("贴图")}));
            QVERIFY(!AppSettings(path).loadStickerCloseReminder());
            QCOMPARE(settings.loadHotkey().sequence, QKeySequence(Qt::Key_F2));
            QCOMPARE(settings.loadSavePreferences().quickDirectory, temporary.path());
            QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Jpeg);
            const auto ratios = settings.loadCaptureRatios();
            QCOMPARE(ratios.enabled.size(), 8);
            QCOMPARE(serializeRatioList(ratios.disabled), QStringLiteral("1:1"));
            QCOMPARE(formatAspectRatio(*ratios.quick), QStringLiteral("5:4"));
            const auto shapes = settings.loadShapeTemplates();
            QCOMPARE(shapes.size(), 1);
            QVERIFY(!shapes[0].enabled);
            QCOMPARE(startup.queries, 2);
            QCOMPARE(startup.requests, QVector<bool>{false});
        }
        else
        {
            QVERIFY(writes.isEmpty());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), before);
            QCOMPARE(startup.queries, 1);
            QVERIFY(startup.requests.isEmpty());
        }
    }
    void settingsMetadataValidationFailureAndPersistence()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        waibusnap::AppSettings settings(path);
        const auto original = waibusnap::defaultScreenshotHotkey();
        QVERIFY(settings.saveHotkey(original).isEmpty());
        int registrations = 0;
        QKeySequence enabled = original;
        waibusnap::SettingsDialog dialog(original,
                                         [&](const QKeySequence& sequence)
                                         {
                                             ++registrations;
                                             if (registrations == 1)
                                                 return QStringLiteral("测试注册冲突，原绑定保留");
                                             enabled = sequence;
                                             return settings.saveHotkey(sequence);
                                         });
        dialog.show();
        auto* editor = dialog.findChild<QKeySequenceEdit*>(QStringLiteral("hotkeyEditor"));
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"));
        auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("cancelSettingsButton"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("settingsStatus"));
        auto* help = dialog.findChild<QLabel*>(QStringLiteral("hotkeyHelp"));
        QVERIFY(editor && save && cancel && status && help);
        QCOMPARE(editor->maximumSequenceLength(), qsizetype(1));
        QCOMPARE(editor->keySequence(), original);
        QCOMPARE(save->text(), QStringLiteral("保存"));
        QCOMPARE(cancel->text(), QStringLiteral("取消"));
        QVERIFY(help->text().contains(QStringLiteral("Fn / 地球键")));
        QVERIFY(help->text().contains(QStringLiteral("不修改系统键盘设置")));
        editor->setKeySequence(QKeySequence(Qt::Key_A));
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(registrations, 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(!status->text().isEmpty());
        QCOMPARE(enabled, original);
        QCOMPARE(settings.loadHotkey().sequence, original);
        const auto chosen = QKeySequence::fromString(QStringLiteral("Ctrl+Shift+2"));
        editor->setKeySequence(chosen);
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(registrations, 1);
        QVERIFY(dialog.isVisible());
        QVERIFY(status->text().contains(QStringLiteral("原绑定保留")));
        QCOMPARE(enabled, original);
        QCOMPARE(settings.loadHotkey().sequence, original);
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(registrations, 2);
        QCOMPARE(enabled, chosen);
        QCOMPARE(settings.loadHotkey().sequence, chosen);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
    }
    void settingsCancelDoesNotSave_data()
    {
        QTest::addColumn<bool>("escape");
        QTest::newRow("cancel") << false;
        QTest::newRow("escape-editor") << true;
    }
    void settingsCancelDoesNotSave()
    {
        QFETCH(bool, escape);
        int saves = 0;
        waibusnap::SettingsDialog dialog(waibusnap::defaultScreenshotHotkey(),
                                         [&](const QKeySequence&)
                                         {
                                             ++saves;
                                             return QString();
                                         });
        dialog.show();
        auto* editor = dialog.findChild<QKeySequenceEdit*>(QStringLiteral("hotkeyEditor"));
        editor->setKeySequence(QKeySequence::fromString(QStringLiteral("Ctrl+Shift+2")));
        if (escape)
            QTest::keyClick(editor, Qt::Key_Escape);
        else
            QTest::mouseClick(
                dialog.findChild<QPushButton*>(QStringLiteral("cancelSettingsButton")),
                Qt::LeftButton);
        QCOMPARE(saves, 0);
        QVERIFY(!dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
    void windowHoverClickAndAdjustment()
    {
        const QVector<QRect> windows = {{20, 10, 60, 50}, {40, 20, 100, 80}};
        waibusnap::SelectionOverlay overlay(sampleFrame(), {}, windows);
        QSignalSpy finished(&overlay, &waibusnap::SelectionOverlay::finished);
        overlay.show();
        QTest::mouseMove(&overlay, {50, 35});
        QCOMPARE(overlay.hoveredWindowPixels(), QRect(40, 20, 120, 100));
        QCOMPARE(overlay.cursor().shape(), Qt::CrossCursor);
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 35});
        QVERIFY(overlay.hoveredWindowPixels().isEmpty());
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {52, 36});
        QCOMPARE(overlay.selection(), QRect(40, 20, 120, 100));
        QCOMPARE(finished.count(), 0);
        QVERIFY(overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"))->isVisible());
        QTest::mouseMove(&overlay, {100, 60});
        QVERIFY(overlay.hoveredWindowPixels().isEmpty());
        drag(overlay, {50, 35}, {60, 45});
        QCOMPARE(overlay.selection(), QRect(60, 40, 120, 100));
        drag(overlay, {90, 70}, {100, 80});
        QCOMPARE(overlay.selection(), QRect(60, 40, 140, 120));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 50});
        QVERIFY(overlay.selection().isEmpty());
        QCOMPARE(overlay.hoveredWindowPixels(), QRect(80, 40, 200, 160));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 35});
        QCOMPARE(overlay.selection(), QRect(40, 20, 120, 100));
        drag(overlay, {200, 100}, {270, 160});
        QCOMPARE(overlay.selection(), QRect(400, 200, 140, 120));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {300, 180});
        QVERIFY(overlay.selection().isEmpty());
        QVERIFY(overlay.hoveredWindowPixels().isEmpty());
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {300, 180});
        QVERIFY(overlay.selection().isEmpty());
        QCOMPARE(finished.count(), 0);
    }
    void windowDragWinsAndOutputsFrozenPixels()
    {
        int copies = 0;
        QImage copied;
        waibusnap::OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            ++copies;
            return waibusnap::ImageOutputResult{true, {}};
        };
        waibusnap::SelectionOverlay overlay(sampleFrame(), actions, {{20, 10, 60, 50}});
        overlay.show();
        drag(overlay, {50, 35}, {70, 45});
        QCOMPARE(overlay.selection(), QRect(100, 70, 40, 20));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {300, 180});
        // 已拖动的手势即使回到按下点，也不能误吸附。
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 35});
        QTest::mouseMove(&overlay, {70, 45});
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 35});
        QVERIFY(overlay.selection().isEmpty());
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 35});
        QCOMPARE(copies, 0);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("吸附.png"));
        QVERIFY(overlay.exportToPath(path));
        QCOMPARE(QImage(path).pixelColor(10, 10), QColor(Qt::green));
        QTest::mouseClick(overlay.findChild<QPushButton*>(QStringLiteral("copyButton")),
                          Qt::LeftButton);
        QCOMPARE(copies, 1);
        QCOMPARE(copied.size(), QSize(120, 100));
        QCOMPARE(copied.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(copied.pixelColor(10, 10), QColor(Qt::green));
    }
    void enumerationErrorStillAllowsManualSelection()
    {
        waibusnap::SelectionOverlay overlay(sampleFrame(), {}, {{20, 10, 60, 50}},
                                            QStringLiteral("枚举失败"));
        overlay.show();
        QTest::mouseMove(&overlay, {50, 35});
        QVERIFY(overlay.hoveredWindowPixels().isEmpty());
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 35});
        QVERIFY(overlay.selection().isEmpty());
        drag(overlay, {20, 10}, {80, 60});
        QCOMPARE(overlay.selection(), QRect(40, 20, 120, 100));
    }
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
        clickAnnotationTool(overlay, "lineToolButton");
        drag(overlay, {30, 35}, {70, 35});
        QCOMPARE(overlay.annotations().size(), qsizetype(1));
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
    void selectionNudgeKeys_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::addColumn<int>("direction");
        QTest::addColumn<int>("mode");
        for (qreal scale : {1.0, 1.5, 2.0})
            for (int mode = 0; mode < 3; ++mode)
                for (int direction = 0; direction < 4; ++direction)
                    QTest::newRow(
                        qPrintable(QStringLiteral("%1-%2-%3").arg(scale).arg(mode).arg(direction)))
                        << scale << direction << mode;
    }
    void selectionNudgeKeys()
    {
        using namespace waibusnap;
        QFETCH(qreal, scale);
        QFETCH(int, direction);
        QFETCH(int, mode);
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = scale;
        frame.pixels = mosaicTestImage(QSize(800, 600) * scale);
        frame.pixels.setDevicePixelRatio(scale);
        SelectionOverlay overlay(frame);
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {200, 200});
        QTRY_VERIFY(overlay.hasFocus());
        const Qt::Key keys[] = {Qt::Key_Left, Qt::Key_Up, Qt::Key_Right, Qt::Key_Down};
        const Qt::KeyboardModifiers modifiers[] = {Qt::NoModifier, Qt::ShiftModifier,
                                                   Qt::AltModifier};
        QRect expected(qRound(100 * scale), qRound(100 * scale), qRound(100 * scale),
                       qRound(100 * scale));
        QCOMPARE(overlay.selection(), expected);
        const auto step = [&]
        {
            if (mode == 0)
                expected.translate(direction == 0   ? -1
                                   : direction == 2 ? 1
                                                    : 0,
                                   direction == 1   ? -1
                                   : direction == 3 ? 1
                                                    : 0);
            else
            {
                const int amount = mode == 1 ? 1 : -1;
                expected.adjust(direction == 0 ? -amount : 0, direction == 1 ? -amount : 0,
                                direction == 2 ? amount : 0, direction == 3 ? amount : 0);
            }
        };
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            // 真实重复事件也必须逐次移动一个源像素，不能变成逻辑像素或加速。
            QKeyEvent key(QEvent::KeyPress, keys[direction], modifiers[mode], QString(), repeat > 0,
                          1);
            QApplication::sendEvent(&overlay, &key);
            step();
            QVERIFY(key.isAccepted());
            QCOMPARE(overlay.selection(), expected);
            QCOMPARE(overlay.selectionSizeText(),
                     QStringLiteral("%1 × %2 像素").arg(expected.width()).arg(expected.height()));
            const QPoint anchor = mode == 0        ? expected.topLeft()
                                  : direction == 0 ? QPoint(expected.left(), expected.center().y())
                                  : direction == 1 ? QPoint(expected.center().x(), expected.top())
                                  : direction == 2
                                      ? QPoint(expected.right(), expected.center().y())
                                      : QPoint(expected.center().x(), expected.bottom());
            QVERIFY(overlay.magnifierVisible());
            QCOMPARE(overlay.magnifierAnchor(), anchor);
            QCOMPARE(overlay.magnifierPositionText(),
                     QStringLiteral("%1, %2").arg(anchor.x()).arg(anchor.y()));
            const QPoint cell = anchor - overlay.magnifierSampleRect().topLeft();
            QCOMPARE(overlay.magnifierSample().pixelColor(cell), frame.pixels.pixelColor(anchor));
        }
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        QVERIFY(toolbar && toolbar->isVisible());
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
        for (const auto& name : {"selectionToolbar", "annotationOptionsBar", "outputStatus"})
        {
            auto* panel = overlay.findChild<QWidget*>(QString::fromLatin1(name));
            QVERIFY(panel);
            if (panel->isVisible())
                QVERIFY(!panel->geometry().intersects(overlay.magnifierRect()));
        }
        QVERIFY(!overlay.magnifierRect().contains(
            (QPointF(overlay.magnifierAnchor()) / scale).toPoint()));
    }
    void selectionNudgeLimits_data()
    {
        QTest::addColumn<int>("direction");
        QTest::addColumn<int>("mode");
        for (int mode = 0; mode < 3; ++mode)
            for (int direction = 0; direction < 4; ++direction)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(mode).arg(direction)))
                    << direction << mode;
    }
    void selectionNudgeLimits()
    {
        using namespace waibusnap;
        QFETCH(int, direction);
        QFETCH(int, mode);
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        overlay.activateWindow();
        if (mode == 2)
            drag(overlay, {100, 100}, {101, 101});
        else
            drag(overlay, {1, 1}, {799, 599});
        QTRY_VERIFY(overlay.hasFocus());
        const Qt::Key keys[] = {Qt::Key_Left, Qt::Key_Up, Qt::Key_Right, Qt::Key_Down};
        const Qt::KeyboardModifiers modifiers[] = {Qt::NoModifier, Qt::ShiftModifier,
                                                   Qt::AltModifier};
        for (int index = 0; index < 6; ++index)
            QTest::keyClick(&overlay, keys[direction], modifiers[mode]);
        const QRect result = overlay.selection();
        const QRect expected[] = {QRect(201, 200, 1, 2), QRect(200, 201, 2, 1),
                                  QRect(200, 200, 1, 2), QRect(200, 200, 2, 1)};
        if (mode == 2)
            QCOMPARE(result, expected[direction]);
        else if (direction == 0)
            QCOMPARE(result.left(), 0);
        else if (direction == 1)
            QCOMPARE(result.top(), 0);
        else if (direction == 2)
            QCOMPARE(result.right(), 1599);
        else
            QCOMPARE(result.bottom(), 1199);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(overlay.exportToPath(temporary.filePath(QStringLiteral("边界.png"))));
        QVERIFY(overlay.isSelectionSaved());
        QKeyEvent limit(QEvent::KeyPress, keys[direction], modifiers[mode]);
        limit.ignore();
        QApplication::sendEvent(&overlay, &limit);
        QVERIFY(limit.isAccepted());
        QCOMPARE(overlay.selection(), result);
        QVERIFY(overlay.isSelectionSaved());
        // 两个维度都缩到 1 后，任意收缩仍不得翻转。
        if (mode == 2)
        {
            for (int index = 0; index < 6; ++index)
                QTest::keyClick(&overlay, direction % 2 == 0 ? Qt::Key_Up : Qt::Key_Left,
                                Qt::AltModifier);
            QCOMPARE(overlay.selection().size(), QSize(1, 1));
            const QRect minimum = overlay.selection();
            for (Qt::Key key : keys)
                QTest::keyClick(&overlay, key, Qt::AltModifier);
            QCOMPARE(overlay.selection(), minimum);
        }
    }
    void selectionNudgeIgnoresModifiersAndControlFocus()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {200, 200});
        QTRY_VERIFY(overlay.hasFocus());
        const QRect before = overlay.selection();
        const Qt::KeyboardModifiers ignored[] = {Qt::ShiftModifier | Qt::AltModifier,
                                                 Qt::ControlModifier,
                                                 Qt::MetaModifier,
                                                 Qt::ControlModifier | Qt::ShiftModifier,
                                                 Qt::MetaModifier | Qt::AltModifier,
                                                 Qt::ShiftModifier | Qt::AltModifier |
                                                     Qt::MetaModifier,
                                                 Qt::ControlModifier | Qt::AltModifier,
                                                 Qt::KeypadModifier};
        for (auto modifiers : ignored)
            for (Qt::Key key : {Qt::Key_Left, Qt::Key_Up, Qt::Key_Right, Qt::Key_Down})
                QTest::keyClick(&overlay, key, modifiers);
        QCOMPARE(overlay.selection(), before);
        QVERIFY(!overlay.magnifierVisible());
        clickAnnotationTool(overlay, "rectangleToolButton");
        auto* color = button(overlay, "annotationColor0Button");
        color->setFocus();
        QTRY_VERIFY(color->hasFocus());
        QTest::keyClick(color, Qt::Key_Down);
        QTest::keyClick(&overlay, Qt::Key_Right);
        QCOMPARE(overlay.selection(), before);
        QVERIFY(!overlay.magnifierVisible());
        // 无选区也不触发；已有鼠标交互不能被键盘更改快照。
        SelectionOverlay empty(annotationFrame());
        empty.show();
        empty.activateWindow();
        empty.setFocus();
        QTRY_VERIFY(empty.hasFocus());
        QTest::keyClick(&empty, Qt::Key_Right);
        QVERIFY(empty.selection().isEmpty());
        QVERIFY(!empty.magnifierVisible());
        QTest::mousePress(&empty, Qt::LeftButton, Qt::NoModifier, {100, 100});
        QTest::mouseMove(&empty, {200, 200});
        const QRect dragging = empty.selection();
        QTest::keyClick(&empty, Qt::Key_Right);
        QCOMPARE(empty.selection(), dragging);
        QTest::mouseRelease(&empty, Qt::LeftButton, Qt::NoModifier, {200, 200});
    }
    void selectionNudgeToolAndTextIsolation()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {400, 350});
        QTRY_VERIFY(overlay.hasFocus());
        const QRect before = overlay.selection();
        const char* tools[] = {"rectangleToolButton", "ellipseToolButton",  "lineToolButton",
                               "arrowToolButton",     "freehandToolButton", "textToolButton",
                               "coverToolButton",     "mosaicToolButton"};
        for (const char* tool : tools)
        {
            clickAnnotationTool(overlay, tool);
            QVERIFY(overlay.activeTool().has_value());
            for (auto modifier : {Qt::NoModifier, Qt::ShiftModifier, Qt::AltModifier})
                for (auto key : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down})
                    QTest::keyClick(&overlay, key, modifier);
            QCOMPARE(overlay.selection(), before);
            QVERIFY(!overlay.magnifierVisible());
            drag(overlay, {150, 150}, {200, 200});
            QVERIFY(!overlay.magnifierVisible());
            if (overlay.activeTool() == AnnotationType::Text)
            {
                auto* editor = overlay.findChild<AnnotationTextEdit*>();
                QVERIFY(editor && editor->isVisible());
                editor->setPlainText(QStringLiteral("测试文本 ABC"));
                editor->moveCursor(QTextCursor::End);
                const int end = editor->textCursor().position();
                QTest::keyClick(editor, Qt::Key_Left);
                QCOMPARE(editor->textCursor().position(), end - 1);
                QInputMethodEvent preedit(QStringLiteral("候选"), {});
                QApplication::sendEvent(editor, &preedit);
                QVERIFY(editor->isComposing());
                QTest::keyClick(editor, Qt::Key_Right);
                QCOMPARE(overlay.selection(), before);
                QVERIFY(!overlay.magnifierVisible());
                QInputMethodEvent clear;
                QApplication::sendEvent(editor, &clear);
            }
            clickAnnotationTool(overlay, tool);
            QVERIFY(!overlay.activeTool());
        }
    }
    void selectionNudgeHistoryDirtyAndOutput()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto frame = annotationFrame();
        QImage copied;
        OverlayActions actions;
        actions.copyImage = [&](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        SelectionOverlay overlay(frame, actions);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {400, 350});
        clickAnnotationTool(overlay, "rectangleToolButton");
        drag(overlay, {150, 150}, {200, 200});
        clickAnnotationTool(overlay, "rectangleToolButton");
        QTest::mouseClick(button(overlay, "undoAnnotationButton"), Qt::LeftButton);
        QVERIFY(overlay.annotations().isEmpty());
        QVERIFY(button(overlay, "redoAnnotationButton")->isEnabled());
        const QString first = temporary.filePath(QStringLiteral("微调前.png"));
        QVERIFY(overlay.exportToPath(first));
        QVERIFY(overlay.isSelectionSaved());
        QTRY_VERIFY(overlay.hasFocus());
        const QRect before = overlay.selection();
        QTest::keyClick(&overlay, Qt::Key_Right);
        QCOMPARE(overlay.selection(), before.translated(1, 0));
        QVERIFY(!overlay.isSelectionSaved());
        QVERIFY(!button(overlay, "undoAnnotationButton")->isEnabled());
        QVERIFY(button(overlay, "redoAnnotationButton")->isEnabled());
        QTest::mouseClick(button(overlay, "redoAnnotationButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), 1);
        const QRect nudged = overlay.selection();
        standardKey(&overlay, QKeySequence::Undo);
        QCOMPARE(overlay.selection(), nudged);
        QVERIFY(overlay.annotations().isEmpty());
        standardKey(&overlay, QKeySequence::Redo);
        QCOMPARE(overlay.annotations().size(), 1);
        QTest::keyClick(&overlay, Qt::Key_Down, Qt::ShiftModifier);
        QVERIFY(overlay.magnifierVisible());
        const QImage expected =
            renderAnnotatedSelection(frame.pixels, overlay.annotations(), overlay.selection());
        const QString path = temporary.filePath(QStringLiteral("微调后.png"));
        QVERIFY(overlay.exportToPath(path));
        const QImage saved(path);
        QCOMPARE(saved, expected.convertToFormat(saved.format()));
        QVERIFY(overlay.isSelectionSaved());
        QVERIFY(overlay.magnifierVisible());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(!overlay.isSelectionSaved());
        const QImage finalImage =
            renderAnnotatedSelection(frame.pixels, overlay.annotations(), overlay.selection());
        QTest::mouseClick(button(overlay, "copyButton"), Qt::LeftButton);
        QCOMPARE(copied, finalImage);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toInt(), copiedSessionOutcome);
        QVERIFY(!overlay.magnifierVisible());
        QVERIFY(overlay.magnifierSample().isNull());
    }
    void selectionNudgeSavePanelAndFinishedIsolation()
    {
        using namespace waibusnap;
        SelectionOverlay* current = nullptr;
        bool panelChecked = false;
        OverlayActions actions;
        actions.chooseSavePath = [&](QWidget*, const QString&)
        {
            const QRect before = current->selection();
            current->setFocus();
            QTest::keyClick(current, Qt::Key_Right);
            panelChecked = current->selection() == before && !current->magnifierVisible();
            return QString();
        };
        SelectionOverlay overlay(annotationFrame(), actions);
        current = &overlay;
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {200, 200});
        QTRY_VERIFY(overlay.hasFocus());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(overlay.magnifierVisible());
        QTest::mouseClick(button(overlay, "saveButton"), Qt::LeftButton);
        QVERIFY(panelChecked);
        const QRect before = overlay.selection();
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QTest::keyClick(&overlay, Qt::Key_Right);
        QCOMPARE(overlay.selection(), before);
        QVERIFY(!overlay.magnifierVisible());
        QVERIFY(overlay.magnifierSampleRect().isEmpty());
    }
    void magnifierMouseSampling_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::addColumn<QPointF>("position");
        const QPointF points[] = {{0, 0},     {800, 0}, {0, 600},   {800, 600}, {400, 0},
                                  {400, 600}, {0, 300}, {800, 300}, {250, 200}};
        for (qreal scale : {1.0, 1.5, 2.0})
            for (int index = 0; index < 9; ++index)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(scale).arg(index)))
                    << scale << points[index];
    }
    void magnifierMouseSampling()
    {
        using namespace waibusnap;
        QFETCH(qreal, scale);
        QFETCH(QPointF, position);
        auto frame = annotationFrame();
        frame.display.devicePixelRatio = scale;
        frame.display.logicalGeometry.moveTopLeft({-800, -600});
        frame.pixels = mosaicTestImage(QSize(800, 600) * scale);
        frame.pixels.setDevicePixelRatio(scale);
        SelectionOverlay overlay(frame);
        overlay.show();
        QMouseEvent press(QEvent::MouseButtonPress, position, overlay.mapToGlobal(position),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &press);
        QVERIFY(overlay.magnifierVisible());
        const QPoint anchor(std::clamp(qRound(position.x() * scale), 0, frame.pixels.width() - 1),
                            std::clamp(qRound(position.y() * scale), 0, frame.pixels.height() - 1));
        QCOMPARE(overlay.magnifierAnchor(), anchor);
        QCOMPARE(overlay.magnifierPositionText(),
                 QStringLiteral("%1, %2").arg(anchor.x()).arg(anchor.y()));
        const QRect sampled = overlay.magnifierSampleRect();
        QVERIFY(sampled.contains(anchor));
        QVERIFY(frame.pixels.rect().contains(sampled));
        QCOMPARE(sampled.size(), QSize(15, 15));
        const QImage sample = overlay.magnifierSample();
        QCOMPARE(sample.devicePixelRatio(), qreal(1));
        for (int y = 0; y < sample.height(); ++y)
            for (int x = 0; x < sample.width(); ++x)
                QCOMPARE(sample.pixelColor(x, y),
                         frame.pixels.pixelColor(sampled.x() + x, sampled.y() + y));
        if (position == QPointF(250, 200))
            QCOMPARE(sample.pixelColor(7, 7), frame.pixels.pixelColor(anchor));
        QVERIFY(!overlay.magnifierRect().contains((QPointF(anchor) / scale).toPoint()));
        QMouseEvent release(QEvent::MouseButtonRelease, position, overlay.mapToGlobal(position),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &release);
        QVERIFY(!overlay.magnifierVisible());
        QVERIFY(overlay.magnifierSample().isNull());
    }
    void magnifierFollowsSelectionGestures()
    {
        using namespace waibusnap;
        const QPoint starts[] = {{300, 200}, {200, 220}, {400, 220}, {300, 150}, {300, 300}};
        const QRect expected[] = {{404, 306, 400, 300},
                                  {404, 300, 396, 300},
                                  {400, 300, 404, 300},
                                  {400, 306, 400, 294},
                                  {400, 300, 400, 306}};
        for (int index = 0; index < 5; ++index)
        {
            SelectionOverlay overlay(annotationFrame());
            overlay.show();
            drag(overlay, {200, 150}, {400, 300});
            const QPoint start = starts[index];
            QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, start);
            QVERIFY(overlay.magnifierVisible());
            QCOMPARE(overlay.magnifierAnchor(), start * 2);
            QTest::mouseMove(&overlay, start + QPoint(2, 3));
            QCOMPARE(overlay.magnifierAnchor(), (start + QPoint(2, 3)) * 2);
            QCOMPARE(overlay.selection(), expected[index]);
            QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, start + QPoint(2, 3));
            QVERIFY(!overlay.magnifierVisible());
        }
    }
    void magnifierPaintUsesNearestPixels()
    {
        using namespace waibusnap;
        auto frame = annotationFrame();
        frame.pixels = mosaicTestImage(frame.pixels.size());
        frame.pixels.setDevicePixelRatio(2);
        SelectionOverlay overlay(frame);
        overlay.show();
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {250, 200});
        const QImage painted = overlay.grab().toImage();
        const qreal dpr = painted.devicePixelRatio();
        const QRect panel = overlay.magnifierRect();
        const int cell = qRound(8 * dpr);
        const QPoint grid(qRound(panel.x() * dpr) + qRound(4 * dpr),
                          qRound(panel.y() * dpr) + qRound(4 * dpr));
        const QImage sample = overlay.magnifierSample();
        const QPoint marked = overlay.magnifierAnchor() - overlay.magnifierSampleRect().topLeft();
        int black = 0, white = 0;
        for (int y = 0; y < sample.height() * cell; ++y)
            for (int x = 0; x < sample.width() * cell; ++x)
            {
                const QColor actual = painted.pixelColor(grid + QPoint(x, y));
                if (QPoint(x / cell, y / cell) == marked)
                {
                    black += actual == QColor(Qt::black);
                    white += actual == QColor(Qt::white);
                    if (x % cell == cell / 2 && y % cell == cell / 2)
                        QCOMPARE(actual, sample.pixelColor(marked));
                }
                else
                    QCOMPARE(actual, sample.pixelColor(x / cell, y / cell));
            }
        QVERIFY(black > 0);
        QVERIFY(white > 0);
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {250, 200});
    }
    void magnifierKeyboardTimeoutAndCleanup()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {200, 200});
        QTRY_VERIFY(overlay.hasFocus());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(overlay.magnifierVisible());
        QTest::qWait(450);
        QTest::keyClick(&overlay, Qt::Key_Right);
        QTest::qWait(400);
        QVERIFY(overlay.magnifierVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!overlay.magnifierVisible(), 1200);
        QVERIFY(overlay.magnifierSample().isNull());
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(overlay.magnifierVisible());
        clickAnnotationTool(overlay, "rectangleToolButton");
        QVERIFY(!overlay.magnifierVisible());
        clickAnnotationTool(overlay, "rectangleToolButton");
        QTest::keyClick(&overlay, Qt::Key_Right);
        QVERIFY(overlay.magnifierVisible());
        overlay.close();
        QVERIFY(!overlay.magnifierVisible());
        QVERIFY(overlay.magnifierSampleRect().isEmpty());
        QVERIFY(overlay.magnifierPositionText().isEmpty());
        QTest::qWait(850);
        QVERIFY(!overlay.magnifierVisible());
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
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString executable = qEnvironmentVariable("WAIBUSNAP_TEST_APP");
        QVERIFY2(!executable.isEmpty(), "请通过 CTest 设置应用路径后运行。");
        QProcess process;
        process.setProgram(executable);
        process.setWorkingDirectory(temporary.path());
        process.setArguments({QStringLiteral("--smoke-test"), QStringLiteral("--settings-file"),
                              temporary.filePath(QStringLiteral("settings.ini"))});
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
        QVERIFY(diagnostics.contains("sticker-cleanup-verified"));
    }
    void injectionRequiresExplicitTestMode()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QProcess process;
        process.setProcessEnvironment(childProcessEnvironment());
        process.start(qEnvironmentVariable("WAIBUSNAP_TEST_APP"),
                      {"--test-count", "2", "--settings-file",
                       temporary.filePath(QStringLiteral("settings.ini"))});
        QVERIFY2(process.waitForFinished(5000), qPrintable(process.errorString()));
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("--test-mode"));
    }
};
QTEST_MAIN(StartupSmokeTest)
#include "startup_smoke.moc"
