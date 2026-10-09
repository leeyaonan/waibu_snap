#include "app/app_settings.h"
#include "app/application_controller.h"
#include "app/hotkey_rules.h"
#include "app/sticker_manager.h"
#include "core/sticker_geometry.h"
#include "interfaces/tray_icon.h"
#include "output/annotation_renderer.h"
#include "session/session_metrics.h"
#include "ui/annotation_text_edit.h"
#include "ui/selection_overlay.h"
#include "ui/settings_dialog.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>

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
void stickerClick(waibusnap::StickerWindow& sticker, const char* name)
{
    auto* control = sticker.findChild<QPushButton*>(QString::fromLatin1(name));
    QVERIFY(control);
    QTest::mouseClick(control, Qt::LeftButton);
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
        QCOMPARE(pin->text(), QStringLiteral("钉到屏幕"));
        QVERIFY(button(overlay, "saveButton")->x() < pin->x());
        QVERIFY(pin->x() < button(overlay, "cancelButton")->x());
        QTest::mouseClick(button(overlay, "lineToolButton"), Qt::LeftButton);
        drag(overlay, {120, 150}, {280, 150});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
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
        QTest::mouseClick(button(overlay, "lineToolButton"), Qt::LeftButton);
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
            window->findChild<QComboBox*>(QStringLiteral("stickerEditColorCombo"))
                ->setCurrentIndex(index);
            window->findChild<QComboBox*>(QStringLiteral("stickerEditWidthCombo"))
                ->setCurrentIndex(index);
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
        StickerManager manager;
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
        QCOMPARE(menu->actions().size(), qsizetype(5));
        QCOMPARE(menu->actions()[0]->text(), QStringLiteral("截图"));
        QCOMPARE(menu->actions()[1]->text(), QStringLiteral("设置…"));
        QCOMPARE(menu->actions()[4]->text(), QStringLiteral("退出"));
        auto* hide = menu->actions()[2];
        auto* restore = menu->actions()[3];
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
        QCOMPARE(sticker->contextMenuPolicy(), Qt::NoContextMenu);
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
        auto* colors = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditColorCombo"));
        auto* widths = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditWidthCombo"));
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        QCOMPARE(colors->count(), 3);
        QCOMPARE(widths->count(), 3);
        QCOMPARE(sizes->count(), 3);
        QVERIFY(!sizes->isEnabled());
        for (auto* combo : {colors, widths, sizes})
            QCOMPARE(combo->focusPolicy(), Qt::NoFocus);
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
        QCOMPARE(menu->actions().size(), qsizetype(16));
        QVERIFY(sticker->rect().contains(more->geometry()));
        // 折叠入口仍能启用文本、选择三字号并完成。
        menu->actions()[5]->trigger();
        QVERIFY(sizes->isEnabled());
        menu->actions()[10]->menu()->actions()[2]->trigger();
        QCOMPARE(sizes->currentIndex(), 2);
        menu->actions()[6]->trigger();
        QVERIFY(sticker->activeTool() == AnnotationType::Cover);
        QVERIFY(menu->actions()[6]->isChecked());
        QVERIFY(!menu->actions()[9]->isEnabled());
        QVERIFY(!menu->actions()[10]->isEnabled());
        menu->actions()[6]->trigger();
        QVERIFY(!sticker->activeTool());
        QVERIFY(menu->actions()[9]->isEnabled());
        menu->actions()[7]->trigger();
        QVERIFY(sticker->activeTool() == AnnotationType::Mosaic);
        QVERIFY(menu->actions()[7]->isChecked());
        for (int index : {8, 9, 10})
            QVERIFY(!menu->actions()[index]->isEnabled());
        auto* status = sticker->findChild<QLabel*>(QStringLiteral("stickerStatus"));
        QVERIFY(status->isVisible());
        QVERIFY(status->text().contains(QStringLiteral("高敏感内容请用实心遮盖")));
        menu->actions()[7]->trigger();
        for (int index : {8, 9})
            QVERIFY(menu->actions()[index]->isEnabled());
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
        auto* colors = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditColorCombo"));
        const int color = colors->currentIndex();
        QTest::mouseClick(colors, Qt::LeftButton);
        colors->hidePopup();
        QCOMPARE(colors->currentIndex(), color);
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
        auto* colors = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditColorCombo"));
        auto* widths = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditWidthCombo"));
        for (int index = 0; index < 3; ++index)
        {
            colors->setCurrentIndex(index);
            widths->setCurrentIndex(index);
            stickerDrag(*sticker, {100, 100 + index * 30}, {220, 100 + index * 30});
            QCOMPARE(sticker->annotations().last().style.color, annotationColors()[index]);
            QCOMPARE(sticker->annotations().last().style.lineWidth, annotationLineWidths[index]);
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
    void coverOverlayToolbarFitsNarrowScreens_data()
    {
        QTest::addColumn<int>("screenWidth");
        for (int width : {320, 480, 559, 560, 639, 640, 720, 800})
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
        SelectionOverlay overlay(frame);
        overlay.show();
        drag(overlay, {100, 100}, {250, 200});
        auto* toolbar = overlay.findChild<QWidget*>(QStringLiteral("selectionToolbar"));
        auto* first = button(overlay, "rectangleToolButton");
        auto* cover = button(overlay, "coverToolButton");
        QVERIFY(toolbar && first && cover);
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
        QVERIFY(toolbar->rect().contains(cover->geometry()));
        QVERIFY(cover->isVisible());
        QCOMPARE(cover->y() == first->y(), screenWidth >= 640);
        QTest::mouseClick(cover, Qt::LeftButton);
        QVERIFY(overlay.activeTool() == AnnotationType::Cover);
        auto* mosaic = button(overlay, "mosaicToolButton");
        QVERIFY(mosaic && mosaic->isVisible());
        QVERIFY(toolbar->rect().contains(mosaic->geometry()));
        QCOMPARE(mosaic->y() == first->y(), screenWidth >= 640);
        QTest::mouseClick(mosaic, Qt::LeftButton);
        QVERIFY(overlay.activeTool() == AnnotationType::Mosaic);
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status->isVisible());
        QVERIFY(overlay.rect().contains(toolbar->geometry()));
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
        QTest::mouseClick(button(overlay, "rectangleToolButton"), Qt::LeftButton);
        drag(overlay, {110, 110}, {240, 220});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {125, 125});
        overlay.findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("敏感 ABC 123"));
        auto* cover = button(overlay, "coverToolButton");
        QVERIFY(cover && cover->isVisible());
        QCOMPARE(cover->text(), QStringLiteral("遮盖"));
        QTest::mouseClick(cover, Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(overlay.activeTool() == AnnotationType::Cover);
        auto* widths = overlay.findChild<QComboBox*>(QStringLiteral("annotationWidthCombo"));
        auto* sizes = overlay.findChild<QComboBox*>(QStringLiteral("annotationTextSizeCombo"));
        auto* colors = overlay.findChild<QComboBox*>(QStringLiteral("annotationColorCombo"));
        QVERIFY(!widths->isEnabled());
        QVERIFY(!sizes->isEnabled());
        QVERIFY(colors->isEnabled());
        QTest::mouseClick(cover, Qt::LeftButton);
        QVERIFY(!overlay.activeTool());
        QVERIFY(!cover->isChecked());
        QVERIFY(widths->isEnabled());
        QTest::mouseClick(cover, Qt::LeftButton);
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
        colors->setCurrentIndex(2);
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
        QVERIFY(cover && cover->isVisible());
        QCOMPARE(cover->text(), QStringLiteral("遮盖"));
        stickerClick(*sticker, "stickerEditCoverToolButton");
        QCOMPARE(sticker->annotations().size(), qsizetype(2));
        QVERIFY(sticker->activeTool() == AnnotationType::Cover);
        auto* widths = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditWidthCombo"));
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        auto* colors = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditColorCombo"));
        QVERIFY(!widths->isEnabled());
        QVERIFY(!sizes->isEnabled());
        QVERIFY(colors->isEnabled());
        stickerClick(*sticker, "stickerEditCoverToolButton");
        QVERIFY(!sticker->activeTool());
        QVERIFY(!cover->isChecked());
        QVERIFY(widths->isEnabled());
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
        colors->setCurrentIndex(2);
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
        QTest::mouseClick(button(overlay, "rectangleToolButton"), Qt::LeftButton);
        drag(overlay, {110, 110}, {240, 220});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {125, 125});
        overlay.findChild<AnnotationTextEdit*>()->setPlainText(QStringLiteral("敏感 ABC 123"));
        auto* mosaic = button(overlay, "mosaicToolButton");
        QVERIFY(mosaic && mosaic->isVisible());
        QCOMPARE(mosaic->text(), QStringLiteral("马赛克"));
        const auto notice = QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖");
        QCOMPARE(mosaic->toolTip(), notice);
        QCOMPARE(mosaic->accessibleName(), notice);
        QTest::mouseClick(mosaic, Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        QVERIFY(overlay.activeTool() == AnnotationType::Mosaic);
        auto* status = overlay.findChild<QLabel*>(QStringLiteral("outputStatus"));
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), notice);
        status->hide();
        status->setText(QStringLiteral("保留反馈"));
        auto* widths = overlay.findChild<QComboBox*>(QStringLiteral("annotationWidthCombo"));
        auto* sizes = overlay.findChild<QComboBox*>(QStringLiteral("annotationTextSizeCombo"));
        auto* colors = overlay.findChild<QComboBox*>(QStringLiteral("annotationColorCombo"));
        QVERIFY(!widths->isEnabled());
        QVERIFY(!sizes->isEnabled());
        QVERIFY(!colors->isEnabled());
        QTest::mouseClick(mosaic, Qt::LeftButton);
        QVERIFY(!overlay.activeTool());
        QVERIFY(!mosaic->isChecked());
        QVERIFY(widths->isEnabled());
        QVERIFY(colors->isEnabled());
        QTest::mouseClick(mosaic, Qt::LeftButton);
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
        colors->setCurrentIndex(2);
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
        QVERIFY(mosaic && mosaic->isVisible());
        QCOMPARE(mosaic->text(), QStringLiteral("马赛克"));
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
        auto* widths = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditWidthCombo"));
        auto* sizes = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditTextSizeCombo"));
        auto* colors = sticker->findChild<QComboBox*>(QStringLiteral("stickerEditColorCombo"));
        QVERIFY(!widths->isEnabled());
        QVERIFY(!sizes->isEnabled());
        QVERIFY(!colors->isEnabled());
        stickerClick(*sticker, "stickerEditMosaicToolButton");
        QVERIFY(!sticker->activeTool());
        QVERIFY(!mosaic->isChecked());
        QVERIFY(widths->isEnabled());
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
        colors->setCurrentIndex(2);
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
            auto* tool = button(overlay, names[index]);
            QVERIFY(tool && tool->isVisible());
            QCOMPARE(tool->text(), labels[index]);
            QVERIFY(tool->toolTip().contains(labels[index]));
            QTest::mouseClick(tool, Qt::LeftButton);
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
            QTest::mouseClick(tool, Qt::LeftButton);
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
        auto* colors = overlay.findChild<QComboBox*>(QStringLiteral("annotationColorCombo"));
        auto* widths = overlay.findChild<QComboBox*>(QStringLiteral("annotationWidthCombo"));
        auto* sizes = overlay.findChild<QComboBox*>(QStringLiteral("annotationTextSizeCombo"));
        auto* undo = button(overlay, "undoAnnotationButton");
        auto* redo = button(overlay, "redoAnnotationButton");
        QVERIFY(colors && widths && sizes && undo && redo);
        QCOMPARE(colors->count(), 3);
        QCOMPARE(widths->count(), 3);
        QCOMPARE(sizes->count(), 3);
        QVERIFY(!undo->isEnabled());
        QVERIFY(!redo->isEnabled());
        QTest::mouseClick(button(overlay, "lineToolButton"), Qt::LeftButton);
        for (int index = 0; index < 3; ++index)
        {
            colors->setCurrentIndex(index);
            widths->setCurrentIndex(index);
            drag(overlay, {120, 150 + index * 20}, {280, 150 + index * 20});
            QCOMPARE(overlay.annotations().last().style.color, annotationColors()[index]);
            QCOMPARE(overlay.annotations().last().style.lineWidth, annotationLineWidths[index]);
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
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
        QVERIFY(!widths->isEnabled());
        QVERIFY(sizes->isEnabled());
        sizes->setCurrentIndex(2);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, {120, 120});
        auto* editor = overlay.findChild<AnnotationTextEdit*>();
        editor->setPlainText(QStringLiteral("ABC"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(overlay.annotations().last().style.textSize, annotationTextSizes[2]);
    }
    void textImeAndEscapeStayInCurrentLayer()
    {
        using namespace waibusnap;
        SelectionOverlay overlay(annotationFrame());
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        overlay.show();
        overlay.activateWindow();
        drag(overlay, {100, 100}, {300, 250});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
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
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
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
        QTest::mouseClick(button(overlay, "lineToolButton"), Qt::LeftButton);
        QCOMPARE(overlay.annotations().size(), qsizetype(2));
        drag(overlay, {120, 160}, {280, 160});
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
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
        QTest::mouseClick(button(overlay, "textToolButton"), Qt::LeftButton);
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
        QTest::mouseClick(button(overlay, "lineToolButton"), Qt::LeftButton);
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
    void invalidQuickDirectoryFallsBackAndCancelKeepsImage_data()
    {
        QTest::addColumn<bool>("sticker");
        QTest::addColumn<int>("kind");
        for (bool sticker : {false, true})
            for (int kind : {0, 1, 2, 3})
                QTest::newRow(qPrintable(
                    QStringLiteral("%1-%2").arg(sticker ? "sticker" : "overlay").arg(kind)))
                    << sticker << kind;
    }
    void invalidQuickDirectoryFallsBackAndCancelKeepsImage()
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
        if (kind == 2)
        {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(
                QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
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
            QVERIFY(!window->saveImage());
            QVERIFY(!window->isSaved());
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
            QVERIFY(!overlay.isSelectionSaved());
            QVERIFY(overlay.isVisible());
            QCOMPARE(overlay.selection(), selection);
            QCOMPARE(finished.count(), 0);
        }
        if (kind == 2)
            QVERIFY(QFile::setPermissions(directory, QFileDevice::ReadOwner |
                                                         QFileDevice::WriteOwner |
                                                         QFileDevice::ExeOwner));
        QCOMPARE(panels, 1);
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
        controller.menu()->actions().at(1)->trigger();
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
        controller.menu()->actions().at(1)->trigger();
        QVERIFY(handled);
        QVERIFY(!window->saveImage());
        QCOMPARE(panels, 2);
        QVERIFY(window->isSaved());
        QVERIFY(AppSettings(options.settingsFile).loadSavePreferences().quickDirectory.isEmpty());
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
        QTest::mouseClick(button(overlay, "lineToolButton"), Qt::LeftButton);
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
