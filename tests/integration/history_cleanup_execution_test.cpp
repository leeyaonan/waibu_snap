#include "app/history_manager.h"
#include "session/session_metrics.h"
#include "ui/group_settings_dialog.h"
#include "ui/history_window.h"
#include "ui/selection_overlay.h"
#include "ui/settings_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
namespace waibusnap
{
struct HistoryCleanupExecutionTestAccess
{
    static void removal(HistoryManager& manager, HistoryTrash callback)
    {
        manager.store_->permanentRemove_ = std::move(callback);
    }
};
}
namespace waibusnap
{
struct OverlayScrollTestAccess
{
    static void select(SelectionOverlay& overlay)
    {
        overlay.setSelection(QRect(2, 2, 60, 40));
        overlay.ensureToolbar();
        overlay.updateToolbar();
    }
};
}
namespace
{
constexpr qint64 day = 86400000;
QImage picture()
{
    QImage result(80, 60, QImage::Format_ARGB32);
    result.fill(QColor(40, 70, 120, 160));
    return result;
}
CleanupPolicy countPolicy(int count = 1)
{
    CleanupPolicy policy;
    policy.ageEnabled = policy.bytesEnabled = false;
    policy.maxCount = count;
    return policy;
}
QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("无法读取临时测试文件。");
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("无法写入临时测试文件。");
}
template <class T> T* child(QObject* root, const char* name)
{
    auto* result = root->findChild<T*>(QLatin1String(name));
    if (!result)
        qFatal("缺少测试控件：%s", name);
    return result;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    HistoryManager manager{settings, temp.filePath(QStringLiteral("历史 根"))};
    HistoryStore store{temp.filePath(QStringLiteral("历史 根"))};
    QString group;
    QString groupPath;
    Fixture()
    {
        if (!temp.isValid() || !manager.initialize().success)
            qFatal("临时历史库初始化失败，拒绝访问真实用户库。");
        group = manager.currentGroupId();
        groupPath = manager.currentGroup()->absolutePath;
    }
    QString index() const
    {
        return QDir(groupPath).filePath(QStringLiteral(".waibusnap/items.json"));
    }
    HistoryItem add(qint64 createdAt = 0, qint64 extraBytes = 0)
    {
        const auto written = store.writeManagedPng(group, picture(), HistorySource::Overlay);
        if (!written.success)
            qFatal("临时历史项写入失败。");
        auto document = QJsonDocument::fromJson(read(index())).object();
        auto entries = document.value(QStringLiteral("items")).toObject();
        auto entry = entries.value(written.item.fileName).toObject();
        entry.insert(QStringLiteral("createdAt"), createdAt);
        entries.insert(written.item.fileName, entry);
        document.insert(QStringLiteral("items"), entries);
        write(index(), QJsonDocument(document).toJson());
        if (extraBytes)
        {
            QFile file(written.item.absolutePath);
            if (!file.open(QIODevice::Append) ||
                file.write(QByteArray(extraBytes, 'x')) != extraBytes)
                qFatal("临时大小样本写入失败。");
        }
        manager.refresh();
        return *manager.findItem(historyItemId(written.item));
    }
    void policy(CleanupPolicy policy)
    {
        if (!store.saveCleanupPolicy(group, policy).success)
            qFatal("临时清理策略写入失败。");
    }
    QVector<HistoryItem> items() { return store.itemsForGroup(group).items; }
};
}
class HistoryCleanupExecutionTest final : public QObject
{
    Q_OBJECT
  private slots:
    void executesRules_data()
    {
        QTest::addColumn<int>("rule");
        QTest::newRow("age") << 0;
        QTest::newRow("count") << 1;
        QTest::newRow("bytes") << 2;
        QTest::newRow("or") << 3;
    }
    void executesRules()
    {
        QFETCH(int, rule);
        Fixture f;
        const auto a = f.add(day), b = f.add(39 * day), c = f.add(40 * day);
        CleanupPolicy policy;
        policy.ageEnabled = rule == 0 || rule == 3;
        policy.countEnabled = rule == 1 || rule == 3;
        policy.bytesEnabled = rule == 2 || rule == 3;
        policy.maxCount = 2;
        policy.maxBytes = (rule == 3 ? 1 : 2) * c.bytes;
        f.policy(policy);
        const auto result = f.manager.runCleanup(40 * day);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.deletedCount, rule == 3 ? 2 : 1);
        QCOMPARE(result.releasedBytes, result.deletedCount * a.bytes);
        QCOMPARE(QFileInfo::exists(a.absolutePath), false);
        QCOMPARE(QFileInfo::exists(b.absolutePath), rule != 3);
        QVERIFY(QFileInfo::exists(c.absolutePath));
        const auto index = QJsonDocument::fromJson(read(f.index()))
                               .object()
                               .value(QStringLiteral("items"))
                               .toObject();
        QCOMPARE(index.size(), 3 - result.deletedCount);
        QCOMPARE(f.items().size(), index.size());
        const auto usage = f.manager.totalUsage();
        QCOMPARE(usage.managedBytes, (3 - result.deletedCount) * c.bytes);
        QCOMPARE(usage.managedCount, index.size());
        QVERIFY(!result.protectedOverLimit);
    }
    void allProtectionsAndExternal()
    {
        Fixture f;
        const auto star = f.add(day), selected = f.add(2 * day), large = f.add(3 * day, 2000);
        const auto removable = f.add(4 * day), latest = f.add(5 * day);
        QVERIFY(f.store.setStarred(f.group, star.fileName, true).success);
        const auto external =
            QDir(f.manager.currentGroup()->absolutePath).filePath(QStringLiteral("用户另存.png"));
        QVERIFY(picture().save(external));
        auto policy = countPolicy();
        policy.bytesEnabled = true;
        policy.maxBytes = latest.bytes + 100;
        f.policy(policy);
        f.manager.setProtectedItem(historyItemId(selected));
        const auto result = f.manager.runCleanup(40 * day);
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.deletedCount, 1);
        QCOMPARE(result.protectedCount, 4);
        QCOMPARE(result.protectedBytes, star.bytes + selected.bytes + large.bytes + latest.bytes);
        QVERIFY(result.protectedOverLimit);
        QVERIFY(result.message().contains(QStringLiteral("受保护 4 张")));
        QVERIFY(!QFileInfo::exists(removable.absolutePath));
        for (const auto& item : {star, selected, large, latest})
            QVERIFY(QFileInfo::exists(item.absolutePath));
        QVERIFY(QFileInfo::exists(external));
        QCOMPARE(f.manager.totalUsage().externalCount, 1);
        QCOMPARE(f.manager.snapshot().groups[0].usage.protectedCount, 4);
        QVERIFY(!f.store.permanentlyDeleteManagedItem(latest, 40 * day).success);
        QVERIFY(!f.store.permanentlyDeleteManagedItem(star, 40 * day).success);
        QVERIFY(!f.store.permanentlyDeleteManagedItem(large, 40 * day).success);
        QVERIFY(!f.store.permanentlyDeleteManagedItem(selected, 40 * day, historyItemId(selected))
                     .success);
    }
    void rejectsForgedStaleAndLinked()
    {
        Fixture f;
        auto old = f.add(day);
        const auto latest = f.add(2 * day);
        f.policy(countPolicy());
        const QByteArray index = read(f.index());
        auto forged = old;
        forged.absolutePath = latest.absolutePath;
        QVERIFY(!f.store.permanentlyDeleteManagedItem(forged, 3 * day).success);
        forged = old;
        forged.bytes++;
        QVERIFY(!f.store.permanentlyDeleteManagedItem(forged, 3 * day).success);
        forged = old;
        forged.managed = false;
        QVERIFY(!f.store.permanentlyDeleteManagedItem(forged, 3 * day).success);
        const auto external =
            QDir(f.manager.currentGroup()->absolutePath).filePath(QStringLiteral("另存.png"));
        QVERIFY(picture().save(external));
        forged.fileName = QStringLiteral("另存.png");
        forged.absolutePath = external;
        forged.managed = true;
        QVERIFY(!f.store.permanentlyDeleteManagedItem(forged, 3 * day).success);
        const auto backup = f.temp.filePath(QStringLiteral("用户原图.png"));
        QVERIFY(QFile::rename(old.absolutePath, backup));
        // macOS 为符号链接；Windows QFile::link 可能是 .lnk，均不能成为扫描受管项。
        QVERIFY(QFile::link(backup, old.absolutePath));
        QVERIFY(!f.store.permanentlyDeleteManagedItem(old, 3 * day).success);
        QCOMPARE(read(f.index()), index);
        QVERIFY(QFileInfo::exists(backup));
        QVERIFY(QFileInfo::exists(external));
        QVERIFY(QFileInfo::exists(latest.absolutePath));
    }
    void staleIndexAndChangedProtectionFailClosed()
    {
        Fixture f;
        const auto old = f.add(day), latest = f.add(2 * day);
        f.policy(countPolicy());
        const auto original = read(f.index());
        auto document = QJsonDocument::fromJson(original).object();
        auto entries = document.value(QStringLiteral("items")).toObject();
        entries.remove(old.fileName);
        document.insert(QStringLiteral("items"), entries);
        write(f.index(), QJsonDocument(document).toJson());
        QVERIFY(!f.store.permanentlyDeleteManagedItem(old, 3 * day).success);
        write(f.index(), original);
        QVERIFY(f.store.setStarred(f.group, old.fileName, true).success);
        QVERIFY(!f.store.permanentlyDeleteManagedItem(old, 3 * day).success);
        QVERIFY(f.store.setStarred(f.group, old.fileName, false).success);
        auto disabled = countPolicy();
        disabled.enabled = false;
        f.policy(disabled);
        QVERIFY(!f.store.permanentlyDeleteManagedItem(old, 3 * day).success);
        QVERIFY(QFileInfo::exists(old.absolutePath));
        QVERIFY(QFileInfo::exists(latest.absolutePath));
    }
    void groupsKeepIndependentPolicies()
    {
        Fixture f;
        const auto old = f.add(day), latest = f.add(2 * day);
        f.policy(countPolicy());
        const auto created = f.manager.createGroup(QStringLiteral("保留全部")).group;
        const auto a = f.store.writeManagedPng(created.id, picture(), HistorySource::Overlay);
        const auto b = f.store.writeManagedPng(created.id, picture(), HistorySource::Overlay);
        QVERIFY(a.success && b.success);
        auto disabled = countPolicy();
        disabled.enabled = false;
        QVERIFY(f.store.saveCleanupPolicy(created.id, disabled).success);
        const auto report = f.manager.runCleanup(3 * day);
        QCOMPARE(report.deletedCount, 1);
        QVERIFY(!QFileInfo::exists(old.absolutePath));
        QVERIFY(QFileInfo::exists(latest.absolutePath));
        QVERIFY(QFileInfo::exists(a.item.absolutePath));
        QVERIFY(QFileInfo::exists(b.item.absolutePath));
        QCOMPARE(f.manager.groups().size(), 2);
        QCOMPARE(f.manager.totalUsage().managedCount, 3);
    }
    void failureStopsAfterFirstAndKeepsFollowing()
    {
        Fixture f;
        const auto a = f.add(day), b = f.add(2 * day), c = f.add(3 * day), d = f.add(4 * day);
        f.policy(countPolicy());
        int attempts = 0;
        HistoryCleanupExecutionTestAccess::removal(
            f.manager,
            [&](const QString& path)
            {
                if (++attempts == 2)
                    return HistoryStatus{false, QStringLiteral("模拟文件占用，删除失败")};
                return HistoryStatus{QFile::remove(path), {}};
            });
        const auto result = f.manager.runCleanup(5 * day);
        QCOMPARE(attempts, 2);
        QCOMPARE(result.deletedCount, 1);
        QVERIFY(result.error.contains(QStringLiteral("模拟文件占用")));
        QVERIFY(!QFileInfo::exists(a.absolutePath));
        for (const auto& item : {b, c, d})
            QVERIFY(QFileInfo::exists(item.absolutePath));
        QCOMPARE(f.items().size(), 3);
        const auto entries = QJsonDocument::fromJson(read(f.index()))
                                 .object()
                                 .value(QStringLiteral("items"))
                                 .toObject();
        QVERIFY(!entries.contains(a.fileName));
        QVERIFY(entries.contains(b.fileName));
        QCOMPARE(f.manager.totalUsage().managedBytes, b.bytes + c.bytes + d.bytes);
        QVERIFY(f.manager.error().isEmpty());
    }
    void indexCommitFailureRestoresOriginal()
    {
        Fixture f;
        const auto a = f.add(day), b = f.add(2 * day), c = f.add(3 * day);
        const auto original = read(a.absolutePath), originalIndex = read(f.index());
        f.policy(countPolicy());
        int attempts = 0;
        const auto savedIndex = f.index() + QStringLiteral(".recover");
        HistoryCleanupExecutionTestAccess::removal(
            f.manager,
            [&](const QString& path)
            {
                ++attempts;
                if (!QFile::rename(f.index(), savedIndex) || !QDir().mkdir(f.index()))
                    qFatal("无法注入索引提交故障。");
                return HistoryStatus{QFile::remove(path), {}};
            });
        const auto result = f.manager.runCleanup(4 * day);
        QCOMPARE(attempts, 1);
        QCOMPARE(result.deletedCount, 0);
        QVERIFY(result.error.contains(QStringLiteral("索引提交失败")));
        QCOMPARE(read(a.absolutePath), original);
        QCOMPARE(read(savedIndex), originalIndex);
        QVERIFY(QDir().rmdir(f.index()));
        QVERIFY(QFile::rename(savedIndex, f.index()));
        QCOMPARE(f.items().size(), 3);
        QVERIFY(QFileInfo::exists(b.absolutePath));
        QVERIFY(QFileInfo::exists(c.absolutePath));
    }
    void scanAndLockFailuresNeverDelete()
    {
        Fixture f;
        const auto a = f.add(day), b = f.add(2 * day);
        f.policy(countPolicy());
        const auto originalIndex = read(f.index());
        QLockFile lock(QDir(f.manager.rootDirectory()).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock());
        QVERIFY(!f.manager.runCleanup().error.isEmpty());
        lock.unlock();
        write(f.index(), QByteArray("broken"));
        QVERIFY(!f.manager.runCleanup().error.isEmpty());
        QVERIFY(QFileInfo::exists(a.absolutePath));
        QVERIFY(QFileInfo::exists(b.absolutePath));
        write(f.index(), originalIndex);
        QCOMPARE(f.manager.runCleanup(3 * day).deletedCount, 1);
    }
    void startupRecordStarAndPolicyTriggers()
    {
        Fixture f;
        const auto old = f.add(day), latest = f.add(2 * day);
        f.policy(countPolicy());
        HistoryManager restart(f.settings, f.manager.rootDirectory());
        QSignalSpy report(&restart, &HistoryManager::cleanupFinished);
        QVERIFY(restart.initialize().success);
        QCOMPARE(report.count(), 1);
        QCOMPARE(restart.lastCleanup().deletedCount, 1);
        QVERIFY(!QFileInfo::exists(old.absolutePath));
        const auto session = restart.beginSession();
        const auto recorded = restart.record(session, picture(), HistorySource::Overlay);
        QVERIFY(recorded.success);
        QVERIFY(!QFileInfo::exists(latest.absolutePath));
        QCOMPARE(restart.snapshot().groups[0].items.size(), 1);
        QCOMPARE(restart.lastCleanup().deletedCount, 1);
        QVERIFY(restart.setStarred(historyItemId(recorded.item), true).success);
        const auto newer =
            restart.record(restart.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(newer.success);
        QVERIFY(restart.lastCleanup().protectedOverLimit);
        QVERIFY(restart.setStarred(historyItemId(recorded.item), false).success);
        QVERIFY(!QFileInfo::exists(recorded.item.absolutePath));
        QCOMPARE(restart.lastCleanup().deletedCount, 1);
        auto disabled = countPolicy();
        disabled.enabled = false;
        QVERIFY(restart.saveCleanupPolicy(f.group, disabled).success);
        QVERIFY(restart.record(restart.beginSession(), picture(), HistorySource::Overlay).success);
        QCOMPARE(restart.snapshot().groups[0].items.size(), 2);
        QVERIFY(restart.saveCleanupPolicy(f.group, countPolicy()).success);
        QCOMPARE(restart.lastCleanup().deletedCount, 1);
        QCOMPARE(restart.snapshot().groups[0].items.size(), 1);
    }
    void ingestKeepsNewestWhenClockFallsBehind()
    {
        Fixture f;
        // 同毫秒 / 时钟回拨都不能让刚写入的副本因稳定 ID 排序而立即被清理。
        const auto previous = f.add(QDateTime::currentMSecsSinceEpoch() + day);
        f.policy(countPolicy());
        const auto written =
            f.manager.record(f.manager.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(written.success);
        QVERIFY(QFileInfo::exists(written.item.absolutePath));
        QVERIFY(!QFileInfo::exists(previous.absolutePath));
        QVERIFY(written.item.createdAtMs > previous.createdAtMs);
        QCOMPARE(f.items().size(), 1);
    }
    void cleanupErrorDoesNotFailIngestOrPolicy()
    {
        Fixture f;
        const auto a = f.add(day);
        f.policy(countPolicy());
        HistoryCleanupExecutionTestAccess::removal(
            f.manager, [](const QString&)
            { return HistoryStatus{false, QStringLiteral("文件占用，稍后重试")}; });
        const auto result =
            f.manager.record(f.manager.beginSession(), picture(), HistorySource::Overlay);
        QVERIFY(result.success);
        QVERIFY(!f.manager.lastCleanup().error.isEmpty());
        QVERIFY(f.manager.error().isEmpty());
        QVERIFY(f.manager.saveCleanupPolicy(f.group, countPolicy()).success);
        QVERIFY(QFileInfo::exists(a.absolutePath));
        QCOMPARE(f.items().size(), 2);
    }
    void outputSurvivesCleanupFailure_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::newRow("copy") << QStringLiteral("copyButton");
        QTest::newRow("pin") << QStringLiteral("pinButton");
        QTest::newRow("save") << QStringLiteral("save");
    }
    void outputSurvivesCleanupFailure()
    {
        QFETCH(QString, operation);
        Fixture f;
        const auto old = f.add(day);
        f.policy(countPolicy());
        HistoryCleanupExecutionTestAccess::removal(
            f.manager, [](const QString&)
            { return HistoryStatus{false, QStringLiteral("文件正在使用，清理失败")}; });
        const auto session = f.manager.beginSession();
        OverlayActions actions;
        actions.historySessionId = session.id;
        actions.historyGroupId = session.groupId;
        actions.recordHistory = [&](const QImage& image, const QString&)
        { return f.manager.record(session, image, HistorySource::Overlay); };
        QImage output;
        actions.copyImage = actions.pinImage = [&](const QImage& image)
        {
            output = image;
            return ImageOutputResult{true, {}};
        };
        CaptureFrame frame{{1, QRect(0, 0, 80, 60), 1, 1, QRect(0, 0, 80, 60)}, picture(), 0};
        SelectionOverlay overlay(frame, actions);
        overlay.show();
        OverlayScrollTestAccess::select(overlay);
        QSignalSpy finished(&overlay, &SelectionOverlay::finished);
        const auto path = f.temp.filePath(QStringLiteral("用户输出.png"));
        if (operation == QStringLiteral("save"))
        {
            QVERIFY(overlay.exportToPath(path));
            QVERIFY(overlay.isSelectionSaved());
            QVERIFY(QFileInfo::exists(path));
            QVERIFY(finished.isEmpty());
        }
        else
        {
            child<QPushButton>(&overlay, operation.toLatin1().constData())->click();
            QVERIFY(!output.isNull());
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(1).toInt(), operation == QStringLiteral("copyButton")
                                                         ? copiedSessionOutcome
                                                         : pinnedSessionOutcome);
        }
        QCOMPARE(f.items().size(), 2);
        QVERIFY(QFileInfo::exists(old.absolutePath));
        QVERIFY(f.manager.lastCleanup().error.contains(QStringLiteral("文件正在使用")));
        QVERIFY(f.manager.error().isEmpty());
    }
    void windowSelectionNoticeAndUsageRefresh()
    {
        Fixture f;
        const auto a = f.add(day), b = f.add(2 * day), c = f.add(3 * day);
        HistoryWindow window(f.manager, f.settings);
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        auto* list = child<QListWidget>(&window, "historyThumbnails");
        list->setCurrentRow(1);
        QCOMPARE(window.selectedItemId(), historyItemId(b));
        QCOMPARE(f.manager.protectedItemId(), historyItemId(b));
        GroupSettingsDialog group(f.manager, f.group, {}, historyItemId(b));
        group.show();
        QVERIFY(child<QLabel>(&group, "groupCleanupExplanation")
                    ->text()
                    .contains(QStringLiteral("永久删除")));
        SettingsDialog settings(QKeySequence(Qt::Key_F1), {});
        QVERIFY(child<QLabel>(&settings, "historyCleanupExplanation")
                    ->text()
                    .contains(QStringLiteral("永久删除")));
        connect(&f.manager, &HistoryManager::groupsChanged, &settings,
                [&] {
                    settings.updateHistoryUsage(f.manager.totalUsage(),
                                                f.manager.lastCleanup().message());
                });
        QVERIFY(f.manager.saveCleanupPolicy(f.group, countPolicy()).success);
        QVERIFY(!QFileInfo::exists(a.absolutePath));
        QVERIFY(QFileInfo::exists(c.absolutePath));
        QCOMPARE(window.selectedItemId(), historyItemId(b));
        QVERIFY(child<QLabel>(&window, "historyNotice")
                    ->text()
                    .contains(QStringLiteral("受保护 2 张")));
        QVERIFY(child<QLabel>(&window, "historyStatus")
                    ->text()
                    .contains(QStringLiteral("受保护 2 张")));
        QVERIFY(
            child<QLabel>(&group, "groupUsage")->text().contains(QStringLiteral("当前占用：2 张")));
        QVERIFY(child<QLabel>(&settings, "historyTotalUsage")
                    ->text()
                    .contains(QStringLiteral("受管：2 张")));
        window.resize(640, 760);
        QCoreApplication::processEvents();
        QVERIFY(child<QLabel>(&window, "historyNotice")->isVisible());
        auto* usage = child<QLabel>(&group, "groupUsage");
        auto* explanation = child<QLabel>(&group, "groupCleanupExplanation");
        // 各平台首行字高与后续行距不同，按 QLabel 当前宽度的文本排版测量，避免 3×行距高估。
        const int requiredUsageHeight = usage->heightForWidth(usage->width());
        QVERIFY(requiredUsageHeight > 0);
        QVERIFY2(usage->height() >= requiredUsageHeight,
                 qPrintable(QStringLiteral("占用标签高度 %1，小于完整文字高度 %2")
                                .arg(usage->height())
                                .arg(requiredUsageHeight)));
        QVERIFY2(explanation->height() >= explanation->heightForWidth(explanation->width()),
                 "自动清理说明应完整可见");
        const auto evidence = qEnvironmentVariable("WAIBUSNAP_CLEANUP_EVIDENCE_DIR");
        if (!evidence.isEmpty())
        {
            QVERIFY(QDir().mkpath(evidence));
            const auto suffix = QString::number(window.devicePixelRatioF());
            QVERIFY(window.grab().save(
                QDir(evidence).filePath(QStringLiteral("history-%1.png").arg(suffix))));
            QVERIFY(group.grab().save(
                QDir(evidence).filePath(QStringLiteral("group-%1.png").arg(suffix))));
            child<QTabWidget>(&settings, "settingsTabs")->setCurrentIndex(4);
            settings.show();
            QCoreApplication::processEvents();
            QVERIFY(settings.grab().save(
                QDir(evidence).filePath(QStringLiteral("settings-%1.png").arg(suffix))));
        }
        window.close();
        QVERIFY(f.manager.protectedItemId().isEmpty());
        QCOMPARE(f.manager.runCleanup(4 * day).deletedCount, 1);
    }
    void lockedScanKeepsWindowSelection()
    {
        Fixture f;
        const auto old = f.add(day), latest = f.add(2 * day);
        f.policy(countPolicy());
        HistoryWindow window(f.manager, f.settings);
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        child<QListWidget>(&window, "historyThumbnails")->setCurrentRow(1);
        QCOMPARE(window.selectedItemId(), historyItemId(old));
        QLockFile lock(QDir(f.manager.rootDirectory()).filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock());
        QVERIFY(!f.manager.runCleanup(3 * day).error.isEmpty());
        QCOMPARE(window.selectedItemId(), historyItemId(old));
        QCOMPARE(child<QListWidget>(&window, "historyThumbnails")->count(), 2);
        QVERIFY(child<QLabel>(&window, "historyNotice")->text().contains(QStringLiteral("锁文件")));
        QVERIFY(QFileInfo::exists(old.absolutePath));
        QVERIFY(QFileInfo::exists(latest.absolutePath));
        lock.unlock();
        QCOMPARE(f.manager.runCleanup(3 * day).deletedCount, 0);
    }
    void missingSelectionFallsBackAndErrorNotice()
    {
        Fixture f;
        const auto a = f.add(day), b = f.add(2 * day), c = f.add(3 * day);
        HistoryWindow window(f.manager, f.settings);
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        auto* list = child<QListWidget>(&window, "historyThumbnails");
        list->setCurrentRow(1);
        QCOMPARE(window.selectedItemId(), historyItemId(b));
        QVERIFY(QFile::remove(b.absolutePath));
        f.manager.runCleanup(4 * day);
        QVERIFY(window.selectedItemId() != historyItemId(b));
        QVERIFY(!window.selectedItemId().isEmpty());
        f.policy(countPolicy());
        // 第一项 a 可清理，当前最新 c 受保护。
        list->setCurrentRow(0);
        HistoryCleanupExecutionTestAccess::removal(
            f.manager, [](const QString&)
            { return HistoryStatus{false, QStringLiteral("中文删除失败原因")}; });
        const auto report = f.manager.runCleanup(4 * day);
        QVERIFY(report.error.contains(QStringLiteral("中文删除失败原因")));
        QVERIFY(child<QLabel>(&window, "historyNotice")
                    ->text()
                    .contains(QStringLiteral("中文删除失败原因")));
        QVERIFY(QFileInfo::exists(a.absolutePath));
        QVERIFY(QFileInfo::exists(c.absolutePath));
    }
};
QTEST_MAIN(HistoryCleanupExecutionTest)
#include "history_cleanup_execution_test.moc"
