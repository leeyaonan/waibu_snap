#include "app/application_controller.h"
#include "ui/quick_gunner_resource_source.h"
#include <QComboBox>
#include <QFile>
#include <QFileSystemWatcher>
#include <QImageWriter>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
using namespace waibusnap;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static HistoryWindow* window(ApplicationController& c) { return c.historyWindow_; }
    static HistoryManager& history(ApplicationController& c) { return c.history_; }
    static void disableHistory(ApplicationController& c)
    {
        c.historyEnabled_ = false;
        c.historyWindow_->setHistoryEnabled(false);
        c.refreshStickerActions();
    }
};
}
namespace
{
QImage picture(QColor color = Qt::red)
{
    QImage image(120, 80, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}
template <class T> T* child(QObject* root, const char* name)
{
    auto* found = root->findChild<T*>(QLatin1String(name));
    if (!found)
        qFatal("缺少资源浏览器测试控件：%s", name);
    return found;
}
struct Fixture
{
    QTemporaryDir temp;
    QString directory = temp.filePath(QStringLiteral("快枪手"));
    QString stars = temp.filePath(QStringLiteral("配置/quick-gunner-stars.json"));
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    HistoryManager history{settings, temp.filePath(QStringLiteral("历史"))};
    HistoryWindowActions actions;
    std::unique_ptr<QuickGunnerResourceSource> source;
    QPointer<HistoryWindow> window;
    QStringList trashed;
    QString revealed;
    QImage copied, pinned;
    QString failPath;
    Fixture()
    {
        if (!temp.isValid() || !QDir().mkpath(directory))
            qFatal("临时快枪手目录创建失败。");
        actions.trash = [this](const QString& path)
        {
            if (path == failPath)
                return HistoryStatus{false, QStringLiteral("回收站失败替身")};
            trashed.append(path);
            return HistoryStatus{QFile::remove(path), {}};
        };
        actions.reveal = [this](const QString& path)
        {
            revealed = path;
            return HistoryStatus{true, {}};
        };
        actions.copyImage = [this](const QImage& image)
        {
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.pinImage = [this](const QImage& image)
        {
            pinned = image;
            return ImageOutputResult{true, {}};
        };
    }
    ~Fixture()
    {
        if (window)
            delete window;
    }
    QString write(const QString& name, QColor color = Qt::red)
    {
        const QString path = QDir(directory).filePath(name);
        if (!picture(color).save(path))
            qFatal("测试图片保存失败。");
        return QFileInfo(path).canonicalFilePath();
    }
    void createSource(bool watcher = true)
    {
        source = std::make_unique<QuickGunnerResourceSource>([this] { return directory; }, stars,
                                                             actions, nullptr, watcher);
    }
    HistoryWindow* open(bool enabled = true)
    {
        if (enabled && !history.initialize().success)
            qFatal("临时历史初始化失败。");
        if (!source)
            createSource();
        window = new HistoryWindow(history, settings, actions, source.get(), enabled);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }
    void close()
    {
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};
}
class QuickGunnerResourceTest final : public QObject
{
    Q_OBJECT
  private slots:
    void scanFlatImagesAndFields()
    {
        Fixture f;
        f.write(QStringLiteral("a.PNG"));
        f.write(QStringLiteral("b.jpeg"));
        f.write(QStringLiteral("c.jpg"));
        const bool webp = QImageWriter::supportedImageFormats().contains("webp");
        if (webp)
            f.write(QStringLiteral("d.webp"));
        QFile text(QDir(f.directory).filePath(QStringLiteral("readme.txt")));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("text");
        text.close();
        QVERIFY(QDir(f.directory).mkpath(QStringLiteral("子目录")));
        QVERIFY(picture().save(QDir(f.directory).filePath(QStringLiteral("子目录/inside.png"))));
        const QString outside = f.temp.filePath(QStringLiteral("outside.png"));
        QVERIFY(picture().save(outside));
        const auto link = QDir(f.directory).filePath(QStringLiteral("link.png"));
        // Windows 的 QFile::link 创建快捷方式；只有文件系统支持链接时核验链接过滤。
        const bool linked = QFile::link(outside, link);
        f.createSource();
        ResourceSource& api = *f.source;
        QCOMPARE(api.id(), QStringLiteral("quick-gunner"));
        QCOMPARE(api.displayName(), QStringLiteral("快枪手"));
        QVERIFY(api.visible());
        const auto caps = api.capabilities();
        QVERIFY(!caps.groups && !caps.cleanup && !caps.originalVariant);
        QVERIFY(caps.star && caps.deleteItem && caps.reveal && caps.clearAll);
        const auto snapshot = api.snapshot();
        QCOMPARE(snapshot.items.size(), webp ? 4 : 3);
        QCOMPARE(snapshot.groups.size(), 1);
        qint64 bytes = 0;
        for (const auto& item : snapshot.items)
        {
            QCOMPARE(item.id, QDir::cleanPath(item.absolutePath));
            QCOMPARE(item.sourceId, api.id());
            QVERIFY(item.groupId.isEmpty());
            QVERIFY(!item.managed && item.deletable && !item.hasOriginal);
            QCOMPARE(item.bytes, QFileInfo(item.id).size());
            QCOMPARE(item.createdAtMs, QFileInfo(item.id).lastModified().toMSecsSinceEpoch());
            bytes += item.bytes;
            const auto image = api.loadImage(item.id, {30, 30});
            QVERIFY2(image.success, qPrintable(image.error));
            QCOMPARE(image.image.size(), QSize(30, 20));
            QCOMPARE(image.image.devicePixelRatio(), qreal(1));
        }
        QCOMPARE(snapshot.usage.managedCount, 0);
        QCOMPARE(snapshot.usage.externalCount, snapshot.items.size());
        QCOMPARE(snapshot.usage.externalBytes, bytes);
        QVERIFY(!api.loadImage(outside).success);
        QVERIFY(!api.deleteItem(outside).success);
        if (linked)
        {
            QVERIFY(!api.loadImage(link).success);
            QVERIFY(!api.deleteItem(link).success);
        }
        QVERIFY(QFileInfo::exists(outside));
    }
    void starsRoundTripCorruptionPruningAndAtomicFailure()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        const auto b = f.write(QStringLiteral("b.png"));
        f.createSource();
        QVERIFY(f.source->setStarred(a, true).success);
        QCOMPARE(f.source->snapshot().usage.starredCount, 1);
        f.createSource();
        QVERIFY(f.source->snapshot().items[0].starred);
        QVERIFY(QFile::rename(a, QDir(f.directory).filePath(QStringLiteral("moved.png"))));
        f.source->refresh();
        QCOMPARE(f.source->snapshot().usage.starredCount, 0);
        QFile stars(f.stars);
        QVERIFY(stars.open(QIODevice::ReadOnly));
        QVERIFY(!stars.readAll().contains(a.toUtf8()));
        stars.close();
        QVERIFY(stars.open(QIODevice::WriteOnly));
        stars.write("{broken");
        stars.close();
        f.createSource();
        QCOMPARE(f.source->snapshot().usage.starredCount, 0);
        QVERIFY(f.source->setStarred(b, true).success);
        QVERIFY(f.source->setStarred(b, false).success);
        const auto files = QDir(f.directory).entryList(QDir::Files);
        QCOMPARE(files.size(), 2); // 用户目录没有 sidecar。
        f.source.reset();
        f.stars = f.directory; // 用目录模拟原子写失败，不能改变内存星标。
        f.createSource();
        QVERIFY(!f.source->setStarred(b, true).success);
        QCOMPARE(f.source->snapshot().usage.starredCount, 0);
    }
    void outputsRevealDeletionAndBounds()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        f.createSource();
        QVERIFY(f.source->outputImage(a, false, false).success);
        QVERIFY(f.source->outputImage(a, false, true).success);
        QCOMPARE(f.copied.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(f.pinned, f.copied);
        QVERIFY(!f.source->loadImage(a, {}, true).success);
        QVERIFY(f.source->revealItem(a).success);
        QCOMPARE(f.revealed, a);
        f.failPath = a;
        QVERIFY(!f.source->deleteItem(a).success);
        QVERIFY(QFileInfo::exists(a));
        f.failPath.clear();
        const auto oldDirectory = f.directory;
        f.directory = f.temp.filePath(QStringLiteral("更换目录"));
        QVERIFY(QDir().mkpath(f.directory));
        QVERIFY(!f.source->deleteItem(a).success); // provider 变更但尚未刷新也拒绝旧项。
        f.directory = oldDirectory;
        QVERIFY(f.source->deleteItem(a).success);
        QCOMPARE(f.trashed, QStringList{a});
        QVERIFY(f.source->snapshot().items.isEmpty());
    }
    void clearStopsOnFirstFailureAndNeverTouchesHistory()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        const auto b = f.write(QStringLiteral("b.png"));
        const auto c = f.write(QStringLiteral("c.png"));
        QVERIFY(f.history.initialize().success);
        const auto record =
            f.history.record(f.history.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(record.success);
        const auto current = f.history.currentGroupId();
        f.createSource();
        QVERIFY(f.source->setStarred(c, true).success);
        f.failPath = b;
        auto result = f.source->clearAll();
        QVERIFY(!result.success);
        QCOMPARE(result.processedCount, 1);
        QVERIFY(result.error.contains(QStringLiteral("1 张")));
        QCOMPARE(f.trashed, QStringList{a});
        QVERIFY(QFileInfo::exists(b) && QFileInfo::exists(c));
        f.failPath.clear();
        result = f.source->clearAll();
        QVERIFY(result.success);
        QCOMPARE(result.processedCount, 2);
        QVERIFY(f.source->snapshot().items.isEmpty());
        QVERIFY(QFileInfo::exists(record.item.absolutePath));
        QCOMPARE(f.history.currentGroupId(), current);
        QCOMPARE(f.history.totalUsage().managedCount, 1);
    }
    void replacementsAndDirectoryChangeStopDestructiveActions()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        const auto b = f.write(QStringLiteral("b.png"));
        f.createSource();
        const auto outside = f.temp.filePath(QStringLiteral("outside.png"));
        QVERIFY(picture().save(outside));
        QVERIFY(QFile::remove(a));
        if (QFile::link(outside, a))
        {
            QVERIFY(!f.source->deleteItem(a).success);
            QVERIFY(QFileInfo::exists(outside));
        }
        const auto oldDirectory = f.directory;
        const auto other = f.temp.filePath(QStringLiteral("另一个目录"));
        QVERIFY(QDir().mkpath(other));
        f.directory = other;
        const auto otherFile = f.write(QStringLiteral("other.png"));
        f.directory = oldDirectory;
        // 在第一次回收回调中切换 provider，后续条目必须停止，不能跨目录清空。
        f.write(QStringLiteral("c.png"));
        f.actions.trash = [&](const QString& path)
        {
            const bool removed = QFile::remove(path);
            f.directory = other;
            return HistoryStatus{removed, {}};
        };
        f.createSource();
        const auto result = f.source->clearAll();
        QVERIFY(!result.success);
        QCOMPARE(result.processedCount, 1);
        QVERIFY(!QFileInfo::exists(b));
        QVERIFY(QFileInfo::exists(otherFile));
        QVERIFY(QFileInfo::exists(outside));
        QVERIFY(QFileInfo::exists(QDir(oldDirectory).filePath(QStringLiteral("c.png"))));
    }
    void directoryErrorsAndConfiguration()
    {
        Fixture f;
        f.directory.clear();
        auto* w = f.open(false);
        QVERIFY(!f.source->visible());
        QVERIFY(!QFileInfo::exists(f.history.rootDirectory()));
        auto* combo = child<QComboBox>(w, "resourceSourceCombo");
        QCOMPARE(combo->count(), 1);
        QCOMPARE(combo->currentData().toString(), QStringLiteral("quick-gunner"));
        QSignalSpy configure(w, &HistoryWindow::directoryRequested);
        auto* button = child<QToolButton>(w, "resourceChooseDirectory");
        QVERIFY(button->isVisible());
        button->click();
        QCOMPARE(configure.count(), 1);
        f.directory = f.temp.filePath(QStringLiteral("missing"));
        f.source->directoryChanged();
        QVERIFY(!f.source->snapshot().error.isEmpty());
        QVERIFY(child<QLabel>(w, "historyNotice")->text().contains(QStringLiteral("不存在")));
    }
    void watcherChangesPreserveSelectionZoomAndFallback()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        f.write(QStringLiteral("b.png"));
        auto* w = f.open(false);
        auto* list = child<QListWidget>(w, "historyThumbnails");
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toString() == a)
                list->setCurrentRow(i);
        auto* preview = child<HistoryImageView>(w, "historyImageView");
        preview->actualSize();
        QSignalSpy changes(f.source.get(), &ResourceSource::changed);
        const auto c = f.write(QStringLiteral("c.png"));
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 3, 5000);
        QCOMPARE(w->selectedItemId(), a);
        QCOMPARE(preview->zoom(), qreal(1));
        QVERIFY(QFile::remove(c));
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 5000);
        QVERIFY(QFile::rename(a, QDir(f.directory).filePath(QStringLiteral("moved.png"))));
        QTRY_VERIFY_WITH_TIMEOUT(w->selectedItemId() != a, 5000);
        QCOMPARE(list->count(), 2);
        QVERIFY(!w->selectedItemId().isEmpty());
        QVERIFY(changes.count() >= 3);
        const auto count = changes.count();
        f.source->refresh();
        QCOMPARE(changes.count(), count); // 手动 refresh 不发布 changed。
    }
    void watcherUnavailableUsesPollingAndPauses()
    {
        Fixture f;
        f.createSource(false);
        f.source->setActive(true);
        QSignalSpy changes(f.source.get(), &ResourceSource::changed);
        f.write(QStringLiteral("a.png"));
        QTRY_COMPARE_WITH_TIMEOUT(f.source->snapshot().items.size(), 1, 4500);
        QCOMPARE(changes.count(), 1);
        f.source->setActive(false);
        f.write(QStringLiteral("b.png"));
        QTest::qWait(2150);
        QCOMPARE(f.source->snapshot().items.size(), 1);
        f.source->setActive(true);
        QCOMPARE(f.source->snapshot().items.size(), 2);
    }
    void missingDirectoryRecoversWithPolling()
    {
        Fixture f;
        f.directory = f.temp.filePath(QStringLiteral("稍后出现"));
        f.createSource();
        f.source->setActive(true);
        QVERIFY(!f.source->snapshot().error.isEmpty());
        QVERIFY(QDir().mkpath(f.directory));
        f.write(QStringLiteral("a.png"));
        QTRY_COMPARE_WITH_TIMEOUT(f.source->snapshot().items.size(), 1, 4500);
        QVERIFY(f.source->snapshot().error.isEmpty());
    }
    void sourceSwitchActionsStateAndPrivacy()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        auto* w = f.open();
        QVERIFY(
            f.history.record(f.history.beginSession(), picture(), HistorySource::Overlay).success);
        const auto current = f.history.currentGroupId();
        const QString external =
            QDir(f.history.currentGroup()->absolutePath).filePath(QStringLiteral("external.jpg"));
        QVERIFY(picture().save(external));
        w->refresh();
        auto* combo = child<QComboBox>(w, "resourceSourceCombo");
        QCOMPARE(combo->count(), 2);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("history"));
        QVERIFY(child<QComboBox>(w, "historyGroupCombo")->findData(QStringLiteral("new-group")) >=
                0);
        QVERIFY(!child<QToolButton>(w, "resourceClearAll")->isVisible());
        auto* list = child<QListWidget>(w, "historyThumbnails");
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->toolTip() == QStringLiteral("external.jpg"))
                list->setCurrentRow(i);
        auto* remove = child<QToolButton>(w, "historyDelete");
        QVERIFY(!remove->isEnabled());
        QCOMPARE(remove->toolTip(), QStringLiteral("请在文件管理器中操作"));
        const auto size = w->size();
        const auto split = child<QSplitter>(w, "historySplitter")->sizes();
        w->selectSource(QStringLiteral("quick-gunner"));
        QCOMPARE(w->selectedItemId(), a);
        QCOMPARE(w->size(), size);
        QCOMPARE(child<QSplitter>(w, "historySplitter")->sizes(), split);
        QCOMPARE(child<QComboBox>(w, "historyGroupCombo")->count(), 2);
        QCOMPARE(child<QListWidget>(w, "historyGroups")->count(), 2);
        QVERIFY(remove->isEnabled());
        QCOMPARE(remove->toolTip(), QStringLiteral("移入系统回收站"));
        QVERIFY(child<QToolButton>(w, "resourceClearAll")->isVisible());
        QCOMPARE(f.history.currentGroupId(), current);
        child<QToolButton>(w, "historyStar")->click();
        QCOMPARE(f.source->snapshot().usage.starredCount, 1);
        f.close();
        QCOMPARE(f.settings.loadHistoryWindowState().lastSource, QStringLiteral("quick-gunner"));
        w = f.open();
        QCOMPARE(w->selectedSourceId(), QStringLiteral("quick-gunner"));
        w->selectSource(QStringLiteral("history"));
        w->setHistoryEnabled(false);
        combo = child<QComboBox>(w, "resourceSourceCombo");
        QCOMPARE(combo->count(), 1);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("quick-gunner"));
        QCOMPARE(f.history.currentGroupId(), current);
    }
    void clearConfirmationIncludesFilteredStarsAndCancel()
    {
        Fixture f;
        const auto a = f.write(QStringLiteral("a.png"));
        f.write(QStringLiteral("b.png"));
        auto* w = f.open(false);
        QVERIFY(f.source->setStarred(a, true).success);
        child<QComboBox>(w, "historyGroupCombo")->setCurrentIndex(1);
        QCOMPARE(child<QListWidget>(w, "historyThumbnails")->count(), 1);
        auto* clear = child<QToolButton>(w, "resourceClearAll");
        bool sawConfirmation = false;
        QTimer::singleShot(
            0, w,
            [&]
            {
                auto* confirm = w->findChild<QMessageBox*>(QStringLiteral("resourceClearConfirm"));
                sawConfirmation =
                    confirm && confirm->defaultButton() == confirm->button(QMessageBox::Cancel);
                if (confirm)
                    confirm->done(QMessageBox::Cancel);
            });
        clear->click();
        QVERIFY(sawConfirmation);
        QCOMPARE(f.source->snapshot().items.size(), 2);
        QTimer::singleShot(0, w,
                           [&]
                           {
                               if (auto* confirm = w->findChild<QMessageBox*>(
                                       QStringLiteral("resourceClearConfirm")))
                                   confirm->done(QMessageBox::Yes);
                           });
        clear->click();
        QVERIFY(f.source->snapshot().items.isEmpty());
        QCOMPARE(f.trashed.size(), 2);
    }
    void historyScanExcludesQuickGunnerEvenWithOverlap()
    {
        Fixture f;
        QVERIFY(f.history.initialize().success);
        const auto record =
            f.history.record(f.history.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(record.success);
        QuickGunnerPreferences preferences;
        preferences.directory = f.history.currentGroup()->absolutePath;
        QVERIFY(f.settings.saveQuickGunnerPreferences(preferences).isEmpty());
        f.history.refresh();
        HistoryResourceSource history(f.history, f.actions);
        QVERIFY(history.snapshot().items.isEmpty());
        QCOMPARE(f.history.totalUsage().managedCount, 0);
        f.directory = preferences.directory;
        f.createSource();
        QCOMPARE(f.source->snapshot().items.size(), 1);
        QVERIFY(QFileInfo::exists(record.item.absolutePath));
        preferences.directory += QStringLiteral("-other");
        QVERIFY(f.settings.saveQuickGunnerPreferences(preferences).isEmpty());
        f.history.refresh();
        QCOMPARE(history.snapshot().items.size(), 1); // 目录前缀有边界。
    }
    void directorySwitchSignalsAndUnavailableActions()
    {
        Fixture f;
        const auto old = f.write(QStringLiteral("old.png"));
        f.createSource();
        auto* w = f.open(false);
        auto* list = child<QListWidget>(w, "historyThumbnails");
        QCOMPARE(list->count(), 1);
        f.directory = f.temp.filePath(QStringLiteral("第二目录"));
        QVERIFY(QDir().mkpath(f.directory));
        const auto next = f.write(QStringLiteral("next.png"));
        QSignalSpy switched(w, &HistoryWindow::sourceChanged);
        f.source->directoryChanged();
        QCOMPARE(list->count(), 1);
        QCOMPARE(w->selectedItemId(), next);
        QVERIFY(!f.source->deleteItem(old).success);
        QVERIFY(!f.source->setStarred(old, true).success);
        QuickGunnerResourceSource noActions([&] { return f.directory; }, f.stars);
        QVERIFY(!noActions.canCopy() && !noActions.canPin());
        QVERIFY(!noActions.deleteItem(next).success);
        QVERIFY(!noActions.revealItem(next).success);
        QVERIFY(!noActions.outputImage(next, false, false).success);
        QVERIFY(!noActions.clearAll().success);
        QVERIFY(QFileInfo::exists(next));
        QCOMPARE(switched.count(), 0);
    }
    void staleSourceStateFallsBackAndInvalidStateDefaults()
    {
        Fixture f;
        QCOMPARE(f.settings.loadHistoryWindowState().lastSource, QStringLiteral("history"));
        HistoryWindowState state;
        state.lastSource = QStringLiteral("history");
        QVERIFY(f.settings.saveHistoryWindowState(state).isEmpty());
        auto* w = f.open(false);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("quick-gunner"));
        f.close();
        w = f.open(false);
        QCOMPARE(child<QComboBox>(w, "resourceSourceCombo")->count(), 1);
        QCOMPARE(w->selectedSourceId(), QStringLiteral("quick-gunner"));
        f.close();
        QSettings settings(f.temp.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("history/window/lastSource"), QStringLiteral("invalid"));
        settings.sync();
        QCOMPARE(f.settings.loadHistoryWindowState().lastSource, QStringLiteral("history"));
        AppSettings failed(f.directory);
        QVERIFY(!failed.saveHistoryWindowState(state).isEmpty());
        QCOMPARE(failed.loadHistoryWindowState().lastSource, QStringLiteral("history"));
    }
    void borrowedSourceMayEndBeforeDeferredWindowDeletion()
    {
        Fixture f;
        f.write(QStringLiteral("a.png"));
        f.open(false);
        f.window->close();
        f.source.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(f.window.isNull());
    }
    void trayReusesWindowAcrossSourcesAndDisabledHistory()
    {
        Fixture f;
        f.write(QStringLiteral("a.png"));
        QuickGunnerPreferences preferences;
        preferences.directory = f.directory;
        QVERIFY(f.settings.saveQuickGunnerPreferences(preferences).isEmpty());
        RunOptions options;
        options.testMode = true;
        options.settingsFile = f.temp.filePath(QStringLiteral("settings.ini"));
        options.historyRoot = f.temp.filePath(QStringLiteral("历史"));
        ApplicationController c(*qApp, options, {}, {}, {}, f.actions);
        auto* open = child<QAction>(c.menu(), "quickGunnerOpenAction");
        QVERIFY(open->isEnabled());
        open->trigger();
        auto* window = ApplicationControllerTestAccess::window(c);
        QVERIFY(window);
        QCOMPARE(window->selectedSourceId(), QStringLiteral("quick-gunner"));
        c.openHistory();
        QCOMPARE(ApplicationControllerTestAccess::window(c), window);
        QCOMPARE(window->selectedSourceId(), QStringLiteral("history"));
        ApplicationControllerTestAccess::disableHistory(c);
        QVERIFY(!child<QAction>(c.menu(), "openHistoryAction")->isEnabled());
        QVERIFY(open->isEnabled());
        QCOMPARE(window->selectedSourceId(), QStringLiteral("quick-gunner"));
        open->trigger();
        QCOMPARE(ApplicationControllerTestAccess::window(c), window);
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!ApplicationControllerTestAccess::window(c));
        c.openHistory();
        window = ApplicationControllerTestAccess::window(c);
        QVERIFY(window);
        QCOMPARE(child<QComboBox>(window, "resourceSourceCombo")->count(), 1);
        QCOMPARE(window->selectedSourceId(), QStringLiteral("quick-gunner"));
    }
};
QTEST_MAIN(QuickGunnerResourceTest)
#include "quick_gunner_resource_test.moc"
