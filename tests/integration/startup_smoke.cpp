#include "app/app_settings.h"
#include "app/hotkey_rules.h"
#include "app/sticker_manager.h"
#include "core/sticker_geometry.h"
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
#include <QInputMethodEvent>
#include <QKeySequenceEdit>
#include <QLabel>
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
        StickerManager manager;
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
        StickerManager manager;
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
        QCOMPARE(sticker->actions().size(), qsizetype(6));
        QCOMPARE(sticker->actions().last()->text(), QStringLiteral("关闭贴图"));
        sticker->actions().last()->trigger();
        QCOMPARE(manager.count(), 0);
        QTRY_VERIFY(!sticker);
    }
    void managerRecoversOffscreenAndDestroysPendingClose()
    {
        using namespace waibusnap;
        StickerManager manager;
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
                               "arrowToolButton",     "freehandToolButton", "textToolButton"};
        const QString labels[] = {QStringLiteral("矩形"), QStringLiteral("椭圆"),
                                  QStringLiteral("直线"), QStringLiteral("箭头"),
                                  QStringLiteral("画笔"), QStringLiteral("文本")};
        for (int index = 0; index < 6; ++index)
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
