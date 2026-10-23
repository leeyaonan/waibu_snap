#include "app/application_controller.h"
#include "ui/history_window.h"
#include "ui/settings_dialog.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
using namespace waibusnap;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static HistoryWindow* window(ApplicationController& controller)
    {
        return controller.historyWindow_;
    }
    static void guard(ApplicationController& controller, int state)
    {
        controller.active_ = state == 1;
        controller.settingsOpen_ = state == 2;
        controller.resolvingQuit_ = state == 3;
        controller.quitting_ = state == 4;
        controller.refreshStickerActions();
    }
};
}
namespace
{
QImage picture(QColor color = Qt::red, QSize size = {160, 100})
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(color);
    image.setDevicePixelRatio(2);
    return image;
}
template <typename T> T* child(QObject* root, const char* name)
{
    auto* value = root->findChild<T*>(QLatin1String(name));
    if (!value)
        qFatal("缺少历史窗口控件：%s", name);
    return value;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings;
    HistoryManager manager;
    HistoryStore store;
    QPointer<HistoryWindow> window;
    Fixture()
        : settings(temp.filePath(QStringLiteral("settings.ini"))),
          manager(settings, temp.filePath(QStringLiteral("历史 根目录"))),
          store(temp.filePath(QStringLiteral("历史 根目录")))
    {
        if (!temp.isValid() || !manager.initialize().success)
            qFatal("临时历史库初始化失败，拒绝访问真实历史库。");
    }
    ~Fixture() { delete window.data(); }
    HistoryWindow* open()
    {
        window = new HistoryWindow(manager, settings);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }
    void close()
    {
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    HistoryItem write(const QString& group, const QImage& image,
                      HistorySource source = HistorySource::Overlay)
    {
        const auto result = store.writeManagedPng(group, image, source);
        if (!result.success)
            qFatal("临时历史图片写入失败。");
        QTest::qWait(2);
        return result.item;
    }
};
QListWidget* thumbnails(HistoryWindow* window)
{
    return child<QListWidget>(window, "historyThumbnails");
}
HistoryImageView* preview(HistoryWindow* window)
{
    return child<HistoryImageView>(window, "historyImageView");
}
QString selected(HistoryWindow* window)
{
    const auto* item = thumbnails(window)->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}
void choose(HistoryWindow* window, const QString& view)
{
    auto* combo = child<QComboBox>(window, "historyGroupCombo");
    combo->setCurrentIndex(combo->findData(view));
}
}
class HistoryWindowTest final : public QObject
{
    Q_OBJECT
  private slots:
    void emptyHistory()
    {
        Fixture f;
        auto* w = f.open();
        QVERIFY(w->isWindow());
        QVERIFY(!w->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(w->testAttribute(Qt::WA_DeleteOnClose));
        QCOMPARE(child<QListWidget>(w, "historyGroups")->count(), 4);
        QCOMPARE(thumbnails(w)->count(), 0);
        QVERIFY(
            child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("还没有截图历史")));
        QVERIFY(preview(w)->image().isNull());
        choose(w, f.manager.currentGroupId());
        QVERIFY(
            child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("此分组暂无记录")));
        choose(w, QStringLiteral("starred"));
        QVERIFY(child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("还没有星标")));
    }
    void originalVariantToggle()
    {
        Fixture f;
        const auto group = f.manager.currentGroup();
        QVERIFY(group);
        const auto watermarked = f.store.writeManagedPng(
            group->id, picture(Qt::red), HistorySource::Overlay, {}, picture(Qt::blue));
        QVERIFY2(watermarked.success, qPrintable(watermarked.error));
        QVERIFY(watermarked.item.hasOriginal);
        f.manager.refresh();
        auto* window = f.open();
        QCOMPARE(thumbnails(window)->count(), 1);
        thumbnails(window)->setCurrentRow(0);
        QCoreApplication::processEvents();
        auto* toggle = child<QToolButton>(window, "historyOriginalToggle");
        QVERIFY(toggle->isVisible());
        QVERIFY(!toggle->isChecked());
        QCOMPARE(preview(window)->image().pixelColor(0, 0), QColor(Qt::red));
        toggle->click();
        QCoreApplication::processEvents();
        QVERIFY(toggle->isChecked());
        QCOMPARE(toggle->text(), QStringLiteral("水印版"));
        QCOMPARE(preview(window)->image().pixelColor(0, 0), QColor(Qt::blue));
        toggle->click();
        QCoreApplication::processEvents();
        QCOMPARE(preview(window)->image().pixelColor(0, 0), QColor(Qt::red));
    }
    void filtersCountsAndCurrentGroup()
    {
        Fixture f;
        const QString current = f.manager.currentGroupId();
        const auto custom = f.store.createGroup(QStringLiteral("课程"));
        QVERIFY(custom.success);
        const auto a = f.write(current, picture());
        f.write(custom.group.id, picture(Qt::blue), HistorySource::Scroll);
        QVERIFY(f.store.setStarred(current, a.fileName, true).success);
        const QString external =
            QDir(custom.group.absolutePath).filePath(QStringLiteral("外部.jpg"));
        QVERIFY(picture(Qt::green).save(external));
        auto* w = f.open();
        auto* list = child<QListWidget>(w, "historyGroups");
        QCOMPARE(list->count(), 5);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("（3）")));
        QVERIFY(list->item(1)->text().contains(QStringLiteral("（1）")));
        QVERIFY(list->item(2)->text().startsWith(QStringLiteral("● ")));
        QVERIFY(list->item(2)->text().contains(QStringLiteral("默认分组")));
        QSignalSpy changed(&f.manager, &HistoryManager::currentGroupChanged);
        list->setCurrentRow(3);
        QCOMPARE(thumbnails(w)->count(), 2);
        QVERIFY(thumbnails(w)->item(0)->text().contains(QStringLiteral("长图")) ||
                thumbnails(w)->item(1)->text().contains(QStringLiteral("长图")));
        QVERIFY(thumbnails(w)->item(0)->text().contains(QStringLiteral("外部图片")) ||
                thumbnails(w)->item(1)->text().contains(QStringLiteral("外部图片")));
        choose(w, QStringLiteral("starred"));
        QCOMPARE(thumbnails(w)->count(), 1);
        QCOMPARE(thumbnails(w)->item(0)->toolTip(), a.fileName);
        QVERIFY(thumbnails(w)->item(0)->text().contains(QStringLiteral("★")));
        QCOMPARE(f.manager.currentGroupId(), current);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, current);
        QCOMPARE(changed.count(), 0);
        const auto snapshot = f.manager.snapshot();
        QCOMPARE(snapshot.groups.size(), 2);
        qsizetype count = 0;
        qint64 bytes = 0;
        for (const auto& group : snapshot.groups)
        {
            count += group.usage.managedCount + group.usage.externalCount;
            bytes += group.usage.managedBytes + group.usage.externalBytes;
        }
        QCOMPARE(count, 3);
        QVERIFY(bytes > 0);
    }
    void sortingSelectionAndKeyboard()
    {
        Fixture f;
        const QString group = f.manager.currentGroupId();
        const auto a = f.write(group, picture());
        const auto b = f.write(group, picture(Qt::blue));
        const auto c = f.write(group, picture(Qt::green));
        auto* w = f.open();
        QCOMPARE(selected(w), historyItemId(c));
        child<QComboBox>(w, "historySortCombo")->setCurrentIndex(1);
        QCOMPARE(thumbnails(w)->item(0)->data(Qt::UserRole).toString(), historyItemId(a));
        QCOMPARE(selected(w), historyItemId(c));
        choose(w, group);
        QCOMPARE(selected(w), historyItemId(a));
        QTest::keyClick(preview(w), Qt::Key_Right);
        QCOMPARE(selected(w), historyItemId(b));
        QTest::keyClick(thumbnails(w), Qt::Key_Down);
        QCOMPARE(selected(w), historyItemId(c));
        QTest::keyClick(w, Qt::Key_Left);
        QCOMPARE(selected(w), historyItemId(b));
        QTest::keyClick(w, Qt::Key_Up);
        QCOMPARE(selected(w), historyItemId(a));
        QCOMPARE(preview(w)->image().devicePixelRatio(), qreal(1));
        QCOMPARE(preview(w)->image().size(), QSize(160, 100));
        QCOMPARE(preview(w)->image().pixelColor(10, 10), QColor(Qt::red));
        QTest::keyClick(w, Qt::Key_Return);
        QTest::keyClick(w, Qt::Key_S);
        QVERIFY(w->isVisible());
        QVERIFY(f.store.scan().groups[0].items[0].starred);
        QTest::keyClick(preview(w), Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(f.window.isNull());
    }
    void lazyThumbnails()
    {
        Fixture f;
        const auto group = f.manager.currentGroup().value();
        const auto first = f.write(group.id, picture());
        for (int i = 0; i < 80; ++i)
            QVERIFY(QFile::copy(
                first.absolutePath,
                QDir(group.absolutePath).filePath(QStringLiteral("外部-%1.png").arg(i))));
        auto* w = f.open();
        auto* list = thumbnails(w);
        QCOMPARE(list->count(), 81);
        QTRY_VERIFY(!list->item(0)->icon().isNull());
        int icons = 0;
        for (int row = 0; row < list->count(); ++row)
            icons += !list->item(row)->icon().isNull();
        QVERIFY(icons < 15);
        QVERIFY(list->item(80)->icon().isNull());
        list->setCurrentRow(80);
        list->scrollToItem(list->item(80));
        QTRY_VERIFY(!list->item(80)->icon().isNull());
        QTRY_VERIFY(list->item(0)->icon().isNull());
    }
    void transparentCheckerboard()
    {
        Fixture f;
        f.write(f.manager.currentGroupId(), picture(Qt::transparent, {400, 400}));
        auto* w = f.open();
        auto* view = preview(w);
        QVERIFY(view->image().hasAlphaChannel());
        QCOMPARE(view->image().pixelColor(10, 10).alpha(), 0);
        const auto shot = view->viewport()->grab().toImage();
        const qreal dpr = shot.devicePixelRatio();
        const auto sample = [&](int x, int y)
        { return shot.pixelColor(qRound(x * dpr), qRound(y * dpr)); };
        QCOMPARE(sample(30, 30), QColor(205, 205, 205));
        QCOMPARE(sample(42, 30), QColor(238, 238, 238));
    }
    void longImageZoomScrollAndPan()
    {
        Fixture f;
        f.write(f.manager.currentGroupId(), picture(Qt::blue, {600, 4000}), HistorySource::Scroll);
        auto* w = f.open();
        auto* view = preview(w);
        QCoreApplication::processEvents();
        QVERIFY(view->zoom() < 1);
        child<QPushButton>(w, "historyActualSize")->click();
        QCOMPARE(view->zoom(), qreal(1));
        QVERIFY(view->verticalScrollBar()->maximum() > 2000);
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        QCOMPARE(view->verticalScrollBar()->value(), view->verticalScrollBar()->maximum());
        const QPointF center = view->viewport()->rect().center();
        QWheelEvent wheel(center, view->viewport()->mapToGlobal(center.toPoint()), {}, {0, 120},
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &wheel);
        QVERIFY(view->zoom() > 1);
        const int before = view->verticalScrollBar()->value();
        QTest::mousePress(view->viewport(), Qt::LeftButton, {}, center.toPoint());
        QMouseEvent move(QEvent::MouseMove, center + QPointF(0, 60),
                         view->viewport()->mapToGlobal((center + QPointF(0, 60)).toPoint()),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &move);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, {},
                            (center + QPointF(0, 60)).toPoint());
        QVERIFY(view->verticalScrollBar()->value() < before);
        const auto shot = view->viewport()->grab().toImage();
        const QPoint pixel = (center * shot.devicePixelRatio()).toPoint();
        QCOMPARE(shot.pixelColor(pixel), QColor(Qt::blue));
        child<QPushButton>(w, "historyFit")->click();
        QVERIFY(view->zoom() < 1);
    }
    void narrowLayoutAndCollapse()
    {
        Fixture f;
        const auto group = f.store.createGroup(QStringLiteral("空课程"));
        QVERIFY(group.success);
        auto* w = f.open();
        const auto* originalList = thumbnails(w);
        w->resize(640, 650);
        QCoreApplication::processEvents();
        QCOMPARE(child<QSplitter>(w, "historySplitter")->orientation(), Qt::Vertical);
        QCOMPARE(child<QSplitter>(w, "historyImageSplitter")->orientation(), Qt::Vertical);
        QCOMPARE(thumbnails(w)->flow(), QListView::LeftToRight);
        child<QToolButton>(w, "historyToggleGroups")->click();
        QVERIFY(!child<QListWidget>(w, "historyGroups")->isVisible());
        QVERIFY(child<QComboBox>(w, "historyGroupCombo")->isVisible());
        choose(w, group.group.id);
        QVERIFY(child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("此分组")));
        w->resize(1100, 700);
        QCoreApplication::processEvents();
        QCOMPARE(child<QSplitter>(w, "historySplitter")->orientation(), Qt::Horizontal);
        QCOMPARE(child<QSplitter>(w, "historyImageSplitter")->orientation(), Qt::Horizontal);
        QCOMPARE(thumbnails(w), originalList);
        QVERIFY(!child<QListWidget>(w, "historyGroups")->isVisible());
    }
    void restoreStateAndSelection()
    {
        Fixture f;
        const auto group = f.store.createGroup(QStringLiteral("会议"));
        QVERIFY(group.success);
        f.write(group.group.id, picture());
        const auto selectedItem = f.write(group.group.id, picture(Qt::green));
        auto* w = f.open();
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        const QSize requested(std::max(420, std::min(760, available.width() - 40)),
                              std::max(360, std::min(620, available.height() - 40)));
        w->resize(requested);
        QCoreApplication::processEvents();
        choose(w, group.group.id);
        child<QComboBox>(w, "historySortCombo")->setCurrentIndex(0);
        thumbnails(w)->setCurrentRow(0);
        QCOMPARE(selected(w), historyItemId(selectedItem));
        child<QToolButton>(w, "historyToggleGroups")->click();
        QCoreApplication::processEvents();
        child<QSplitter>(w, "historyImageSplitter")->setSizes({160, 350});
        QCoreApplication::processEvents();
        const auto split = child<QSplitter>(w, "historyImageSplitter")->sizes();
        f.close();
        QVERIFY(f.window.isNull());
        const auto state = f.settings.loadHistoryWindowState();
        QCOMPARE(state.lastView, group.group.id);
        QCOMPARE(state.lastSelection, historyItemId(selectedItem));
        QCOMPARE(state.sortOrder, QStringLiteral("newest"));
        w = f.open();
        QCOMPARE(w->size(), requested);
        QCOMPARE(thumbnails(w)->flow(), QListView::LeftToRight);
        QCOMPARE(child<QComboBox>(w, "historyGroupCombo")->currentData().toString(),
                 group.group.id);
        QCOMPARE(child<QComboBox>(w, "historySortCombo")->currentIndex(), 0);
        QCOMPARE(selected(w), historyItemId(selectedItem));
        QVERIFY(!child<QListWidget>(w, "historyGroups")->isVisible());
        QCOMPARE(child<QSplitter>(w, "historyImageSplitter")->sizes(), split);
        QCOMPARE(preview(w)->image().pixelColor(10, 10), QColor(Qt::green));
    }
    void offscreenGeometryAndStaleSelection()
    {
        Fixture f;
        QWidget outside;
        outside.resize(650, 500);
        outside.move(-50000, -50000);
        HistoryWindowState state;
        state.geometry = outside.saveGeometry();
        state.lastView = QStringLiteral("11111111-1111-1111-1111-111111111111");
        state.lastSelection = QStringLiteral("已丢失记录");
        state.splitterState = QByteArrayLiteral("损坏状态");
        QVERIFY(f.settings.saveHistoryWindowState(state).isEmpty());
        auto* w = f.open();
        bool intersects = false;
        for (auto* screen : QGuiApplication::screens())
            intersects |= screen->availableGeometry().intersects(w->frameGeometry());
        QVERIFY(intersects);
        QCOMPARE(child<QComboBox>(w, "historyGroupCombo")->currentData().toString(),
                 QStringLiteral("all"));
        QCOMPARE(thumbnails(w)->count(), 0);
    }
    void missingImageAndRefresh()
    {
        Fixture f;
        const auto a = f.write(f.manager.currentGroupId(), picture());
        const auto b = f.write(f.manager.currentGroupId(), picture(Qt::green));
        auto* w = f.open();
        QCOMPARE(thumbnails(w)->count(), 2);
        QVERIFY(QFile::remove(a.absolutePath));
        thumbnails(w)->setCurrentRow(1);
        QVERIFY(child<QLabel>(w, "historyEmpty")->isVisible());
        QVERIFY(child<QLabel>(w, "historyEmpty")->text().contains(QStringLiteral("丢失")));
        QVERIFY(preview(w)->image().isNull());
        child<QPushButton>(w, "historyRefresh")->click();
        QCOMPARE(thumbnails(w)->count(), 1);
        QCOMPARE(selected(w), historyItemId(b));
        f.write(f.manager.currentGroupId(), picture(Qt::blue));
        w->refresh();
        QCOMPARE(thumbnails(w)->count(), 2);
        QCOMPARE(selected(w), historyItemId(b));
    }
    void refreshPreservesIngestState()
    {
        Fixture f;
        const auto session = f.manager.beginSession();
        QVERIFY(f.manager.record(session, picture(), HistorySource::Overlay).success);
        QLockFile lock(QDir(f.manager.rootDirectory()).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock(0));
        QVERIFY(
            !f.manager.record(f.manager.beginSession(), picture(), HistorySource::Overlay).success);
        const QString error = f.manager.error();
        const auto bad = f.manager.refresh();
        QVERIFY(!bad.error.isEmpty());
        QCOMPARE(f.manager.error(), error);
        lock.unlock();
        f.manager.refresh();
        QCOMPARE(f.manager.error(), error);
        QCOMPARE(f.manager.currentGroupId(), session.groupId);
        QVERIFY(f.manager.record(session, picture(Qt::blue), HistorySource::Overlay).success);
        QCOMPARE(f.manager.refresh().groups[0].items.size(), 1);
        auto* w = f.open();
        const auto invalid = f.manager.loadImage(QStringLiteral("../../外部"));
        QVERIFY(!invalid.success);
        QVERIFY(invalid.image.isNull());
        QCOMPARE(thumbnails(w)->count(), 1);
    }
    void corruptedMetadataAndUnavailableRoot()
    {
        Fixture f;
        f.write(f.manager.currentGroupId(), picture());
        const auto group = f.manager.currentGroup().value();
        QFile index(QDir(group.absolutePath).filePath(QStringLiteral(".waibusnap/items.json")));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("{broken");
        index.close();
        auto* w = f.open();
        QVERIFY(child<QLabel>(w, "historyNotice")->isVisible());
        QVERIFY(child<QLabel>(w, "historyNotice")->text().contains(QStringLiteral("损坏")));
        QCOMPARE(thumbnails(w)->count(), 1);
        QVERIFY(!preview(w)->image().isNull());
        QVERIFY(QDir(f.manager.rootDirectory()).removeRecursively());
        w->refresh();
        QCOMPARE(thumbnails(w)->count(), 0);
        QVERIFY(child<QLabel>(w, "historyNotice")->isVisible());
    }
    void settingsIsolationFailureAndDefaults()
    {
        Fixture f;
        const auto original = f.settings.loadHistoryPreferences();
        HistoryWindowState state;
        state.lastView = QStringLiteral("starred");
        state.lastSelection = QStringLiteral("稳定标识");
        state.sortOrder = QStringLiteral("oldest");
        QVERIFY(f.settings.saveHistoryWindowState(state).isEmpty());
        QCOMPARE(f.settings.loadHistoryPreferences().rootDirectory, original.rootDirectory);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, original.currentGroupId);
        QCOMPARE(f.settings.loadHistoryPreferences().defaultCleanup.maxCount,
                 original.defaultCleanup.maxCount);
        QSettings ini(f.temp.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        ini.setValue(QStringLiteral("unknown/kept"), QStringLiteral("保留"));
        ini.setValue(QStringLiteral("history/window/lastView"), QStringLiteral("无效分组"));
        ini.setValue(QStringLiteral("history/window/sortOrder"), QStringLiteral("损坏排序"));
        ini.setValue(QStringLiteral("history/window/geometry"), QStringLiteral("损坏字节"));
        ini.sync();
        const auto fallback = f.settings.loadHistoryWindowState();
        QCOMPARE(fallback.lastView, QStringLiteral("all"));
        QVERIFY(fallback.sortOrder.isEmpty());
        QVERIFY(fallback.geometry.isEmpty());
        QVERIFY(f.settings.saveHistoryWindowState(state).isEmpty());
        QCOMPARE(ini.value(QStringLiteral("unknown/kept")).toString(), QStringLiteral("保留"));
        const QString blocked = f.temp.filePath(QStringLiteral("blocked"));
        QVERIFY(QDir().mkpath(blocked));
        AppSettings failed(blocked);
        QVERIFY(!failed.saveHistoryWindowState(state).isEmpty());
        QCOMPARE(failed.loadHistoryWindowState().lastView, QStringLiteral("all"));
        f.window = new HistoryWindow(f.manager, failed);
        f.window->show();
        f.close();
        QVERIFY(f.window.isNull());
        QVERIFY(QDir().rmdir(blocked));
        QVERIFY(failed.saveHistoryWindowState(state).isEmpty());
        QCOMPARE(failed.loadHistoryWindowState().lastView, QStringLiteral("starred"));
    }
    void controllerLazyLifecycleAndGuards()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        RunOptions options;
        options.historyRoot = temp.filePath(QStringLiteral("历史 根目录"));
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        int clipboardReads = 0;
        ApplicationController controller(*qApp, options, {},
                                         {[&]
                                          {
                                              ++clipboardReads;
                                              return QImage();
                                          }});
        auto* action = child<QAction>(controller.menu(), "openHistoryAction");
        QCOMPARE(action->text(), QStringLiteral("打开历史…"));
        QVERIFY(action->isEnabled());
        QVERIFY(!ApplicationControllerTestAccess::window(controller));
        for (int guard = 1; guard <= 4; ++guard)
        {
            ApplicationControllerTestAccess::guard(controller, guard);
            QVERIFY(!action->isEnabled());
            controller.openHistory();
            QVERIFY(!ApplicationControllerTestAccess::window(controller));
        }
        ApplicationControllerTestAccess::guard(controller, 0);
        QVERIFY(action->isEnabled());
        action->trigger();
        QPointer<HistoryWindow> first = ApplicationControllerTestAccess::window(controller);
        QVERIFY(first);
        QVERIFY(first->isVisible());
        HistoryStore store(options.historyRoot);
        const auto initial = store.scan();
        QVERIFY(store
                    .writeManagedPng(initial.groups[0].group.id, picture(Qt::blue),
                                     HistorySource::Overlay)
                    .success);
        first->hide();
        action->trigger();
        QCOMPARE(ApplicationControllerTestAccess::window(controller), first.data());
        QVERIFY(first->isVisible());
        QCOMPARE(thumbnails(first)->count(), 1);
        first->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(first.isNull());
        QVERIFY(!ApplicationControllerTestAccess::window(controller));
        action->trigger();
        QVERIFY(ApplicationControllerTestAccess::window(controller));
        QCOMPARE(clipboardReads, 0);
        const auto scan = HistoryStore(options.historyRoot).scan();
        QCOMPARE(scan.groups.size(), 1);
        QCOMPARE(scan.groups[0].items.size(), 1);
    }
    void controllerSettingsModalGuard()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        RunOptions options;
        options.historyRoot = temp.filePath(QStringLiteral("history"));
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        AutostartActions autostart;
        autostart.query = [] { return AutostartState{}; };
        autostart.setEnabled = [](bool) { return QString(); };
        ApplicationController controller(*qApp, options, {}, {[] { return QImage(); }}, autostart);
        auto* history = child<QAction>(controller.menu(), "openHistoryAction");
        bool guarded = false;
        QTimer::singleShot(0, &controller,
                           [&]
                           {
                               auto* dialog =
                                   qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                               guarded = dialog && !history->isEnabled();
                               controller.openHistory();
                               guarded &= !ApplicationControllerTestAccess::window(controller);
                               if (dialog)
                                   dialog->reject();
                           });
        child<QAction>(controller.menu(), "settingsAction")->trigger();
        QVERIFY(guarded);
        QVERIFY(history->isEnabled());
        QVERIFY(!ApplicationControllerTestAccess::window(controller));
    }
};
QTEST_MAIN(HistoryWindowTest)
#include "history_window_test.moc"
