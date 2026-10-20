#include "app/application_controller.h"
#include "ui/group_settings_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
using namespace waibusnap;
namespace waibusnap
{
struct ApplicationControllerTestAccess
{
    static void cleanup(ApplicationController& c) { c.cleanup(); }
    static HistoryManager& history(ApplicationController& c) { return c.history_; }
    static HistoryWindow* window(ApplicationController& c) { return c.historyWindow_; }
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
    auto* value = root->findChild<T*>(QLatin1String(name));
    if (!value)
        qFatal("缺少分组控件：%s", name);
    return value;
}
QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
QImage picture()
{
    QImage image(120, 80, QImage::Format_ARGB32);
    image.fill(QColor(30, 100, 200, 120));
    return image;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings;
    HistoryManager manager;
    HistoryStore store;
    HistoryTrash trash;
    bool failTrash = false;
    int trashCalls = 0;
    QPointer<HistoryWindow> window;
    Fixture()
        : settings(temp.filePath(QStringLiteral("settings.ini"))),
          manager(settings, temp.filePath(QStringLiteral("历史 根目录"))),
          store(temp.filePath(QStringLiteral("历史 根目录")))
    {
        if (!temp.isValid() || !manager.initialize().success ||
            !QDir().mkdir(temp.filePath(QStringLiteral("假回收站"))))
            qFatal("临时历史库初始化失败，拒绝访问真实目录。");
        trash = [this](const QString& path)
        {
            ++trashCalls;
            if (failTrash)
                return HistoryStatus{false, QStringLiteral("回收站失败替身")};
            const bool moved = QDir().rename(path, temp.filePath(QStringLiteral("假回收站/")) +
                                                       QFileInfo(path).fileName());
            return HistoryStatus{moved, moved ? QString() : QStringLiteral("临时目录移动失败")};
        };
    }
    ~Fixture() { delete window.data(); }
    QString current() const { return manager.currentGroupId(); }
    HistoryGroup group(const QString& id)
    {
        for (const auto& group : manager.snapshot().groups)
            if (group.group.id == id)
                return group.group;
        qFatal("缺少临时分组。");
    }
    QString groupFile(const QString& id)
    {
        return QDir(group(id).absolutePath).filePath(QStringLiteral(".waibusnap/group.json"));
    }
    HistoryWindow* open()
    {
        HistoryWindowActions actions;
        actions.trash = trash;
        window = new HistoryWindow(manager, settings, actions);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }
};
void menuAction(HistoryWindow* window, const QString& id, const char* actionName,
                const std::function<void(QAction*)>& inspect = {})
{
    auto* list = child<QListWidget>(window, "historyGroups");
    QListWidgetItem* entry = nullptr;
    for (int row = 0; row < list->count(); ++row)
        if (list->item(row)->data(Qt::UserRole).toString() == id)
            entry = list->item(row);
    if (!entry)
        qFatal("缺少上下文菜单分组。");
    list->scrollToItem(entry);
    QTimer::singleShot(0, window,
                       [window, actionName, inspect]
                       {
                           auto* menu = child<QMenu>(window, "historyGroupMenu");
                           auto* action = child<QAction>(menu, actionName);
                           if (inspect)
                               inspect(action);
                           menu->close();
                           if (action->isEnabled())
                               action->trigger();
                       });
    const QPoint point = list->visualItemRect(entry).center();
    QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, point));
}
void confirmDelete(bool accept, qsizetype externalCount = -1)
{
    QTimer::singleShot(
        0, qApp,
        [accept, externalCount]
        {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box);
            QCOMPARE(box->objectName(), QStringLiteral("historyDeleteGroupConfirmation"));
            if (externalCount >= 0)
                QVERIFY(box->text().contains(QStringLiteral("外部图片：%1 张").arg(externalCount)));
            for (auto* button : box->buttons())
                if (box->buttonRole(button) ==
                    (accept ? QMessageBox::DestructiveRole : QMessageBox::RejectRole))
                {
                    button->click();
                    return;
                }
            QFAIL("缺少删除确认按钮。");
        });
}
}
class HistoryGroupsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void managerNamesAndDefaultIdentity()
    {
        Fixture f;
        const auto original = f.current();
        QSignalSpy changed(&f.manager, &HistoryManager::groupsChanged);
        const auto created = f.manager.createGroup(QStringLiteral("  微积分  "));
        QVERIFY(created.success);
        QCOMPARE(created.group.name, QStringLiteral("微积分"));
        QCOMPARE(f.current(), original);
        QCOMPARE(f.manager.groups().size(), 2);
        QCOMPARE(f.manager.snapshot().groups.size(), 2);
        for (const auto& name :
             {QString(), QStringLiteral("../逃逸"), QStringLiteral("CON"),
              QStringLiteral(".waibusnap"), QStringLiteral("a/b"), QStringLiteral("a.")})
        {
            const auto result = f.manager.createGroup(name);
            QVERIFY(!result.success);
            QVERIFY(!result.error.isEmpty());
        }
        QVERIFY(!f.manager.createGroup(QStringLiteral("微积分")).success);
        QVERIFY(f.manager.createGroup(QStringLiteral("Meeting")).success);
        QVERIFY(!f.manager.createGroup(QStringLiteral("meeting")).success);
        QVERIFY(!f.manager.renameGroup(created.group.id, QStringLiteral("MEETING")).success);
        const auto renamed = f.manager.renameGroup(original, QStringLiteral("默认 已改名"));
        QVERIFY(renamed.success);
        QVERIFY(renamed.group.isDefault);
        QCOMPARE(renamed.group.id, original);
        QVERIFY(!f.manager.deleteGroup(original, f.trash).success);
        QCOMPARE(f.trashCalls, 0);
        QCOMPARE(f.current(), original);
        QCOMPARE(changed.count(), 3);
    }
    void templateIsReadAtCreationAndGroupsIndependent()
    {
        Fixture f;
        const auto before = f.group(f.current()).cleanup;
        auto preferences = f.settings.loadHistoryPreferences();
        preferences.defaultCleanup = {true, false, 7, true, 12, false, 3456789};
        QVERIFY(f.settings.saveHistoryPreferences(preferences).isEmpty());
        const auto created = f.manager.createGroup(QStringLiteral("新模板"));
        QVERIFY(created.success);
        QCOMPARE(cleanupPolicyToJson(created.group.cleanup),
                 cleanupPolicyToJson(preferences.defaultCleanup));
        QCOMPARE(cleanupPolicyToJson(f.group(f.current()).cleanup), cleanupPolicyToJson(before));
    }
    void deleteCurrentAndRestart()
    {
        Fixture f;
        const auto fallback = f.current();
        const auto group = f.manager.createGroup(QStringLiteral("待删除")).group;
        QVERIFY(f.manager.setCurrentGroup(group.id).success);
        QVERIFY(
            f.manager.record(f.manager.beginSession(), picture(), HistorySource::Overlay).success);
        QVERIFY(picture().save(QDir(group.absolutePath).filePath(QStringLiteral("外部.png"))));
        QSignalSpy current(&f.manager, &HistoryManager::currentGroupChanged);
        QVERIFY(f.manager.deleteGroup(group.id, f.trash).success);
        QCOMPARE(f.trashCalls, 1);
        QVERIFY(!QFileInfo::exists(group.absolutePath));
        const QString moved = f.temp.filePath(QStringLiteral("假回收站/待删除"));
        QVERIFY(QFileInfo::exists(moved));
        QCOMPARE(QDir(moved).entryList({QStringLiteral("*.png")}, QDir::Files).size(), 2);
        QCOMPARE(f.current(), fallback);
        QCOMPARE(current.count(), 1);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, fallback);
        HistoryManager restarted(f.settings, f.manager.rootDirectory());
        QVERIFY(restarted.initialize().success);
        QCOMPARE(restarted.currentGroupId(), fallback);
    }
    void deleteFailuresKeepCurrentAndSnapshot()
    {
        Fixture f;
        const auto group = f.manager.createGroup(QStringLiteral("保留")).group;
        QVERIFY(f.manager.setCurrentGroup(group.id).success);
        QSignalSpy changes(&f.manager, &HistoryManager::groupsChanged);
        QSignalSpy current(&f.manager, &HistoryManager::currentGroupChanged);
        f.failTrash = true;
        QVERIFY(!f.manager.deleteGroup(group.id, f.trash).success);
        QCOMPARE(f.current(), group.id);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, group.id);
        QVERIFY(QFileInfo::exists(group.absolutePath));
        QCOMPARE(f.manager.groups().size(), 2);
        QCOMPARE(changes.count(), 0);
        QCOMPARE(current.count(), 0);
        QVERIFY(
            !f.manager.deleteGroup(group.id, [](const QString&) { return HistoryStatus{true, {}}; })
                 .success);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, group.id);
        QVERIFY(!f.manager.deleteGroup(group.id, {}).success);
        QVERIFY(!f.manager.deleteGroup(QStringLiteral("missing"), f.trash).success);
        const QString settings = f.temp.filePath(QStringLiteral("settings.ini"));
        QVERIFY(QFile::rename(settings, settings + QStringLiteral(".backup")));
        QVERIFY(QDir().mkdir(settings));
        const int calls = f.trashCalls;
        QVERIFY(!f.manager.deleteGroup(group.id, f.trash).success);
        QCOMPARE(f.trashCalls, calls);
        QCOMPARE(f.current(), group.id);
        QVERIFY(QDir().rmdir(settings));
        QVERIFY(QFile::rename(settings + QStringLiteral(".backup"), settings));
        f.failTrash = false;
        QVERIFY(f.manager.deleteGroup(group.id, f.trash).success);
    }
    void policyValidationAndFailureIsolation()
    {
        Fixture f;
        const auto group = f.manager.createGroup(QStringLiteral("策略")).group;
        auto policy = group.cleanup;
        const auto before = read(f.groupFile(group.id));
        policy.ageEnabled = policy.countEnabled = policy.bytesEnabled = false;
        QVERIFY(!f.manager.saveCleanupPolicy(group.id, policy, true).success);
        QCOMPARE(read(f.groupFile(group.id)), before);
        QVERIFY(f.current() != group.id);
        QLockFile lock(QDir(f.manager.rootDirectory()).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock());
        policy.countEnabled = true;
        QVERIFY(!f.manager.saveCleanupPolicy(group.id, policy, true).success);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, f.current());
        QCOMPARE(read(f.groupFile(group.id)), before);
        lock.unlock();
        QVERIFY(f.manager.saveCleanupPolicy(group.id, policy, true).success);
        QCOMPARE(f.current(), group.id);
    }
    void browsingAndExplicitCurrent()
    {
        Fixture f;
        const auto original = f.current();
        const auto group = f.manager.createGroup(QStringLiteral("浏览")).group;
        auto* w = f.open();
        QSignalSpy changed(&f.manager, &HistoryManager::currentGroupChanged);
        child<QComboBox>(w, "historyGroupCombo")->setCurrentIndex(3);
        QCOMPARE(f.current(), original);
        QCOMPARE(changed.count(), 0);
        menuAction(w, group.id, "historyGroupSetCurrent",
                   [w](QAction*) { w->setGroupActionsEnabled(false); });
        QCOMPARE(f.current(), original);
        w->setGroupActionsEnabled(true);
        menuAction(w, group.id, "historyGroupSetCurrent");
        QCOMPARE(f.current(), group.id);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, group.id);
        QCOMPARE(changed.count(), 1);
        QVERIFY(child<QListWidget>(w, "historyGroups")
                    ->item(3)
                    ->text()
                    .startsWith(QStringLiteral("● ")));
        QVERIFY(!child<QListWidget>(w, "historyGroups")
                     ->item(2)
                     ->text()
                     .startsWith(QStringLiteral("● ")));
        QCOMPARE(child<QComboBox>(w, "historyGroupCombo")->currentData().toString(), group.id);
    }
    void namingDialogValidationAndRename()
    {
        Fixture f;
        auto* w = f.open();
        const auto original = f.current();
        QTimer::singleShot(
            0, w,
            [w]
            {
                auto* dialog = child<QDialog>(w, "historyGroupNameDialog");
                auto* edit = child<QLineEdit>(dialog, "historyGroupName");
                auto* save = child<QPushButton>(dialog, "historyGroupNameSave");
                for (const auto& name :
                     {QStringLiteral("CON"), QStringLiteral("../越界"), QStringLiteral("默认分组")})
                {
                    edit->setText(name);
                    save->click();
                    QVERIFY(dialog->isVisible());
                    QVERIFY(!child<QLabel>(dialog, "historyGroupNameError")->text().isEmpty());
                }
                edit->setText(QStringLiteral("网课"));
                save->click();
            });
        auto* combo = child<QComboBox>(w, "historyGroupCombo");
        combo->setCurrentIndex(combo->findData(QStringLiteral("new-group")));
        QCOMPARE(f.manager.groups().size(), 2);
        QCOMPARE(f.current(), original);
        const auto id = combo->currentData().toString();
        const auto group = f.group(id);
        QCOMPARE(group.name, QStringLiteral("网课"));
        QTimer::singleShot(0, w,
                           [w]
                           {
                               // 上下文菜单本身先进入事件循环，下一轮才打开命名对话框。
                               QTimer::singleShot(
                                   0, w,
                                   [w]
                                   {
                                       auto* dialog = child<QDialog>(w, "historyGroupNameDialog");
                                       child<QLineEdit>(dialog, "historyGroupName")
                                           ->setText(QStringLiteral("网课 改名"));
                                       child<QPushButton>(dialog, "historyGroupNameSave")->click();
                                   });
                           });
        menuAction(w, group.id, "historyGroupRename");
        QCOMPARE(f.group(group.id).name, QStringLiteral("网课 改名"));
        QCOMPARE(f.current(), original);
    }
    void settingsRulesSaveAndCancel()
    {
        Fixture f;
        const auto group = f.manager.createGroup(QStringLiteral("独立策略")).group;
        const auto before = read(f.groupFile(group.id));
        {
            GroupSettingsDialog dialog(f.manager, group.id, f.trash);
            dialog.show();
            child<QSpinBox>(&dialog, "groupMaxAgeDays")->setValue(9);
            child<QCheckBox>(&dialog, "groupMakeCurrent")->setChecked(true);
            child<QPushButton>(&dialog, "groupSettingsCancel")->click();
        }
        QCOMPARE(read(f.groupFile(group.id)), before);
        QVERIFY(f.current() != group.id);
        GroupSettingsDialog dialog(f.manager, group.id, f.trash);
        dialog.show();
        child<QSpinBox>(&dialog, "groupMaxAgeDays")->setValue(7);
        child<QSpinBox>(&dialog, "groupMaxCount")->setValue(13);
        child<QDoubleSpinBox>(&dialog, "groupMaxMiB")->setValue(2.5);
        child<QCheckBox>(&dialog, "groupMakeCurrent")->setChecked(true);
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        const auto saved = f.group(group.id).cleanup;
        QCOMPARE(saved.maxAgeDays, 7);
        QCOMPARE(saved.maxCount, 13);
        QCOMPARE(saved.maxBytes, 2621440);
        QCOMPARE(f.current(), group.id);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, group.id);
    }
    void settingsAtLeastOneAndDisabledRules()
    {
        Fixture f;
        GroupSettingsDialog dialog(f.manager, f.current(), f.trash);
        dialog.show();
        for (const auto* key : {"groupAgeEnabled", "groupCountEnabled", "groupBytesEnabled"})
            child<QCheckBox>(&dialog, key)->setChecked(false);
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QVERIFY(dialog.isVisible());
        QVERIFY(
            child<QLabel>(&dialog, "groupSettingsError")->text().contains(QStringLiteral("至少")));
        QVERIFY(!child<QSpinBox>(&dialog, "groupMaxCount")->isEnabled());
        child<QCheckBox>(&dialog, "groupCleanupEnabled")->setChecked(false);
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        const auto policy = f.group(f.current()).cleanup;
        QVERIFY(!policy.enabled && !policy.ageEnabled && !policy.countEnabled &&
                !policy.bytesEnabled);
        QVERIFY(!child<QPushButton>(&dialog, "groupDelete")->isEnabled());
    }
    void settingsRestoreCurrentTemplateAndBytesPrecision()
    {
        Fixture f;
        auto policy = f.group(f.current()).cleanup;
        policy.maxBytes = 3456789;
        QVERIFY(f.manager.saveCleanupPolicy(f.current(), policy).success);
        GroupSettingsDialog unchanged(f.manager, f.current(), f.trash);
        child<QPushButton>(&unchanged, "groupSettingsSave")->click();
        QCOMPARE(f.group(f.current()).cleanup.maxBytes, 3456789);
        GroupSettingsDialog dialog(f.manager, f.current(), f.trash);
        auto preferences = f.settings.loadHistoryPreferences();
        preferences.defaultCleanup = {false, false, 55, true, 8, false, 1234567};
        QVERIFY(f.settings.saveHistoryPreferences(preferences).isEmpty());
        child<QPushButton>(&dialog, "groupRestoreDefaults")->click();
        QCOMPARE(child<QSpinBox>(&dialog, "groupMaxAgeDays")->value(), 55);
        QCOMPARE(child<QSpinBox>(&dialog, "groupMaxCount")->value(), 8);
        QCOMPARE(f.group(f.current()).cleanup.maxBytes, 3456789);
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QCOMPARE(cleanupPolicyToJson(f.group(f.current()).cleanup),
                 cleanupPolicyToJson(preferences.defaultCleanup));
    }
    void settingsUsageProtectionAndNoCleanup()
    {
        Fixture f;
        const auto session = f.manager.beginSession();
        const auto first = f.manager.record(session, picture(), HistorySource::Overlay);
        QVERIFY(first.success);
        const auto second =
            f.manager.record(f.manager.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(second.success);
        const auto third =
            f.manager.record(f.manager.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(third.success);
        QVERIFY(f.store.setStarred(f.current(), first.item.fileName, true).success);
        QVERIFY(picture().save(
            QDir(f.group(f.current()).absolutePath).filePath(QStringLiteral("外部.jpg"))));
        f.manager.refresh();
        GroupSettingsDialog dialog(f.manager, f.current(), f.trash, historyItemId(second.item));
        dialog.show();
        const auto text = child<QLabel>(&dialog, "groupUsage")->text();
        QVERIFY(text.contains(QStringLiteral("当前占用：4 张")));
        QVERIFY(text.contains(QStringLiteral("外部图片：1 张")));
        QVERIFY(text.contains(QStringLiteral("受保护占用：3 张")));
        child<QSpinBox>(&dialog, "groupMaxCount")->setValue(1);
        child<QDoubleSpinBox>(&dialog, "groupMaxMiB")->setValue(0.000001);
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QCOMPARE(f.store.scan().groups[0].items.size(), 4);
        QVERIFY(f.manager.record(session, picture(), HistorySource::Overlay).success);
        QCOMPARE(f.store.scan().groups[0].items.size(), 4);
        QCOMPARE(f.trashCalls, 0);
    }
    void settingsCurrentSyncAndNarrowLayout()
    {
        Fixture f;
        const auto group = f.manager.createGroup(QStringLiteral("切换")).group;
        GroupSettingsDialog dialog(f.manager, group.id, f.trash);
        dialog.resize(420, 500);
        dialog.show();
        QCoreApplication::processEvents();
        auto* current = child<QCheckBox>(&dialog, "groupMakeCurrent");
        QVERIFY(!current->isChecked());
        QVERIFY(f.manager.setCurrentGroup(group.id).success);
        QVERIFY(current->isChecked());
        QVERIFY(!current->isEnabled());
        for (const auto* key :
             {"groupSettingsSave", "groupSettingsCancel", "groupDelete", "groupRestoreDefaults"})
        {
            auto* button = child<QPushButton>(&dialog, key);
            QVERIFY(button->isVisible());
            QVERIFY(dialog.rect().contains(button->mapTo(&dialog, QPoint(0, 0))));
            QVERIFY(dialog.rect().contains(button->mapTo(&dialog, button->rect().bottomRight())));
        }
    }
    void deleteConfirmationCancelFailureAndSuccess()
    {
        Fixture f;
        const auto group = f.manager.createGroup(QStringLiteral("确认删除")).group;
        QVERIFY(picture().save(QDir(group.absolutePath).filePath(QStringLiteral("外部.png"))));
        f.manager.refresh();
        GroupSettingsDialog dialog(f.manager, group.id, f.trash);
        dialog.show();
        confirmDelete(false, 1);
        child<QPushButton>(&dialog, "groupDelete")->click();
        QCOMPARE(f.trashCalls, 0);
        QVERIFY(dialog.isVisible());
        f.failTrash = true;
        confirmDelete(true, 1);
        child<QPushButton>(&dialog, "groupDelete")->click();
        QVERIFY(dialog.isVisible());
        QVERIFY(
            child<QLabel>(&dialog, "groupSettingsError")->text().contains(QStringLiteral("失败")));
        QVERIFY(QFileInfo::exists(group.absolutePath));
        f.failTrash = false;
        confirmDelete(true, 1);
        child<QPushButton>(&dialog, "groupDelete")->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!QFileInfo::exists(group.absolutePath));
    }
    void managerOperationsPreserveIngestErrorAndSession()
    {
        Fixture f;
        const auto session = f.manager.beginSession();
        QVERIFY(f.manager.record(session, picture(), HistorySource::Overlay).success);
        QVERIFY(!f.manager.record({}, {}, HistorySource::Overlay).success);
        const auto error = f.manager.error();
        const auto group = f.manager.createGroup(QStringLiteral("错误隔离")).group;
        QVERIFY(f.manager.renameGroup(f.current(), QStringLiteral("默认改名")).success);
        QVERIFY(f.manager.saveCleanupPolicy(group.id, {}).success);
        QVERIFY(f.manager.setCurrentGroup(group.id).success);
        QVERIFY(f.manager.deleteGroup(group.id, f.trash).success);
        QCOMPARE(f.manager.error(), error);
        QVERIFY(f.manager.record(session, picture(), HistorySource::Overlay).success);
        QCOMPARE(f.store.scan().groups[0].items.size(), 1);
    }
    void trayCurrentMenuSyncGuardsAndRestart()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        RunOptions options;
        options.testMode = true;
        options.historyRoot = temp.filePath(QStringLiteral("history"));
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        QString selected;
        {
            ApplicationController controller(*qApp, options);
            auto& manager = ApplicationControllerTestAccess::history(controller);
            const auto original = manager.currentGroupId();
            selected = manager.createGroup(QStringLiteral("托盘 分组")).group.id;
            auto* menu = child<QMenu>(controller.menu(), "currentGroupMenu");
            const auto itemName = QStringLiteral("currentGroupItem_%1").arg(selected).toLatin1();
            auto* item = child<QAction>(menu, itemName.constData());
            QCOMPARE(menu->actions().first()->data().toString(), original);
            QVERIFY(menu->actions().first()->isChecked());
            QVERIFY(!item->isChecked());
            controller.openHistory();
            auto* window = ApplicationControllerTestAccess::window(controller);
            item->trigger();
            QCOMPARE(manager.currentGroupId(), selected);
            QVERIFY(item->isChecked());
            QVERIFY(!menu->actions().first()->isChecked());
            QCOMPARE(menu->title(), QStringLiteral("当前分组：托盘 分组"));
            QCOMPARE(ApplicationControllerTestAccess::window(controller), window);
            QVERIFY(child<QListWidget>(window, "historyGroups")
                        ->item(3)
                        ->text()
                        .startsWith(QStringLiteral("● ")));
            for (int state = 1; state <= 4; ++state)
            {
                ApplicationControllerTestAccess::guard(controller, state);
                QVERIFY(!menu->isEnabled());
                QVERIFY(!child<QAction>(controller.menu(), "openHistoryAction")->isEnabled());
                auto* groups = child<QListWidget>(window, "historyGroups");
                QVERIFY(!(groups->item(groups->count() - 1)->flags() & Qt::ItemIsEnabled));
                auto* combo = child<QComboBox>(window, "historyGroupCombo");
                combo->setCurrentIndex(combo->findData(QStringLiteral("new-group")));
                QVERIFY(!window->findChild<QDialog*>(QStringLiteral("historyGroupNameDialog")));
                menu->actions().first()->trigger();
                QCOMPARE(manager.currentGroupId(), selected);
            }
            ApplicationControllerTestAccess::guard(controller, 0);
            QVERIFY(menu->isEnabled());
            QVERIFY(manager.renameGroup(selected, QStringLiteral("新名称")).success);
            QCOMPARE(menu->title(), QStringLiteral("当前分组：新名称"));
            QStringList oldNames;
            for (auto* action : controller.menu()->actions())
                if (action->objectName() != QStringLiteral("openHistoryAction") &&
                    action->objectName() != QStringLiteral("currentGroupAction"))
                    oldNames.append(action->objectName());
            QCOMPARE(oldNames, (QStringList{QStringLiteral("screenshotAction"),
                                            QStringLiteral("scrollCaptureAction"),
                                            QStringLiteral("clipboardStickerAction"),
                                            QStringLiteral("settingsAction"),
                                            QStringLiteral("hideAllStickersAction"),
                                            QStringLiteral("restoreAllStickersAction"),
                                            QStringLiteral("quitAction")}));
        }
        HistoryManager restarted(AppSettings(options.settingsFile), options.historyRoot);
        QVERIFY(restarted.initialize().success);
        QCOMPARE(restarted.currentGroupId(), selected);
    }
    void policySettingsFailureAndRetryPreservesOtherPreferences()
    {
        Fixture f;
        const auto group = f.manager.createGroup(QStringLiteral("持久化失败")).group;
        auto preferences = f.settings.loadHistoryPreferences();
        preferences.rootDirectory = f.manager.rootDirectory();
        preferences.defaultCleanup.maxCount = 89;
        QVERIFY(f.settings.saveHistoryPreferences(preferences).isEmpty());
        const auto before = read(f.groupFile(group.id));
        const auto path = f.temp.filePath(QStringLiteral("settings.ini"));
        QVERIFY(QFile::rename(path, path + QStringLiteral(".backup")));
        QVERIFY(QDir().mkdir(path));
        GroupSettingsDialog dialog(f.manager, group.id, f.trash);
        dialog.show();
        child<QSpinBox>(&dialog, "groupMaxCount")->setValue(3);
        child<QCheckBox>(&dialog, "groupMakeCurrent")->setChecked(true);
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QVERIFY(dialog.isVisible());
        QVERIFY(
            child<QLabel>(&dialog, "groupSettingsError")->text().contains(QStringLiteral("失败")));
        QCOMPARE(read(f.groupFile(group.id)), before);
        QVERIFY(QDir().rmdir(path));
        QVERIFY(QFile::rename(path + QStringLiteral(".backup"), path));
        child<QPushButton>(&dialog, "groupSettingsSave")->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(f.current(), group.id);
        const auto after = f.settings.loadHistoryPreferences();
        QCOMPARE(after.rootDirectory, preferences.rootDirectory);
        QCOMPARE(cleanupPolicyToJson(after.defaultCleanup),
                 cleanupPolicyToJson(preferences.defaultCleanup));
    }
    void contextSettingsAndDefaultDeleteDisabled()
    {
        Fixture f;
        auto* window = f.open();
        bool inspected = false;
        menuAction(window, f.current(), "historyGroupDelete",
                   [&](QAction* action)
                   {
                       inspected = true;
                       QVERIFY(!action->isEnabled());
                   });
        QVERIFY(inspected);
        QTimer::singleShot(
            0, window,
            [window]
            {
                QTimer::singleShot(
                    0, window,
                    [window]
                    {
                        auto* dialog = child<GroupSettingsDialog>(window, "groupSettingsDialog");
                        QVERIFY(!child<QPushButton>(dialog, "groupDelete")->isEnabled());
                        dialog->reject();
                    });
            });
        menuAction(window, f.current(), "historyGroupSettings");
        const auto group = f.manager.createGroup(QStringLiteral("菜单删除")).group;
        QTimer::singleShot(0, window, [] { confirmDelete(true, 0); });
        menuAction(window, group.id, "historyGroupDelete");
        QVERIFY(!QFileInfo::exists(group.absolutePath));
        QCOMPARE(f.trashCalls, 1);
    }
    void controllerCleanupDuringGroupModal_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("命名") << 0;
        QTest::newRow("分组设置") << 1;
        QTest::newRow("删除确认") << 2;
    }
    void controllerCleanupDuringGroupModal()
    {
        QFETCH(int, mode);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        RunOptions options;
        options.testMode = true;
        options.historyRoot = temp.filePath(QStringLiteral("history"));
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        ApplicationController controller(*qApp, options);
        auto& manager = ApplicationControllerTestAccess::history(controller);
        const auto id = manager.createGroup(QStringLiteral("临时分组")).group.id;
        controller.openHistory();
        QPointer<HistoryWindow> w = ApplicationControllerTestAccess::window(controller);
        const auto cleanup = [&controller]
        { ApplicationControllerTestAccess::cleanup(controller); };
        if (mode == 0)
        {
            QTimer::singleShot(0, w, cleanup);
            auto* combo = child<QComboBox>(w, "historyGroupCombo");
            combo->setCurrentIndex(combo->findData(QStringLiteral("new-group")));
        }
        else
        {
            QTimer::singleShot(0, w, [w, cleanup] { QTimer::singleShot(0, w, cleanup); });
            menuAction(w, id, mode == 1 ? "historyGroupSettings" : "historyGroupDelete");
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(w.isNull());
        QVERIFY(!ApplicationControllerTestAccess::window(controller));
        QCOMPARE(manager.groups().size(), 2);
    }
    void controllerGroupDialogDisablesTray()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        RunOptions options;
        options.testMode = true;
        options.historyRoot = temp.filePath(QStringLiteral("history"));
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        ApplicationController controller(*qApp, options);
        controller.openHistory();
        auto* w = ApplicationControllerTestAccess::window(controller);
        QTimer::singleShot(
            0, w,
            [&controller, w]
            {
                auto* menu = child<QMenu>(controller.menu(), "currentGroupMenu");
                QVERIFY(!menu->isEnabled());
                QVERIFY(!child<QAction>(controller.menu(), "openHistoryAction")->isEnabled());
                child<QDialog>(w, "historyGroupNameDialog")->reject();
            });
        auto* combo = child<QComboBox>(w, "historyGroupCombo");
        combo->setCurrentIndex(combo->findData(QStringLiteral("new-group")));
        QVERIFY(child<QMenu>(controller.menu(), "currentGroupMenu")->isEnabled());
        QVERIFY(child<QAction>(controller.menu(), "openHistoryAction")->isEnabled());
    }
};
QTEST_MAIN(HistoryGroupsTest)
#include "history_groups_test.moc"
