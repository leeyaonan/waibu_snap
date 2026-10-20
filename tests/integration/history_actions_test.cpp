#include "app/application_controller.h"
#include "core/sticker_geometry.h"
#include "interfaces/file_manager.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
using namespace waibusnap;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static HistoryManager& history(ApplicationController& c) { return c.history_; }
    static HistoryWindow* window(ApplicationController& c) { return c.historyWindow_; }
    static quint64 sequence(ApplicationController& c) { return c.historyStickerSequence_; }
    static void blockPin(ApplicationController& c, bool blocked) { c.active_ = blocked; }
};
}
namespace
{
QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
QImage picture(QSize size = {180, 110})
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(QColor(30, 80, 190, 120));
    image.setPixelColor(0, 0, Qt::red);
    image.setDevicePixelRatio(2);
    return image;
}
template <class T> T* child(QObject* root, const char* name)
{
    auto* value = root->findChild<T*>(QLatin1String(name));
    if (!value)
        qFatal("缺少历史动作控件：%s", name);
    return value;
}
class FakeFileManager final : public FileManager
{
  public:
    QString trashDirectory;
    QString revealed;
    int trashCalls = 0;
    int openCalls = 0;
    bool failTrash = false;
    bool failReveal = false;
    std::function<void()> afterTrash;
    FileManagerResult revealInFolder(const QString& path) override
    {
        revealed = path;
        return {!failReveal, failReveal ? QStringLiteral("定位失败替身") : QString()};
    }
    FileManagerResult openFolder(const QString&) override
    {
        ++openCalls;
        return {true, {}};
    }
    FileManagerResult moveToTrash(const QString& path) override
    {
        ++trashCalls;
        if (failTrash)
            return {false, QStringLiteral("回收站失败替身")};
        const auto target = QDir(trashDirectory).filePath(QFileInfo(path).fileName());
        if (!QFile::rename(path, target))
            return {false, QStringLiteral("临时回收目录移动失败")};
        if (afterTrash)
            afterTrash();
        return {true, {}};
    }
};
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings;
    HistoryManager manager;
    HistoryStore store;
    std::shared_ptr<FakeFileManager> files = std::make_shared<FakeFileManager>();
    HistoryWindowActions actions;
    QPointer<HistoryWindow> window;
    QImage copied, pinned;
    int copies = 0, pins = 0;
    bool copyFails = false, pinFails = false;
    Fixture()
        : settings(temp.filePath(QStringLiteral("settings.ini"))),
          manager(settings, temp.filePath(QStringLiteral("历史 根目录"))),
          store(temp.filePath(QStringLiteral("历史 根目录")))
    {
        if (!temp.isValid() || !manager.initialize().success)
            qFatal("临时历史库初始化失败，拒绝使用真实目录。");
        files->trashDirectory = temp.filePath(QStringLiteral("假回收站"));
        if (!QDir().mkdir(files->trashDirectory))
            qFatal("无法创建临时回收目录。");
        actions.copyImage = [this](const QImage& image)
        {
            ++copies;
            copied = image;
            return ImageOutputResult{!copyFails,
                                     copyFails ? QStringLiteral("复制失败替身") : QString()};
        };
        actions.pinImage = [this](const QImage& image)
        {
            ++pins;
            pinned = image;
            return ImageOutputResult{!pinFails,
                                     pinFails ? QStringLiteral("钉图失败替身") : QString()};
        };
        actions.trash = [this](const QString& path)
        {
            const auto r = files->moveToTrash(path);
            return HistoryStatus{r.success, r.error};
        };
        actions.reveal = [this](const QString& path)
        {
            const auto r = files->revealInFolder(path);
            return HistoryStatus{r.success, r.error};
        };
    }
    ~Fixture() { delete window.data(); }
    HistoryItem write(QImage image = picture())
    {
        const auto result =
            store.writeManagedPng(manager.currentGroupId(), image, HistorySource::Overlay);
        if (!result.success)
            qFatal("临时图片写入失败。");
        return result.item;
    }
    HistoryWindow* open()
    {
        window = new HistoryWindow(manager, settings, actions);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }
    QListWidget* list() { return child<QListWidget>(window, "historyThumbnails"); }
    QString selected()
    {
        return list()->currentItem() ? list()->currentItem()->data(Qt::UserRole).toString()
                                     : QString();
    }
    void click(const char* name) { child<QToolButton>(window, name)->click(); }
    QString notice() { return child<QLabel>(window, "historyNotice")->text(); }
    void view(const QString& view)
    {
        auto* combo = child<QComboBox>(window, "historyGroupCombo");
        combo->setCurrentIndex(combo->findData(view));
    }
    QString index()
    {
        return QDir(manager.currentGroup()->absolutePath)
            .filePath(QStringLiteral(".waibusnap/items.json"));
    }
    qsizetype count() { return store.scan().groups[0].items.size(); }
};
}
class HistoryActionsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void starRoundTripAndViews()
    {
        Fixture f;
        const auto item = f.write();
        auto* w = f.open();
        const auto current = f.manager.currentGroupId();
        const QByteArray original = bytes(item.absolutePath);
        f.click("historyStar");
        QVERIFY(f.manager.findItem(historyItemId(item))->starred);
        QCOMPARE(f.list()->currentItem()->checkState(), Qt::Checked);
        QVERIFY(child<QToolButton>(w, "historyStar")->isChecked());
        auto* groups = child<QListWidget>(w, "historyGroups");
        QVERIFY(groups->item(1)->text().contains(QStringLiteral("（1）")));
        QVERIFY(bytes(f.index()).contains("\"starred\": true"));
        f.view(QStringLiteral("starred"));
        QCOMPARE(f.list()->count(), 1);
        QTest::keyClick(child<HistoryImageView>(w, "historyImageView"), Qt::Key_S);
        QCOMPARE(f.list()->count(), 0);
        QVERIFY(groups->item(1)->text().contains(QStringLiteral("（0）")));
        f.view(QStringLiteral("all"));
        f.list()->currentItem()->setCheckState(Qt::Checked);
        QVERIFY(f.manager.findItem(historyItemId(item))->starred);
        QCOMPARE(f.selected(), historyItemId(item));
        f.list()->currentItem()->setCheckState(Qt::Unchecked);
        QVERIFY(!f.manager.findItem(historyItemId(item))->starred);
        QCOMPARE(bytes(item.absolutePath), original);
        QCOMPARE(f.count(), 1);
        QCOMPARE(f.manager.currentGroupId(), current);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, current);
    }
    void externalStarAndDeleteDisabled()
    {
        Fixture f;
        const auto path =
            QDir(f.manager.currentGroup()->absolutePath).filePath(QStringLiteral("外部 图片.png"));
        QVERIFY(picture().save(path));
        auto* w = f.open();
        const auto id = f.selected();
        QVERIFY(!f.manager.findItem(id)->managed);
        QVERIFY(!child<QToolButton>(w, "historyDelete")->isEnabled());
        QCOMPARE(child<QToolButton>(w, "historyDelete")->toolTip(),
                 QStringLiteral("请在文件管理器中操作"));
        f.click("historyDelete");
        QCOMPARE(f.files->trashCalls, 0);
        QTest::keyClick(f.list(), Qt::Key_S);
        QVERIFY(f.manager.findItem(id)->starred);
        f.view(QStringLiteral("starred"));
        QCOMPARE(f.list()->count(), 1);
        f.list()->currentItem()->setCheckState(Qt::Unchecked);
        QCOMPARE(f.list()->count(), 0);
        QVERIFY(!bytes(f.index()).contains(QFileInfo(path).fileName().toUtf8()));
        QCOMPARE(f.count(), 1);
    }
    void starFailureKeepsStateAndSelection()
    {
        Fixture f;
        const auto item = f.write();
        auto* w = f.open();
        const auto oldIndex = bytes(f.index());
        QLockFile lock(QDir(f.manager.rootDirectory()).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock());
        f.click("historyStar");
        QVERIFY(!child<QToolButton>(w, "historyStar")->isChecked());
        QCOMPARE(f.selected(), historyItemId(item));
        QVERIFY(!f.notice().isEmpty());
        f.list()->currentItem()->setCheckState(Qt::Checked);
        QCOMPARE(f.list()->currentItem()->checkState(), Qt::Unchecked);
        QCOMPARE(bytes(f.index()), oldIndex);
        QVERIFY(w->isVisible());
        lock.unlock();
        f.click("historyStar");
        QVERIFY(child<QToolButton>(w, "historyStar")->isChecked());
    }
    void copyOriginalAndFailure()
    {
        Fixture f;
        const QImage original = picture({703, 321});
        const auto item = f.write(original);
        auto* w = f.open();
        const auto oldIndex = bytes(f.index());
        f.click("historyCopy");
        QCOMPARE(f.copies, 1);
        QCOMPARE(f.copied.size(), original.size());
        QCOMPARE(f.copied.devicePixelRatio(), qreal(1));
        QCOMPARE(f.copied, QImage(item.absolutePath));
        QVERIFY(w->isVisible());
        f.copyFails = true;
        f.click("historyCopy");
        QCOMPARE(f.copies, 2);
        QCOMPARE(f.selected(), historyItemId(item));
        QVERIFY(f.notice().contains(QStringLiteral("复制失败")));
        QCOMPARE(f.count(), 1);
        QCOMPARE(bytes(f.index()), oldIndex);
    }
    void pinKeyboardLifecycle()
    {
        Fixture f;
        const auto item = f.write();
        auto* w = f.open();
        const auto oldIndex = bytes(f.index());
        f.pinFails = true;
        QTest::keyClick(w, Qt::Key_Return);
        QVERIFY(w->isVisible());
        QCOMPARE(f.selected(), historyItemId(item));
        QVERIFY(f.notice().contains(QStringLiteral("钉图失败")));
        f.pinFails = false;
        QTest::keyClick(f.list(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(f.pins, 2);
        QVERIFY(w->isVisible());
        QCOMPARE(f.pinned.size(), QSize(180, 110));
        QCOMPARE(f.pinned.devicePixelRatio(), qreal(1));
        QCOMPARE(f.pinned, QImage(item.absolutePath));
        QTest::keyClick(child<HistoryImageView>(w, "historyImageView"), Qt::Key_Enter);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(f.window.isNull());
        QCOMPARE(f.pins, 3);
        QCOMPARE(f.count(), 1);
        QCOMPARE(bytes(f.index()), oldIndex);
    }
    void pinToolbarClosesOnSuccess()
    {
        Fixture f;
        f.write();
        auto* w = f.open();
        f.pinFails = true;
        f.click("historyPin");
        QVERIFY(w->isVisible());
        f.pinFails = false;
        f.click("historyPin");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(f.window.isNull());
        QCOMPARE(f.pins, 2);
        QCOMPARE(f.count(), 1);
    }
    void deleteSuccessAndRefresh()
    {
        Fixture f;
        const auto item = f.write();
        auto* w = f.open();
        f.click("historyStar");
        f.view(QStringLiteral("starred"));
        f.click("historyDelete");
        QCOMPARE(f.files->trashCalls, 1);
        QVERIFY(!QFileInfo::exists(item.absolutePath));
        QVERIFY(QFileInfo::exists(QDir(f.files->trashDirectory).filePath(item.fileName)));
        QVERIFY(!bytes(f.index()).contains(item.fileName.toUtf8()));
        QCOMPARE(f.count(), 0);
        QCOMPARE(f.list()->count(), 0);
        QVERIFY(child<QListWidget>(w, "historyGroups")
                    ->item(1)
                    ->text()
                    .contains(QStringLiteral("（0）")));
        QVERIFY(!child<QToolButton>(w, "historyDelete")->isEnabled());
        QVERIFY(w->isVisible());
    }
    void deleteTrashFailure()
    {
        Fixture f;
        const auto item = f.write();
        auto* w = f.open();
        const auto index = bytes(f.index());
        const auto image = bytes(item.absolutePath);
        f.files->failTrash = true;
        f.click("historyDelete");
        QVERIFY(f.notice().contains(QStringLiteral("回收站失败")));
        QCOMPARE(bytes(item.absolutePath), image);
        QCOMPARE(bytes(f.index()), index);
        QCOMPARE(f.selected(), historyItemId(item));
        QCOMPARE(f.count(), 1);
        QVERIFY(w->isVisible());
    }
    void deleteIndexFailureBeforeTrash()
    {
        Fixture f;
        const auto item = f.write();
        f.open();
        const auto original = bytes(item.absolutePath);
        QFile index(f.index());
        QVERIFY(index.open(QIODevice::WriteOnly));
        QVERIFY(index.write("{broken") > 0);
        index.close();
        const auto broken = bytes(f.index());
        f.click("historyDelete");
        QCOMPARE(f.files->trashCalls, 0);
        QCOMPARE(bytes(item.absolutePath), original);
        QCOMPARE(bytes(f.index()), broken);
        QCOMPARE(f.selected(), historyItemId(item));
        QVERIFY(f.notice().contains(QStringLiteral("损坏")));
    }
    void deleteIndexCommitFailureRestoresImage()
    {
        Fixture f;
        const auto item = f.write();
        f.open();
        const auto original = bytes(item.absolutePath);
        const auto index = bytes(f.index());
        bool disrupted = false;
        f.files->afterTrash = [&]
        {
            // 移走暂存文件的父目录，使 QSaveFile 提交失败；旧索引原样复制回原路径。
            const auto meta = QFileInfo(f.index()).absolutePath();
            const auto moved = f.temp.filePath(QStringLiteral("原元数据"));
            disrupted = QDir().rename(meta, moved) && QDir().mkdir(meta) &&
                        QFile::copy(QDir(moved).filePath(QStringLiteral("group.json")),
                                    QDir(meta).filePath(QStringLiteral("group.json"))) &&
                        QFile::copy(QDir(moved).filePath(QStringLiteral("items.json")), f.index());
        };
        f.click("historyDelete");
        QVERIFY(disrupted);
        QVERIFY(f.notice().contains(QStringLiteral("索引提交失败")));
        QCOMPARE(bytes(item.absolutePath), original);
        QCOMPARE(bytes(f.index()), index);
        QCOMPARE(f.selected(), historyItemId(item));
        QCOMPARE(f.count(), 1);
    }
    void rejectExternalForgedPathsAndStaleIdentity()
    {
        Fixture f;
        const auto item = f.write();
        f.manager.refresh();
        HistoryItem forged = item;
        forged.absolutePath = f.temp.filePath(QStringLiteral("逃逸.png"));
        QVERIFY(picture().save(forged.absolutePath));
        QVERIFY(!f.store.deleteManagedItem(forged, f.actions.trash).success);
        forged = item;
        forged.fileName = QStringLiteral("../逃逸.png");
        QVERIFY(!f.store.deleteManagedItem(forged, f.actions.trash).success);
        forged = item;
        forged.managed = false;
        QVERIFY(!f.store.deleteManagedItem(forged, f.actions.trash).success);
        // 扫描后索引被改为外部身份，旧快照不得继续删除。
        auto json = QJsonDocument::fromJson(bytes(f.index())).object();
        auto items = json.value(QStringLiteral("items")).toObject();
        items[item.fileName] = QJsonObject{{QStringLiteral("managed"), false},
                                           {QStringLiteral("starred"), false},
                                           {QStringLiteral("source"), QStringLiteral("external")}};
        json[QStringLiteral("items")] = items;
        QFile index(f.index());
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write(QJsonDocument(json).toJson());
        index.close();
        QVERIFY(!f.manager.deleteItem(historyItemId(item), f.actions.trash).success);
        QVERIFY(QFileInfo::exists(item.absolutePath));
        QCOMPARE(f.files->trashCalls, 0);
    }
    void missingImageAndSymlinkProtection()
    {
        Fixture f;
        const auto item = f.write();
        f.open();
        QVERIFY(QFile::remove(item.absolutePath));
        f.click("historyCopy");
        QCOMPARE(f.copies, 0);
        QVERIFY(f.notice().contains(QStringLiteral("丢失")));
        f.click("historyPin");
        QCOMPARE(f.pins, 0);
        f.click("historyDelete");
        QCOMPARE(f.files->trashCalls, 0);
        f.click("historyReveal");
        QVERIFY(f.files->revealed.isEmpty());
        QCOMPARE(f.selected(), historyItemId(item));
#ifdef Q_OS_MACOS
        const auto outside = f.temp.filePath(QStringLiteral("外部源.png"));
        QVERIFY(picture().save(outside));
        QVERIFY(QFile::link(outside, item.absolutePath));
        QVERIFY(!f.store.deleteManagedItem(item, f.actions.trash).success);
        QVERIFY(!f.manager.loadImage(historyItemId(item)).success);
        QVERIFY(QFileInfo::exists(outside));
        QCOMPARE(f.files->trashCalls, 0);
#endif
    }
    void revealSuccessFailureAndExternal()
    {
        Fixture f;
        const auto path =
            QDir(f.manager.currentGroup()->absolutePath).filePath(QStringLiteral("外部 中文.jpg"));
        QVERIFY(picture().save(path));
        f.open();
        const auto id = f.selected();
        const auto index = bytes(f.index());
        f.click("historyReveal");
        QCOMPARE(f.files->revealed, path);
        f.files->failReveal = true;
        f.click("historyReveal");
        QVERIFY(f.notice().contains(QStringLiteral("定位失败")));
        QCOMPARE(f.selected(), id);
        QCOMPARE(bytes(f.index()), index);
        QCOMPARE(f.count(), 1);
        QCOMPARE(f.files->openCalls, 0);
    }
    void emptyAndNarrowControls()
    {
        Fixture f;
        auto* w = f.open();
        for (const auto* name :
             {"historyStar", "historyPin", "historyCopy", "historyReveal", "historyDelete"})
            QVERIFY(!child<QToolButton>(w, name)->isEnabled());
        f.write();
        w->refresh();
        w->resize(420, 600);
        QCoreApplication::processEvents();
        for (const auto* name :
             {"historyStar", "historyPin", "historyCopy", "historyReveal", "historyDelete"})
        {
            auto* button = child<QToolButton>(w, name);
            QVERIFY(button->isEnabled());
            QVERIFY(!button->toolTip().isEmpty());
            QVERIFY(w->rect().contains(button->geometry()));
        }
        QTest::keyClick(w, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!f.manager.findItem(f.selected())->starred);
    }
    void actionsKeepIngestErrorAndSession()
    {
        Fixture f;
        const auto session = f.manager.beginSession();
        const auto recorded = f.manager.record(session, picture(), HistorySource::Overlay);
        QVERIFY(recorded.success);
        f.open();
        QVERIFY(!f.manager.record({}, picture(), HistorySource::Overlay).success);
        const auto error = f.manager.error();
        QSignalSpy errors(&f.manager, &HistoryManager::errorChanged);
        f.click("historyStar");
        f.click("historyCopy");
        QTest::keyClick(f.window, Qt::Key_Return, Qt::ShiftModifier);
        f.click("historyReveal");
        QCOMPARE(f.manager.error(), error);
        QCOMPARE(errors.count(), 0);
        const auto repeated =
            f.manager.record(session, picture({200, 130}), HistorySource::Overlay);
        QVERIFY(repeated.success);
        QCOMPARE(repeated.item.fileName, recorded.item.fileName);
        QCOMPARE(f.count(), 1);
        QVERIFY(f.store.scan().groups[0].items[0].starred);
    }
    void pinGeometry_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("dpr1") << qreal(1);
        QTest::newRow("dpr1.5") << qreal(1.5);
        QTest::newRow("dpr2") << qreal(2);
    }
    void pinGeometry()
    {
        QFETCH(qreal, dpr);
        const QRect available(-1600, 30, 1000, 800);
        QCOMPARE(historyStickerScale({200, 120}, available, dpr), qreal(1));
        for (const QSize pixels : {QSize(4000, 200), QSize(400, 12000), QSize(4000, 2000)})
        {
            const auto scale = historyStickerScale(pixels, available, dpr);
            const auto size = stickerWindowSize(pixels, scale, dpr);
            QVERIFY(size.width() <= 800);
            QVERIFY(size.height() <= 640);
            QVERIFY(size.width() == 800 || size.height() == 640);
            const auto a = clipboardStickerPosition(available, size, 0);
            const auto b = clipboardStickerPosition(available, size, 1);
            QCOMPARE(b - a, QPoint(24, 24));
            QVERIFY(available.contains(QRect(b, size)));
        }
    }
    void controllerOutputsUseFakeServicesAndKeepOriginal()
    {
        Fixture f;
        RunOptions options;
        options.settingsFile = f.temp.filePath(QStringLiteral("控制器.ini"));
        options.historyRoot = f.temp.filePath(QStringLiteral("控制器历史"));
        StickerActions sticker;
        sticker.copyImage = f.actions.copyImage;
        // 历史窗口的 pin 保留生产路径，其他外部副作用都使用替身。
        HistoryWindowActions actions;
        actions.copyImage = f.actions.copyImage;
        ApplicationController controller(*qApp, options, sticker, {}, {}, actions, f.files);
        auto& manager = ApplicationControllerTestAccess::history(controller);
        const auto session = manager.beginSession();
        const auto recorded = manager.record(session, picture({400, 12000}), HistorySource::Scroll);
        QVERIFY(recorded.success);
        controller.openHistory();
        auto* w = ApplicationControllerTestAccess::window(controller);
        QVERIFY(w);
        auto* list = child<QListWidget>(w, "historyThumbnails");
        ApplicationControllerTestAccess::blockPin(controller, true);
        QTest::keyClick(list, Qt::Key_Return);
        QVERIFY(w->isVisible());
        QCOMPARE(controller.stickers().count(), 0);
        QCOMPARE(ApplicationControllerTestAccess::sequence(controller), quint64(0));
        ApplicationControllerTestAccess::blockPin(controller, false);
        child<QToolButton>(w, "historyCopy")->click();
        QCOMPARE(f.copies, 1);
        child<QToolButton>(w, "historyReveal")->click();
        QCOMPARE(f.files->revealed, recorded.item.absolutePath);
        QTest::keyClick(list, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClick(list, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(controller.stickers().count(), 2);
        const auto windows = controller.stickers().windows();
        const auto* screen = w->screen();
        QVERIFY(screen);
        const auto available = screen->availableGeometry();
        const auto expected =
            historyStickerScale({400, 12000}, available, screen->devicePixelRatio());
        for (const auto& window : windows)
        {
            QVERIFY(window);
            QCOMPARE(window->image(), QImage(recorded.item.absolutePath));
            QCOMPARE(window->image().devicePixelRatio(), qreal(1));
            QCOMPARE(window->scale(), expected);
            QVERIFY(window->height() <= qFloor(available.height() * 0.8));
            QVERIFY(window->width() <= qFloor(available.width() * 0.8));
            QVERIFY(window->isSaved());
        }
        QCOMPARE(windows[1]->pos(), clipboardStickerPosition(available, windows[1]->size(), 1));
        QCOMPARE(ApplicationControllerTestAccess::sequence(controller), quint64(2));
        QCOMPARE(manager.refresh().groups[0].items.size(), 1);
        // 贴图复制也只走替身，不会接入批次 2 的入库。
        child<QPushButton>(windows[0], "stickerCopyButton")->click();
        QCOMPARE(f.copies, 2);
        QCOMPARE(manager.refresh().groups[0].items.size(), 1);
        child<QToolButton>(w, "historyDelete")->click();
        QCOMPARE(f.files->trashCalls, 1);
        QCOMPARE(manager.refresh().groups[0].items.size(), 0);
    }
};
QTEST_MAIN(HistoryActionsTest)
#include "history_actions_test.moc"
