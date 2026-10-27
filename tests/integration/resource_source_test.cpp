#include "app/application_controller.h"
#include "ui/history_resource_source.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
namespace
{
QImage picture(QColor color = Qt::red)
{
    QImage image(120, 80, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    HistoryManager manager{settings, temp.filePath(QStringLiteral("历史"))};
    HistoryStore store{temp.filePath(QStringLiteral("历史"))};
    QImage copied, pinned;
    QString revealed;
    HistoryWindowActions actions;
    Fixture()
    {
        if (!temp.isValid() || !manager.initialize().success)
            qFatal("临时历史库初始化失败。");
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
        actions.trash = [](const QString& path) { return HistoryStatus{QFile::remove(path), {}}; };
        actions.reveal = [this](const QString& path)
        {
            revealed = path;
            return HistoryStatus{true, {}};
        };
    }
    HistoryItem record(HistorySource kind = HistorySource::Overlay, bool original = false)
    {
        const auto result = manager.record(manager.beginSession(), picture(), kind,
                                           original ? picture(Qt::blue) : QImage());
        if (!result.success)
            qFatal("临时记录入库失败。");
        return result.item;
    }
};
}
class ResourceSourceTest final : public QObject
{
    Q_OBJECT
  private slots:
    void disabledHistoryStillOpensBrowser()
    {
        QTemporaryDir temp;
        RunOptions options;
        options.testMode = true;
        options.settingsFile = temp.filePath(QStringLiteral("settings.ini"));
        options.historyRoot = temp.filePath(QStringLiteral("历史"));
        options.historyEnabledOverride = false;
        ApplicationController controller(*qApp, options);
        controller.openHistory();
        HistoryWindow* browser = nullptr;
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->objectName() == QStringLiteral("historyWindow"))
                browser = qobject_cast<HistoryWindow*>(widget);
        QVERIFY(browser);
        QVERIFY(!QFileInfo::exists(options.historyRoot));
        browser->close();
    }
    void identityCapabilitiesAndVisibility()
    {
        Fixture f;
        HistoryResourceSource source(f.manager, f.actions);
        ResourceSource& api = source;
        QCOMPARE(api.id(), QStringLiteral("history"));
        QCOMPARE(api.displayName(), QStringLiteral("历史"));
        QVERIFY(api.visible());
        const auto caps = api.capabilities();
        QVERIFY(caps.groups && caps.cleanup && caps.originalVariant && caps.star);
        QVERIFY(caps.deleteItem && caps.reveal);
        QVERIFY(api.canCopy() && api.canPin());
        HistoryResourceSource unavailableActions(f.manager);
        QVERIFY(!unavailableActions.capabilities().deleteItem);
        QVERIFY(!unavailableActions.capabilities().reveal);
        QVERIFY(!unavailableActions.canCopy() && !unavailableActions.canPin());
        HistoryManager uninitialized(f.settings, f.temp.filePath(QStringLiteral("未初始化")));
        HistoryResourceSource hidden(uninitialized);
        QVERIFY(!hidden.visible());
        hidden.refresh();
        QVERIFY(!hidden.snapshot().error.isEmpty());
        QVERIFY(!QFileInfo::exists(uninitialized.rootDirectory()));
    }
    void snapshotMapsAllFieldsAndUsage()
    {
        Fixture f;
        const auto first = f.record(HistorySource::Scroll, true);
        QVERIFY(f.manager.setStarred(historyItemId(first), true).success);
        const auto custom = f.manager.createGroup(QStringLiteral("课程"));
        QVERIFY(custom.success);
        const auto externalPath =
            QDir(custom.group.absolutePath).filePath(QStringLiteral("外部.jpg"));
        QVERIFY(picture(Qt::green).save(externalPath));
        HistoryResourceSource source(f.manager, f.actions);
        source.refresh();
        const auto snapshot = source.snapshot();
        QCOMPARE(snapshot.groups.size(), 2);
        QCOMPARE(snapshot.items.size(), 2);
        QVERIFY(snapshot.error.isEmpty());
        bool sawManaged = false, sawExternal = false;
        for (const auto& item : snapshot.items)
        {
            const auto history = f.manager.findItem(item.id);
            QVERIFY(history);
            QCOMPARE(item.id, historyItemId(*history));
            QCOMPARE(item.sourceId, source.id());
            QCOMPARE(item.groupId, history->groupId);
            QCOMPARE(item.fileName, history->fileName);
            QCOMPARE(item.absolutePath, history->absolutePath);
            QCOMPARE(item.bytes, history->bytes);
            QCOMPARE(item.originalBytes, history->originalBytes);
            QCOMPARE(item.hasOriginal, history->hasOriginal);
            QCOMPARE(item.starred, history->starred);
            QCOMPARE(item.managed, history->managed);
            QCOMPARE(item.deletable, history->managed);
            QCOMPARE(item.createdAtMs, history->createdAtMs);
            QCOMPARE(item.pixelSize, history->pixelSize);
            if (item.managed)
            {
                sawManaged = true;
                QVERIFY(item.badges.contains(QStringLiteral("长图")));
                QVERIFY(item.badges.contains(QStringLiteral("水印/原图")));
            }
            else
            {
                sawExternal = true;
                QVERIFY(item.badges.contains(QStringLiteral("外部图片")));
            }
        }
        QVERIFY(sawManaged && sawExternal);
        for (const auto& group : snapshot.groups)
        {
            QCOMPARE(group.isCurrent, group.id == f.manager.currentGroupId());
            for (const auto& history : f.manager.groups())
                if (history.id == group.id)
                {
                    QCOMPARE(group.name, history.name);
                    QCOMPARE(group.isDefault, history.isDefault);
                    QCOMPARE(group.absolutePath, history.absolutePath);
                }
        }
        QCOMPARE(snapshot.usage.managedCount, 1);
        QCOMPARE(snapshot.usage.externalCount, 1);
        QCOMPARE(snapshot.usage.starredCount, 1);
        QCOMPARE(snapshot.usage.managedBytes, first.bytes + first.originalBytes);
        QCOMPARE(snapshot.usage.externalBytes, QFileInfo(externalPath).size());
        QCOMPARE(snapshot.groupUsage.value(first.groupId).managedCount, 1);
        QCOMPARE(snapshot.groupUsage.value(custom.group.id).externalCount, 1);
    }
    void imageVariantsOutputsAndReveal()
    {
        Fixture f;
        const auto record = f.record(HistorySource::Overlay, true);
        HistoryResourceSource source(f.manager, f.actions);
        const auto id = historyItemId(record);
        const auto thumbnail = source.loadImage(id, {30, 30});
        QVERIFY(thumbnail.success);
        QCOMPARE(thumbnail.image.size(), QSize(30, 20));
        QCOMPARE(source.loadImage(id).image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(source.loadImage(id, {}, true).image.pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(source.outputImage(id, true, false).success);
        QCOMPARE(f.copied.pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(source.outputImage(id, false, true).success);
        QCOMPARE(f.pinned.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(source.revealItem(id).success);
        QCOMPARE(f.revealed, record.absolutePath);
        QVERIFY(!source.loadImage(QStringLiteral("../../任意文件")).success);
        QVERIFY(!source.revealItem(QStringLiteral("不存在")).success);
        HistoryResourceSource unavailable(f.manager);
        QVERIFY(!unavailable.outputImage(id, false, false).success);
        QVERIFY(!unavailable.snapshot().items.first().deletable);
        QVERIFY(!unavailable.deleteItem(id).success);
        QVERIFY(QFileInfo::exists(record.absolutePath));
    }
    void mutationsNotifyAndRefreshIsSilent()
    {
        Fixture f;
        HistoryResourceSource source(f.manager, f.actions);
        QSignalSpy changed(&source, &ResourceSource::changed);
        QSignalSpy items(&f.manager, &HistoryManager::itemsChanged);
        source.refresh();
        source.snapshot();
        f.manager.refresh();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(items.count(), 0);
        const auto record = f.record();
        QCOMPARE(items.count(), 1);
        QVERIFY(changed.count() > 0);
        QCOMPARE(source.snapshot().items.size(), 1);
        items.clear();
        changed.clear();
        QVERIFY(source.setStarred(historyItemId(record), true).success);
        QCOMPARE(items.count(), 1);
        QVERIFY(changed.count() > 0);
        QVERIFY(source.snapshot().items.first().starred);
        items.clear();
        changed.clear();
        QVERIFY(source.deleteItem(historyItemId(record)).success);
        QCOMPARE(items.count(), 1);
        QCOMPARE(changed.count(), 1);
        QVERIFY(source.snapshot().items.isEmpty());
        items.clear();
        changed.clear();
        QVERIFY(!source.deleteItem(historyItemId(record)).success);
        QVERIFY(!source.setStarred(historyItemId(record), true).success);
        QVERIFY(!f.manager.record({}, {}, HistorySource::Overlay).success);
        QCOMPARE(items.count(), 0);
        QCOMPARE(changed.count(), 0);
    }
    void protectionAndCleanupNotice()
    {
        Fixture f;
        const auto old = f.record();
        QTest::qWait(2);
        const auto newest = f.record();
        HistoryResourceSource source(f.manager, f.actions);
        source.setProtectedItem(historyItemId(old));
        QCOMPARE(f.manager.protectedItemId(), historyItemId(old));
        QCOMPARE(source.snapshot().usage.protectedCount, 2);
        CleanupPolicy policy;
        policy.ageEnabled = false;
        policy.bytesEnabled = false;
        policy.maxCount = 1;
        QVERIFY(f.manager.saveCleanupPolicy(old.groupId, policy).success);
        QVERIFY(source.snapshot().notice.contains(QStringLiteral("受保护 2 张")));
        source.setProtectedItem({});
        QSignalSpy items(&f.manager, &HistoryManager::itemsChanged);
        const auto report = f.manager.runCleanup();
        QCOMPARE(report.deletedCount, 1);
        QCOMPARE(items.count(), 1);
        QCOMPARE(source.snapshot().items.size(), 1);
        QCOMPARE(source.snapshot().items.first().id, historyItemId(newest));
        QCOMPARE(source.snapshot().notice, report.message());
    }
    void scanFailureKeepsKnownItemsAndReportsError()
    {
        Fixture f;
        const auto record = f.record();
        HistoryResourceSource source(f.manager, f.actions);
        source.setProtectedItem(historyItemId(record));
        QLockFile lock(QDir(f.manager.rootDirectory()).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock(0));
        QSignalSpy changed(&source, &ResourceSource::changed);
        source.refresh();
        QCOMPARE(changed.count(), 0);
        const auto snapshot = source.snapshot();
        QVERIFY(!snapshot.error.isEmpty());
        QCOMPARE(snapshot.items.size(), 1);
        QCOMPARE(snapshot.items.first().id, historyItemId(record));
        QCOMPARE(f.manager.protectedItemId(), historyItemId(record));
    }
    void sortingMatchesHistoryIncludingTies()
    {
        QVector<HistoryItem> history;
        QVector<ResourceItem> resources;
        for (const auto& name :
             {QStringLiteral("b.png"), QStringLiteral("a.png"), QStringLiteral("c.png")})
        {
            HistoryItem item;
            item.groupId = QStringLiteral("g");
            item.fileName = name;
            item.createdAtMs = name == QStringLiteral("c.png") ? 2 : 1;
            history.append(item);
            ResourceItem mapped;
            mapped.id = historyItemId(item);
            mapped.createdAtMs = item.createdAtMs;
            resources.append(mapped);
        }
        for (const bool newest : {true, false})
        {
            const auto expected = sortedHistoryItems(history, newest ? HistorySort::NewestFirst
                                                                     : HistorySort::OldestFirst);
            const auto actual = sortedResourceItems(resources, newest ? ResourceSort::NewestFirst
                                                                      : ResourceSort::OldestFirst);
            for (qsizetype i = 0; i < actual.size(); ++i)
                QCOMPARE(actual[i].id, historyItemId(expected[i]));
        }
    }
};
QTEST_MAIN(ResourceSourceTest)
#include "resource_source_test.moc"
