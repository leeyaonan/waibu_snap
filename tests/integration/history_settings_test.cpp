#include "app/application_controller.h"
#include "app/hotkey_rules.h"
#include "ui/settings_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
using namespace waibusnap;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static HistoryManager& history(ApplicationController& c) { return c.history_; }
    static HistoryWindow* window(ApplicationController& c) { return c.historyWindow_; }
    static void cleanup(ApplicationController& c) { c.cleanup(); }
    static void initializeHotkeys(ApplicationController& c)
    {
        c.hotkeySettings_.initialize();
        for (auto& key : c.historyHotkeys_)
            key->initialize();
    }
    static QString change(ApplicationController& c, HotkeyAction action, QKeySequence key)
    {
        return c.changeHotkey(key, action);
    }
    static void guard(ApplicationController& c, int state)
    {
        c.active_ = state == 1;
        c.settingsOpen_ = state == 2;
        c.resolvingQuit_ = state == 3;
        c.quitting_ = state == 4;
        c.refreshStickerActions();
    }
};
}
namespace
{
template <class T> T* child(QObject* root, const char* name)
{
    auto* result = root->findChild<T*>(QLatin1String(name));
    if (!result)
        qFatal("缺少历史设置测试控件：%s", name);
    return result;
}
class FakeHotkey final : public GlobalHotkey
{
  public:
    std::array<QKeySequence, 4> keys;
    std::array<std::function<void()>, 4> handlers;
    QString rejected;
    QVector<HotkeyAction> attempts;
    QVector<HotkeyAction> released;
    HotkeyRegistration registerHotkey(HotkeyAction action, const QKeySequence& sequence,
                                      std::function<void()> handler) override
    {
        attempts.append(action);
        if (rejected == sequence.toString(QKeySequence::PortableText))
            return {false, QStringLiteral("注册失败替身，原绑定保留。")};
        for (size_t i = 0; i < keys.size(); ++i)
            if (i != size_t(action) && !keys[i].isEmpty() && keys[i] == sequence)
                return {false, QStringLiteral("动作重复，原绑定保留。")};
        keys[size_t(action)] = sequence;
        handlers[size_t(action)] = std::move(handler);
        return {true, QStringLiteral("替身已启用")};
    }
    void unregister(HotkeyAction action) override
    {
        keys[size_t(action)] = {};
        handlers[size_t(action)] = {};
        released.append(action);
    }
    void fire(HotkeyAction action)
    {
        const auto callback = handlers[size_t(action)];
        if (callback)
            callback();
    }
};
AutostartActions startup()
{
    return {[] { return AutostartState{}; }, [](bool) { return QString(); }};
}
QImage image()
{
    QImage value(80, 60, QImage::Format_ARGB32);
    value.fill(Qt::green);
    return value;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings;
    HistoryManager manager;
    Fixture()
        : settings(temp.filePath(QStringLiteral("设置.ini"))),
          manager(settings, temp.filePath(QStringLiteral("旧 历史")))
    {
        if (!temp.isValid() || !manager.initialize().success)
            qFatal("临时历史初始化失败，拒绝使用真实目录。");
    }
    HistorySettingsActions actions()
    {
        HistorySettingsActions result;
        result.preferences = settings.loadHistoryPreferences();
        result.preferences.rootDirectory = manager.rootDirectory();
        result.usage = manager.totalUsage();
        result.savePreferences = [this](const HistoryPreferences& p)
        { return manager.savePreferences(p).error; };
        result.chooseDirectory = [this](QWidget*, const QString&)
        { return temp.filePath(QStringLiteral("新 历史")); };
        return result;
    }
    HistoryWriteResult add(HistoryManager* target = nullptr)
    {
        auto& m = target ? *target : manager;
        const auto value = m.record(m.beginSession(), image(), HistorySource::Overlay);
        if (!value.success)
            qFatal("临时历史入库失败。");
        m.refresh();
        return value;
    }
};
void save(SettingsDialog& dialog) { child<QPushButton>(&dialog, "saveSettingsButton")->click(); }
void historyPage(SettingsDialog& dialog)
{
    dialog.show();
    child<QTabWidget>(&dialog, "settingsTabs")->setCurrentIndex(4);
    QApplication::processEvents();
}
RunOptions options(Fixture& f)
{
    RunOptions value;
    value.testMode = true;
    value.settingsFile = f.temp.filePath(QStringLiteral("控制器.ini"));
    value.historyRoot = f.temp.filePath(QStringLiteral("控制器历史"));
    return value;
}
}
class HistorySettingsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void pageControlsAndUsage()
    {
        Fixture f;
        const auto item = f.add();
        QVERIFY(f.manager.setStarred(historyItemId(item.item), true).success);
        const auto group = f.manager.currentGroup();
        QVERIFY(image().save(QDir(group->absolutePath).filePath(QStringLiteral("外部.png"))));
        const auto usage = f.manager.totalUsage();
        QCOMPARE(usage.managedCount, 1);
        QCOMPARE(usage.externalCount, 1);
        QCOMPARE(usage.protectedCount, 1);
        QVERIFY(usage.managedBytes > 0 && usage.externalBytes > 0 && usage.protectedBytes > 0);
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), f.actions());
        historyPage(dialog);
        auto* tabs = child<QTabWidget>(&dialog, "settingsTabs");
        QCOMPARE(tabs->count(), 5);
        QCOMPARE(tabs->widget(4)->objectName(), QStringLiteral("settingsHistoryPage"));
        QVERIFY(child<QLineEdit>(&dialog, "historyRootDirectory")->isReadOnly());
        QCOMPARE(child<QSpinBox>(&dialog, "historyMaxAgeDays")->value(), 30);
        QCOMPARE(child<QSpinBox>(&dialog, "historyMaxCount")->value(), 100);
        QCOMPARE(child<QDoubleSpinBox>(&dialog, "historyMaxMiB")->value(), 100.0);
        const auto text = child<QLabel>(&dialog, "historyTotalUsage")->text();
        QVERIFY(text.contains(QStringLiteral("受管：1 张")));
        QVERIFY(text.contains(QStringLiteral("外部：1 张")));
        QVERIFY(text.contains(QStringLiteral("受保护：1 张")));
        for (const auto* id :
             {"historyChooseDirectory", "historyClearDirectory", "historyPreviousHotkey",
              "historyNextHotkey", "historyOpenHotkey", "historyTotalUsage"})
        {
            auto* control = child<QWidget>(&dialog, id);
            QVERIFY(control->isVisible());
            QVERIFY(!control->visibleRegion().isEmpty());
            QVERIFY(
                dialog.rect().contains(QRect(control->mapTo(&dialog, QPoint()), control->size())));
        }
    }
    void directoryChooseClearCancelAndEscape_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("cancel") << 0;
        QTest::newRow("escape-in-history-editor") << 1;
    }
    void directoryChooseClearCancelAndEscape()
    {
        QFETCH(int, mode);
        Fixture f;
        const auto root = f.manager.rootDirectory();
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), f.actions());
        historyPage(dialog);
        child<QPushButton>(&dialog, "historyClearDirectory")->click();
        QVERIFY(child<QLineEdit>(&dialog, "historyRootDirectory")->text().isEmpty());
        child<QPushButton>(&dialog, "historyChooseDirectory")->click();
        child<QKeySequenceEdit>(&dialog, "historyOpenHotkey")
            ->setKeySequence(QKeySequence(Qt::Key_F10));
        QCOMPARE(child<QLineEdit>(&dialog, "historyRootDirectory")->text(),
                 f.temp.filePath(QStringLiteral("新 历史")));
        if (mode == 0)
            child<QPushButton>(&dialog, "cancelSettingsButton")->click();
        else
            QTest::keyClick(child<QKeySequenceEdit>(&dialog, "historyOpenHotkey"), Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(f.manager.rootDirectory(), root);
        QVERIFY(!QFileInfo::exists(f.temp.filePath(QStringLiteral("新 历史"))));
    }
    void rootSwitchAndNoMigration()
    {
        Fixture f;
        const auto old = f.add();
        const auto root = f.manager.rootDirectory();
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), f.actions());
        historyPage(dialog);
        child<QPushButton>(&dialog, "historyChooseDirectory")->click();
        save(dialog);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(f.manager.rootDirectory() != root);
        QVERIFY(QFileInfo::exists(old.item.absolutePath));
        QCOMPARE(f.manager.totalUsage().managedCount, 0);
        QCOMPARE(f.settings.loadHistoryPreferences().rootDirectory, f.manager.rootDirectory());
        f.add();
        HistoryManager restarted(f.settings);
        QVERIFY(restarted.initialize().success);
        QCOMPARE(restarted.totalUsage().managedCount, 1);
    }
    void rootFailureKeepsOldAndRetry()
    {
        Fixture f;
        const auto session = f.manager.beginSession();
        const auto old = f.manager.record(session, image(), HistorySource::Overlay);
        QVERIFY(old.success);
        const auto root = f.manager.rootDirectory();
        const auto group = f.manager.currentGroupId();
        QFile blocker(f.temp.filePath(QStringLiteral("阻塞文件")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        auto actions = f.actions();
        QString chosen = blocker.fileName();
        actions.chooseDirectory = [&](QWidget*, const QString&) { return chosen; };
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), actions);
        historyPage(dialog);
        child<QPushButton>(&dialog, "historyChooseDirectory")->click();
        save(dialog);
        QVERIFY(dialog.isVisible());
        QVERIFY(child<QLabel>(&dialog, "settingsStatus")
                    ->text()
                    .contains(QStringLiteral("旧目录保留")));
        QCOMPARE(f.manager.rootDirectory(), root);
        QCOMPARE(f.manager.currentGroupId(), group);
        const auto repeated = f.manager.record(session, image(), HistorySource::Overlay);
        QVERIFY(repeated.success);
        QCOMPARE(repeated.item.fileName, old.item.fileName);
        chosen = f.temp.filePath(QStringLiteral("可用"));
        child<QPushButton>(&dialog, "historyChooseDirectory")->click();
        save(dialog);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(QFileInfo::exists(old.item.absolutePath));
    }
    void lockedRootAndSettingsFailure()
    {
        Fixture f;
        const auto oldRoot = f.manager.rootDirectory();
        const auto newRoot = f.temp.filePath(QStringLiteral("锁库"));
        HistoryStore candidate(newRoot);
        QVERIFY(candidate.initialize().error.isEmpty());
        QLockFile lock(QDir(newRoot).filePath(QStringLiteral(".waibusnap.lock")));
        // 使用 Store 的真实锁名，避免测试假定锁定成功却没有走到失败路径。
        QVERIFY(lock.tryLock());
        auto preferences = f.actions().preferences;
        preferences.rootDirectory = newRoot;
        const auto locked = f.manager.savePreferences(preferences);
        QVERIFY(!locked.success);
        QCOMPARE(f.manager.rootDirectory(), oldRoot);
        lock.unlock();
        QFile blocker(f.temp.filePath(QStringLiteral("不可写配置")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        HistoryManager manager(AppSettings(blocker.fileName() + QStringLiteral("/settings.ini")),
                               f.temp.filePath(QStringLiteral("独立旧库")));
        manager.initialize();
        const auto before = manager.rootDirectory();
        preferences.rootDirectory = f.temp.filePath(QStringLiteral("独立新库"));
        QVERIFY(!manager.savePreferences(preferences).success);
        QCOMPARE(manager.rootDirectory(), before);
    }
    void templateOnlyAffectsNewGroupsAndAtLeastOne()
    {
        Fixture f;
        const auto old = f.manager.currentGroup();
        const auto persistentRoot = f.settings.loadHistoryPreferences().rootDirectory;
        const auto session = f.manager.beginSession();
        const auto record = f.manager.record(session, image(), HistorySource::Overlay);
        QVERIFY(record.success);
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), f.actions());
        historyPage(dialog);
        for (const auto* id : {"historyAgeEnabled", "historyCountEnabled", "historyBytesEnabled"})
            child<QCheckBox>(&dialog, id)->setChecked(false);
        save(dialog);
        QVERIFY(dialog.isVisible());
        QVERIFY(!child<QLabel>(&dialog, "settingsStatus")->text().isEmpty());
        child<QCheckBox>(&dialog, "historyCountEnabled")->setChecked(true);
        child<QSpinBox>(&dialog, "historyMaxCount")->setValue(3);
        save(dialog);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(f.manager.currentGroup()->cleanup.maxCount, old->cleanup.maxCount);
        QCOMPARE(f.settings.loadHistoryPreferences().rootDirectory, persistentRoot);
        const auto added = f.manager.createGroup(QStringLiteral("新建组"));
        QVERIFY(added.success);
        QCOMPARE(added.group.cleanup.maxCount, 3);
        QVERIFY(!added.group.cleanup.ageEnabled && !added.group.cleanup.bytesEnabled);
        const auto repeat = f.manager.record(session, image(), HistorySource::Overlay);
        QVERIFY(repeat.success);
        QCOMPARE(repeat.item.fileName, record.item.fileName);
    }
    void bytePrecisionAndProtection()
    {
        Fixture f;
        auto preferences = f.actions().preferences;
        preferences.defaultCleanup.maxBytes = 103;
        QVERIFY(f.manager.savePreferences(preferences).success);
        const auto first = f.add();
        const auto second = f.add();
        QVERIFY(f.manager.setStarred(historyItemId(first.item), true).success);
        const auto usage = f.manager.totalUsage(historyItemId(second.item));
        QCOMPARE(usage.protectedCount, 2);
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), f.actions());
        save(dialog);
        QCOMPARE(f.manager.defaultCleanupPolicy().maxBytes, 103);
        QVERIFY(QFileInfo::exists(first.item.absolutePath));
        QVERIFY(QFileInfo::exists(second.item.absolutePath));
    }
    void hotkeyConfigIndependentAndInvalidFallback()
    {
        Fixture f;
        for (const auto action : hotkeyActions)
            QCOMPARE(f.settings.loadHotkey(action).sequence, defaultHotkey(action));
        QVERIFY(f.settings.saveHotkey(QKeySequence(Qt::Key_F2)).isEmpty());
        QVERIFY(
            f.settings.saveHotkey(QKeySequence(Qt::CTRL | Qt::Key_7), HotkeyAction::HistoryPrevious)
                .isEmpty());
        QCOMPARE(f.settings.loadHotkey().sequence, QKeySequence(Qt::Key_F2));
        QCOMPARE(f.settings.loadHotkey(HotkeyAction::HistoryPrevious).sequence,
                 QKeySequence(Qt::CTRL | Qt::Key_7));
        QSettings ini(f.temp.filePath(QStringLiteral("设置.ini")), QSettings::IniFormat);
        ini.setValue(QStringLiteral("hotkey/historyNext"), QStringLiteral("A"));
        ini.sync();
        const auto invalid = f.settings.loadHotkey(HotkeyAction::HistoryNext);
        QCOMPARE(invalid.sequence, QKeySequence(Qt::Key_F8));
        QVERIFY(!invalid.explanation.isEmpty());
        QCOMPARE(ini.value(QStringLiteral("hotkey/sequence")).toString(), QStringLiteral("F2"));
    }
    void hotkeyFourActionsFailureAndRelease()
    {
        Fixture f;
        FakeHotkey fake;
        std::array<int, 4> triggered{};
        std::array<std::unique_ptr<HotkeySettings>, 4> keys;
        for (size_t i = 0; i < keys.size(); ++i)
        {
            keys[i] = std::make_unique<HotkeySettings>(
                f.settings, fake, [&, i] { ++triggered[i]; }, hotkeyActions[i]);
            keys[i]->initialize();
            QVERIFY(keys[i]->enabled());
            fake.fire(hotkeyActions[i]);
            QCOMPARE(triggered[i], 1);
        }
        fake.rejected = QStringLiteral("F10");
        QVERIFY(!keys[1]->changeHotkey(QKeySequence(Qt::Key_F10)).isEmpty());
        QCOMPARE(fake.keys[1], defaultHotkey(HotkeyAction::HistoryPrevious));
        QCOMPARE(f.settings.loadHotkey(HotkeyAction::HistoryPrevious).sequence, fake.keys[1]);
        QVERIFY(!keys[1]->changeHotkey(QKeySequence(Qt::Key_F8)).isEmpty());
        QVERIFY(keys[2]->changeHotkey(QKeySequence(Qt::Key_F11)).isEmpty());
        QCOMPARE(fake.keys[0], defaultScreenshotHotkey());
        QCOMPARE(fake.keys[3], QKeySequence(Qt::Key_F9));
        fake.unregister(HotkeyAction::HistoryNext);
        fake.fire(HotkeyAction::HistoryNext);
        QCOMPARE(triggered[2], 1);
        fake.fire(HotkeyAction::Screenshot);
        QCOMPARE(triggered[0], 2);
        static_cast<GlobalHotkey&>(fake).unregister();
        for (const auto& handler : fake.handlers)
            QVERIFY(!handler);
    }
    void startupSingleFailureKeepsOtherActions()
    {
        Fixture f;
        FakeHotkey fake;
        fake.rejected = QStringLiteral("F7");
        for (const auto action : hotkeyActions)
        {
            HotkeySettings settings(f.settings, fake, [] {}, action);
            const auto result = settings.initialize();
            QCOMPARE(settings.enabled(), action != HotkeyAction::HistoryPrevious);
            QCOMPARE(result.warning.isEmpty(), action != HotkeyAction::HistoryPrevious);
        }
        QCOMPARE(fake.keys[0], QKeySequence(Qt::Key_F1));
        QVERIFY(fake.keys[1].isEmpty());
        QCOMPARE(fake.keys[2], QKeySequence(Qt::Key_F8));
        QCOMPARE(fake.keys[3], QKeySequence(Qt::Key_F9));
    }
    void dialogDuplicateInvalidAndSingleFailure()
    {
        Fixture f;
        int calls = 0;
        auto actions = f.actions();
        actions.saveHotkey = [&](HotkeyAction action, const QKeySequence& sequence)
        {
            ++calls;
            return sequence == QKeySequence(Qt::Key_F10)
                       ? QStringLiteral("单项注册失败，原绑定保留")
                       : f.settings.saveHotkey(sequence, action);
        };
        SettingsDialog dialog(
            defaultScreenshotHotkey(),
            [&](const auto&)
            {
                ++calls;
                return QString();
            },
            {}, nullptr, {}, startup(), actions);
        historyPage(dialog);
        auto* editor = child<QKeySequenceEdit>(&dialog, "historyNextHotkey");
        editor->setKeySequence(defaultScreenshotHotkey());
        save(dialog);
        QCOMPARE(calls, 0);
        QVERIFY(child<QLabel>(&dialog, "settingsStatus")->text().contains(QStringLiteral("重复")));
        editor->setKeySequence(QKeySequence(Qt::Key_A));
        save(dialog);
        QCOMPARE(calls, 0);
        editor->setKeySequence(QKeySequence(Qt::Key_F10));
        save(dialog);
        QCOMPARE(calls, 1);
        QVERIFY(dialog.isVisible());
        QCOMPARE(f.settings.loadHotkey(HotkeyAction::HistoryNext).sequence,
                 QKeySequence(Qt::Key_F8));
        editor->setKeySequence(QKeySequence(Qt::Key_F11));
        save(dialog);
        QCOMPARE(calls, 2);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(f.settings.loadHotkey(HotkeyAction::HistoryNext).sequence,
                 QKeySequence(Qt::Key_F11));
    }
    void trayMenusEmptyAndGuards()
    {
        Fixture f;
        auto fake = std::make_unique<FakeHotkey>();
        auto* raw = fake.get();
        ApplicationController c(*qApp, options(f), {}, {}, startup(), {}, {}, std::move(fake));
        ApplicationControllerTestAccess::initializeHotkeys(c);
        const auto attempts = raw->attempts.size();
        QVERIFY(!ApplicationControllerTestAccess::change(c, HotkeyAction::Screenshot,
                                                         QKeySequence(Qt::Key_F8))
                     .isEmpty());
        QCOMPARE(raw->attempts.size(), attempts);
        auto* historyMenu = child<QMenu>(c.menu(), "historyMenu");
        QCOMPARE(historyMenu->actions().size(), 3);
        auto* previous = child<QAction>(c.menu(), "historyPreviousAction");
        auto* next = child<QAction>(c.menu(), "historyNextAction");
        auto* open = child<QAction>(c.menu(), "openHistoryAction");
        QVERIFY(!previous->isEnabled() && !next->isEnabled() && open->isEnabled());
        auto& manager = ApplicationControllerTestAccess::history(c);
        QVERIFY(manager.record(manager.beginSession(), image(), HistorySource::Overlay).success);
        QVERIFY(QMetaObject::invokeMethod(c.menu(), "aboutToShow"));
        QVERIFY(previous->isEnabled() && next->isEnabled());
        for (int state = 1; state <= 4; ++state)
        {
            ApplicationControllerTestAccess::guard(c, state);
            QVERIFY(!historyMenu->isEnabled());
            QVERIFY(!previous->isEnabled() && !next->isEnabled() && !open->isEnabled());
            c.navigateHistory(1);
            c.openHistory();
            for (const auto action : hotkeyActions)
                raw->fire(action);
            QVERIFY(!ApplicationControllerTestAccess::window(c));
        }
        ApplicationControllerTestAccess::guard(c, 0);
        next->trigger();
        QVERIFY(ApplicationControllerTestAccess::window(c));
    }
    void navigationAllGroupsCurrentSelectionAndReuse()
    {
        Fixture f;
        auto fake = std::make_unique<FakeHotkey>();
        auto* raw = fake.get();
        ApplicationController c(*qApp, options(f), {}, {}, startup(), {}, {}, std::move(fake));
        auto& m = ApplicationControllerTestAccess::history(c);
        f.add(&m);
        f.add(&m);
        const auto group = m.createGroup(QStringLiteral("另外分组"));
        QVERIFY(group.success);
        QVERIFY(m.setCurrentGroup(group.group.id).success);
        f.add(&m);
        HistoryWindowState stored;
        stored.lastView = group.group.id;
        QVERIFY(AppSettings(options(f).settingsFile).saveHistoryWindowState(stored).isEmpty());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        raw->fire(HotkeyAction::HistoryNext);
        auto* window = ApplicationControllerTestAccess::window(c);
        QVERIFY(window);
        auto* list = child<QListWidget>(window, "historyThumbnails");
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->currentRow(), 0);
        raw->fire(HotkeyAction::HistoryPrevious);
        QCOMPARE(list->currentRow(), 1);
        raw->fire(HotkeyAction::HistoryNext);
        QCOMPARE(list->currentRow(), 0);
        raw->fire(HotkeyAction::HistoryOpen);
        QCOMPARE(ApplicationControllerTestAccess::window(c), window);
        auto* combo = child<QComboBox>(window, "historyGroupCombo");
        combo->setCurrentIndex(combo->findData(group.group.id));
        QCOMPARE(list->count(), 1);
        raw->fire(HotkeyAction::HistoryNext);
        QCOMPARE(list->currentRow(), 0);
        QCOMPARE(m.currentGroupId(), group.group.id);
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        raw->fire(HotkeyAction::HistoryPrevious);
        window = ApplicationControllerTestAccess::window(c);
        QVERIFY(window);
        QCOMPARE(child<QListWidget>(window, "historyThumbnails")->count(), 3);
        ApplicationControllerTestAccess::cleanup(c);
        for (const auto& handler : raw->handlers)
            QVERIFY(!handler);
    }
    void navigationFirstStep_data()
    {
        QTest::addColumn<bool>("previous");
        QTest::addColumn<QString>("selection");
        QTest::addColumn<int>("expectedRow");
        QTest::newRow("previous-no-selection") << true << QString() << 1;
        QTest::newRow("next-no-selection") << false << QString() << 0;
        QTest::newRow("previous-restored") << true << QStringLiteral("middle") << 2;
        QTest::newRow("next-restored") << false << QStringLiteral("middle") << 0;
        QTest::newRow("previous-missing") << true << QStringLiteral("missing") << 1;
        QTest::newRow("next-missing") << false << QStringLiteral("missing") << 0;
        QTest::newRow("previous-oldest") << true << QStringLiteral("oldest") << 2;
        QTest::newRow("next-newest") << false << QStringLiteral("newest") << 0;
    }
    void navigationFirstStep()
    {
        QFETCH(bool, previous);
        QFETCH(QString, selection);
        QFETCH(int, expectedRow);
        Fixture f;
        auto fake = std::make_unique<FakeHotkey>();
        auto* raw = fake.get();
        ApplicationController c(*qApp, options(f), {}, {}, startup(), {}, {}, std::move(fake));
        auto& m = ApplicationControllerTestAccess::history(c);
        const auto oldest = f.add(&m);
        QTest::qWait(2);
        const auto middle = f.add(&m);
        const auto group = m.createGroup(QStringLiteral("首步分组"));
        QVERIFY(group.success);
        QVERIFY(m.setCurrentGroup(group.group.id).success);
        QTest::qWait(2);
        const auto newest = f.add(&m);
        const QStringList ids = {historyItemId(newest.item), historyItemId(middle.item),
                                 historyItemId(oldest.item)};
        const AppSettings settings(options(f).settingsFile);
        HistoryWindowState stored;
        stored.lastView = group.group.id;
        stored.sortOrder = QStringLiteral("oldest");
        stored.lastSelection = selection == QStringLiteral("middle")   ? ids[1]
                               : selection == QStringLiteral("oldest") ? ids[2]
                               : selection == QStringLiteral("newest") ? ids[0]
                                                                       : selection;
        QVERIFY(settings.saveHistoryWindowState(stored).isEmpty());
        ApplicationControllerTestAccess::initializeHotkeys(c);
        QVERIFY(!ApplicationControllerTestAccess::window(c));
        raw->fire(previous ? HotkeyAction::HistoryPrevious : HotkeyAction::HistoryNext);
        auto* window = ApplicationControllerTestAccess::window(c);
        QVERIFY(window && window->isVisible());
        QCOMPARE(window->selectedItemId(), ids[expectedRow]);
        QCOMPARE(child<QComboBox>(window, "historyGroupCombo")->currentData().toString(),
                 QStringLiteral("all"));
        QCOMPARE(child<QComboBox>(window, "historySortCombo")->currentIndex(), 0);
        QCOMPARE(m.currentGroupId(), group.group.id);
        QCOMPARE(m.totalUsage().managedCount, 3);
        window->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!ApplicationControllerTestAccess::window(c));
        QCOMPARE(settings.loadHistoryWindowState().lastSelection, ids[expectedRow]);
        // 真正关闭并销毁后再次触发，必须从刚落盘的记录继续，而非重置到最新。
        raw->fire(previous ? HotkeyAction::HistoryPrevious : HotkeyAction::HistoryNext);
        window = ApplicationControllerTestAccess::window(c);
        QVERIFY(window);
        QCOMPARE(window->selectedItemId(),
                 ids[previous ? std::min(expectedRow + 1, 2) : std::max(expectedRow - 1, 0)]);
    }
    void navigationOpenGroupAndSort()
    {
        Fixture f;
        HistoryWindow window(f.manager, f.settings);
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        const auto oldest = f.add();
        QTest::qWait(2);
        const auto middle = f.add();
        QTest::qWait(2);
        const auto newest = f.add();
        window.refresh();
        window.show();
        auto* combo = child<QComboBox>(&window, "historyGroupCombo");
        combo->setCurrentIndex(combo->findData(f.manager.currentGroupId()));
        auto* list = child<QListWidget>(&window, "historyThumbnails");
        auto* sort = child<QComboBox>(&window, "historySortCombo");
        for (int order = 0; order < 2; ++order)
        {
            sort->setCurrentIndex(order);
            list->setCurrentRow(1);
            QCOMPARE(window.selectedItemId(), historyItemId(middle.item));
            window.navigate(-1);
            QCOMPARE(window.selectedItemId(), historyItemId(oldest.item));
            window.navigate(-1);
            QCOMPARE(window.selectedItemId(), historyItemId(oldest.item));
            window.navigate(1);
            QCOMPARE(window.selectedItemId(), historyItemId(middle.item));
            window.navigate(1);
            QCOMPARE(window.selectedItemId(), historyItemId(newest.item));
            window.navigate(1);
            QCOMPARE(window.selectedItemId(), historyItemId(newest.item));
            QCOMPARE(combo->currentData().toString(), f.manager.currentGroupId());
            QCOMPARE(sort->currentIndex(), order);
            // 窗口方向键仍按列表行序移动，不改变既有交互。
            list->setCurrentRow(1);
            QTest::keyClick(&window, Qt::Key_Right);
            QCOMPARE(list->currentRow(), 2);
            QTest::keyClick(&window, Qt::Key_Left);
            QCOMPARE(list->currentRow(), 1);
        }
    }
    void controllerSettingsRootSwitchAndModalGuard()
    {
        Fixture f;
        ApplicationController c(*qApp, options(f), {}, {}, startup(), {}, {},
                                std::make_unique<FakeHotkey>());
        auto& m = ApplicationControllerTestAccess::history(c);
        const auto old = f.add(&m);
        c.openHistory();
        auto* window = ApplicationControllerTestAccess::window(c);
        QTimer::singleShot(0,
                           [&]
                           {
                               auto* dialog =
                                   qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                               QVERIFY(dialog);
                               auto* menu = child<QMenu>(c.menu(), "historyMenu");
                               QVERIFY(!menu->isEnabled());
                               c.navigateHistory(1);
                               QCOMPARE(ApplicationControllerTestAccess::window(c), window);
                               child<QLineEdit>(dialog, "historyRootDirectory")
                                   ->setText(f.temp.filePath(QStringLiteral("设置切换")));
                               save(*dialog);
                           });
        child<QAction>(c.menu(), "settingsAction")->trigger();
        QCOMPARE(m.rootDirectory(), f.temp.filePath(QStringLiteral("设置切换")));
        QCOMPARE(ApplicationControllerTestAccess::window(c), window);
        QCOMPARE(child<QListWidget>(window, "historyThumbnails")->count(), 0);
        QVERIFY(QFileInfo::exists(old.item.absolutePath));
        QVERIFY(child<QAction>(c.menu(), "openHistoryAction")->isEnabled());
    }
};
QTEST_MAIN(HistorySettingsTest)
#include "history_settings_test.moc"
