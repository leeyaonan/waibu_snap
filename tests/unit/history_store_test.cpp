#include "app/app_settings.h"
#include "app/history_store.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <algorithm>
using namespace waibusnap;
namespace
{
bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QString marker(const HistoryGroup& group, const QString& name)
{
    return QDir(QDir(group.absolutePath).filePath(QStringLiteral(".waibusnap"))).filePath(name);
}
QImage image(QRgb color = qRgba(30, 80, 150, 123))
{
    QImage pixels(17, 11, QImage::Format_ARGB32);
    pixels.fill(color);
    pixels.setPixel(1, 2, qRgba(255, 40, 90, 0));
    pixels.setPixel(3, 4, qRgba(10, 200, 60, 255));
    pixels.setDevicePixelRatio(2);
    return pixels;
}
QImage physicalPixels(const QImage& image)
{
    QImage result = image.convertToFormat(QImage::Format_ARGB32);
    result.setDevicePixelRatio(1);
    return result;
}
bool copyGroup(const HistoryGroup& group, const QString& target)
{
    if (!QDir().mkpath(QDir(target).filePath(QStringLiteral(".waibusnap"))))
        return false;
    for (const auto* name : {"group.json", "items.json"})
        if (!QFile::copy(marker(group, QLatin1String(name)),
                         QDir(QDir(target).filePath(QStringLiteral(".waibusnap")))
                             .filePath(QLatin1String(name))))
            return false;
    return true;
}
}
class HistoryStoreTest final : public QObject
{
    Q_OBJECT
  private slots:
    void defaultGroupAndOrdinaryFolders()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString root = temporary.filePath(QStringLiteral("历史 根目录"));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("默认分组"))));
        const QString userFile = QDir(root).filePath(QStringLiteral("默认分组/用户文件.txt"));
        QVERIFY(writeBytes(userFile, "用户文件"));
        QVERIFY(QDir(root).mkdir(QStringLiteral("普通目录")));
        HistoryStore store(root);
        auto result = store.initialize();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.groups.size(), 1);
        const auto group = result.groups[0].group;
        QCOMPARE(group.name, QStringLiteral("默认分组-2"));
        QVERIFY(group.isDefault);
        QVERIFY(!QUuid(group.id).isNull());
        QCOMPARE(readBytes(userFile), QByteArray("用户文件"));
        QVERIFY(
            !QFile::exists(QDir(root).filePath(QStringLiteral("默认分组/.waibusnap/group.json"))));
        QVERIFY(copyGroup(group, QDir(root).filePath(QStringLiteral(".hidden"))));
        result = store.scan();
        QCOMPARE(result.groups.size(), 1);
        const auto deleted = store.deleteGroup(group.id);
        QVERIFY(!deleted.success);
        QVERIFY(deleted.error.contains(QStringLiteral("默认")));
        const auto renamed = store.renameGroup(group.id, QStringLiteral("  我的默认组  "));
        QVERIFY2(renamed.success, qPrintable(renamed.error));
        QCOMPARE(renamed.group.id, group.id);
        QCOMPARE(renamed.group.name, QStringLiteral("我的默认组"));
        const auto reopened = HistoryStore(root).initialize();
        QVERIFY(reopened.error.isEmpty());
        QCOMPARE(reopened.groups.size(), 1);
        QCOMPARE(reopened.groups[0].group.id, group.id);
        QVERIFY(reopened.groups[0].group.isDefault);
    }
    void createRenameAndPolicyPersistence()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        CleanupPolicy defaults;
        defaults.maxCount = 7;
        HistoryStore store(temporary.path(), defaults);
        QVERIFY(store.initialize().error.isEmpty());
        const auto created = store.createGroup(QStringLiteral("  Notes  "));
        QVERIFY2(created.success, qPrintable(created.error));
        QCOMPARE(created.group.cleanup.maxCount, 7);
        QVERIFY(!store.createGroup(QStringLiteral("notes")).success);
        QVERIFY(QDir(temporary.path()).mkdir(QStringLiteral("用户目录")));
        QVERIFY(!store.renameGroup(created.group.id, QStringLiteral("用户目录")).success);
        QVERIFY(!store.renameGroup(created.group.id, QStringLiteral("../逃逸")).success);
        const auto renamed = store.renameGroup(created.group.id, QStringLiteral("课程"));
        QVERIFY(renamed.success);
        QCOMPARE(renamed.group.id, created.group.id);
        QCOMPARE(renamed.group.createdAtMs, created.group.createdAtMs);
        CleanupPolicy changed;
        changed.maxCount = 20;
        changed.enabled = false;
        QVERIFY(store.saveCleanupPolicy(created.group.id, changed).success);
        const auto scanned = store.scan();
        QVERIFY(scanned.error.isEmpty());
        bool found = false;
        for (const auto& snapshot : scanned.groups)
            if (snapshot.group.id == created.group.id)
            {
                found = true;
                QCOMPARE(snapshot.group.name, QStringLiteral("课程"));
                QCOMPARE(cleanupPolicyToJson(snapshot.group.cleanup), cleanupPolicyToJson(changed));
            }
        QVERIFY(found);
        CleanupPolicy invalid;
        invalid.ageEnabled = invalid.countEnabled = invalid.bytesEnabled = false;
        const auto before = readBytes(marker(renamed.group, QStringLiteral("group.json")));
        QVERIFY(!store.saveCleanupPolicy(created.group.id, invalid).success);
        QCOMPARE(readBytes(marker(renamed.group, QStringLiteral("group.json"))), before);
        QVERIFY(!store.itemsForGroup(QStringLiteral("丢失 UUID")).success);
        QVERIFY(!store.createGroup(QStringLiteral("NUL.png")).success);
        // 新的全局模板不会覆盖磁盘中已有分组的独立策略。
        HistoryStore other(temporary.path());
        QVERIFY(other.initialize().error.isEmpty());
        QCOMPARE(other.itemsForGroup(created.group.id).success, true);
        const auto newer = other.createGroup(QStringLiteral("新组"));
        QVERIFY(newer.success);
        QCOMPARE(newer.group.cleanup.maxCount, 100);
    }
    void independentGroupDirectoryAndMove()
    {
        QTemporaryDir temporary;
        QTemporaryDir external;
        QVERIFY(temporary.isValid());
        QVERIFY(external.isValid());
        HistoryStore store(temporary.path());
        QVERIFY(store.initialize().error.isEmpty());
        const QString registry = QDir(QDir(temporary.path()).filePath(QStringLiteral(".waibusnap")))
                                     .filePath(QStringLiteral("groups.json"));
        QVERIFY(QFileInfo::exists(registry));
        QVERIFY(readBytes(registry).contains("\"默认分组\""));
        QVERIFY(!readBytes(registry).contains(temporary.path().toUtf8()));
        const QString directory = external.filePath(QStringLiteral("独立分组"));
        const auto created = store.createGroup(QStringLiteral("独立分组"), directory);
        QVERIFY2(created.success, qPrintable(created.error));
        QCOMPARE(created.group.absolutePath, QDir(directory).absolutePath());
        QVERIFY(QFileInfo::exists(marker(created.group, QStringLiteral("group.json"))));
        const auto written =
            store.writeManagedPng(created.group.id, image(), HistorySource::Overlay);
        QVERIFY2(written.success, qPrintable(written.error));
        QCOMPARE(QFileInfo(written.item.absolutePath).absolutePath(),
                 QDir(directory).absolutePath());

        HistoryStore reopened(temporary.path());
        const auto scanned = reopened.initialize();
        QVERIFY2(scanned.error.isEmpty(), qPrintable(scanned.error));
        QVERIFY(std::any_of(scanned.groups.cbegin(), scanned.groups.cend(),
                            [&](const auto& snapshot)
                            {
                                return snapshot.group.id == created.group.id &&
                                       snapshot.group.absolutePath ==
                                           QDir(directory).absolutePath() &&
                                       snapshot.items.size() == 1;
                            }));
        const auto renamed = reopened.renameGroup(created.group.id, QStringLiteral("改名后"));
        QVERIFY2(renamed.success, qPrintable(renamed.error));
        QCOMPARE(renamed.group.absolutePath, QDir(directory).absolutePath());
        const auto renamedScan = HistoryStore(temporary.path()).scan();
        QVERIFY(std::any_of(renamedScan.groups.cbegin(), renamedScan.groups.cend(),
                            [&](const auto& snapshot)
                            {
                                return snapshot.group.id == created.group.id &&
                                       snapshot.group.name == QStringLiteral("改名后");
                            }));

        const QString movedDirectory = external.filePath(QStringLiteral("移动后"));
        const auto moved = reopened.changeGroupDirectory(created.group.id, movedDirectory);
        QVERIFY2(moved.success, qPrintable(moved.error));
        QVERIFY(!QFileInfo::exists(directory));
        QVERIFY(QFileInfo::exists(marker(moved.group, QStringLiteral("group.json"))));
        const auto afterMove = HistoryStore(temporary.path()).initialize();
        QVERIFY2(afterMove.error.isEmpty(), qPrintable(afterMove.error));
        QVERIFY(std::any_of(afterMove.groups.cbegin(), afterMove.groups.cend(),
                            [&](const auto& snapshot)
                            {
                                return snapshot.group.id == created.group.id &&
                                       snapshot.group.absolutePath ==
                                           QDir(movedDirectory).absolutePath() &&
                                       snapshot.items.size() == 1;
                            }));

        const QString nonEmpty = external.filePath(QStringLiteral("非空"));
        QVERIFY(QDir().mkpath(nonEmpty));
        QVERIFY(writeBytes(QDir(nonEmpty).filePath(QStringLiteral("用户文件.txt")), "保留"));
        QVERIFY(!reopened.createGroup(QStringLiteral("不可接管"), nonEmpty).success);
        QVERIFY(!reopened.changeGroupDirectory(created.group.id, nonEmpty).success);
        QCOMPARE(readBytes(QDir(nonEmpty).filePath(QStringLiteral("用户文件.txt"))),
                 QByteArray("保留"));
    }
    void duplicateUuidIsRepairedAndPersisted()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto first = store.initialize();
        QVERIFY(first.error.isEmpty());
        const auto a = store.createGroup(QStringLiteral("01-original"));
        QVERIFY(a.success);
        const QString copy = temporary.filePath(QStringLiteral("02-copy"));
        QVERIFY(copyGroup(a.group, copy));
        auto scanned = store.scan();
        QVERIFY2(scanned.error.isEmpty(), qPrintable(scanned.error));
        QCOMPARE(scanned.groups.size(), 3);
        QCOMPARE(scanned.groups[0].group.id, a.group.id);
        const QString repaired = scanned.groups[1].group.id;
        QVERIFY(repaired != a.group.id);
        QVERIFY(!QUuid(repaired).isNull());
        QCOMPARE(HistoryStore(temporary.path()).scan().groups[1].group.id, repaired);
        QVERIFY(copyGroup(first.groups[0].group,
                          temporary.filePath(QStringLiteral("03-default-copy"))));
        scanned = store.scan();
        QVERIFY(scanned.error.isEmpty());
        int defaults = 0;
        QSet<QString> ids;
        for (const auto& snapshot : scanned.groups)
        {
            defaults += snapshot.group.isDefault;
            QVERIFY(!ids.contains(snapshot.group.id));
            ids.insert(snapshot.group.id);
        }
        QCOMPARE(defaults, 1);
    }
    void managedPngPixelsDprUpdateAndStars()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto initial = store.initialize();
        QVERIFY(initial.error.isEmpty());
        const auto group = initial.groups[0].group;
        const QImage original = image();
        auto written = store.writeManagedPng(group.id, original, HistorySource::Overlay);
        QVERIFY2(written.success, qPrintable(written.error));
        const QImage loaded(written.item.absolutePath);
        QCOMPARE(loaded.devicePixelRatio(), 1.0);
        QCOMPARE(loaded.size(), original.size());
        QCOMPARE(physicalPixels(loaded), physicalPixels(original));
        QVERIFY(store.setStarred(group.id, written.item.fileName, true).success);
        auto queried = HistoryStore(temporary.path()).itemsForGroup(group.id);
        QVERIFY(queried.success);
        QCOMPARE(queried.items.size(), 1);
        QVERIFY(queried.items[0].starred && queried.items[0].managed);
        QCOMPARE(queried.usage.starredCount, 1);
        QCOMPARE(queried.usage.managedBytes, QFileInfo(written.item.absolutePath).size());
        const QImage changed = image(qRgba(200, 10, 80, 255));
        const auto updated =
            store.writeManagedPng(group.id, changed, HistorySource::Overlay, written.item.fileName);
        QVERIFY2(updated.success, qPrintable(updated.error));
        QCOMPARE(updated.item.absolutePath, written.item.absolutePath);
        QCOMPARE(updated.item.createdAtMs, written.item.createdAtMs);
        QVERIFY(updated.item.starred);
        QCOMPARE(physicalPixels(QImage(updated.item.absolutePath)), physicalPixels(changed));
        QCOMPARE(store.itemsForGroup(group.id).items.size(), 1);
        const auto second = store.writeManagedPng(group.id, original, HistorySource::Scroll);
        QVERIFY(second.success);
        QVERIFY(second.item.absolutePath != written.item.absolutePath);
        QCOMPARE(store.itemsForGroup(group.id).items.size(), 2);
        QVERIFY(store.setStarred(group.id, written.item.fileName, false).success);
        for (const auto& current : store.itemsForGroup(group.id).items)
        {
            QVERIFY(!current.starred);
            if (current.fileName == second.item.fileName)
                QCOMPARE(current.source, HistorySource::Scroll);
        }
        QVERIFY(!store.writeManagedPng(group.id, {}, HistorySource::Overlay).success);
        QVERIFY(!store.writeManagedPng(group.id, original, HistorySource::External).success);
        QVERIFY(
            !store.writeManagedPng(group.id, original, HistorySource::Scroll, written.item.fileName)
                 .success);
    }
    void managedOriginalPairUsageAndDelete()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto initial = store.initialize();
        QVERIFY(initial.error.isEmpty());
        const auto group = initial.groups[0].group;
        const QImage watermarked = image(qRgba(30, 80, 200, 255));
        const QImage original = image(qRgba(200, 30, 40, 255));
        auto written =
            store.writeManagedPng(group.id, watermarked, HistorySource::Overlay, {}, original);
        QVERIFY2(written.success, qPrintable(written.error));
        QVERIFY(written.item.hasOriginal);
        QVERIFY(written.item.originalBytes > 0);
        QVERIFY(QFileInfo::exists(written.item.originalPath));
        auto queried = store.itemsForGroup(group.id);
        QVERIFY(queried.success);
        QCOMPARE(queried.items.size(), 1);
        QVERIFY(queried.items[0].hasOriginal);
        QVERIFY(store.setStarred(group.id, written.item.fileName, true).success);
        queried = store.itemsForGroup(group.id);
        QVERIFY(queried.items[0].starred);
        QVERIFY(queried.items[0].hasOriginal);
        QCOMPARE(queried.usage.managedBytes,
                 queried.items[0].bytes + queried.items[0].originalBytes);

        const auto plain = store.writeManagedPng(group.id, watermarked, HistorySource::Overlay,
                                                 written.item.fileName);
        QVERIFY2(plain.success, qPrintable(plain.error));
        QVERIFY(!plain.item.hasOriginal);
        QVERIFY(!QFileInfo::exists(written.item.originalPath));
        QCOMPARE(store.itemsForGroup(group.id).usage.managedBytes, plain.item.bytes);

        auto paired = store.writeManagedPng(group.id, watermarked, HistorySource::Overlay,
                                            plain.item.fileName, original);
        QVERIFY2(paired.success, qPrintable(paired.error));
        QVERIFY(paired.item.hasOriginal);
        QVERIFY(QFileInfo::exists(paired.item.originalPath));
        int removed = 0;
        const auto deleted = store.deleteManagedItem(
            paired.item,
            [&](const QString& path)
            {
                const bool ok = QFile::remove(path);
                removed += ok ? 1 : 0;
                return HistoryStatus{ok, ok ? QString() : QStringLiteral("删除失败")};
            });
        QVERIFY2(deleted.success, qPrintable(deleted.error));
        QCOMPARE(removed, 2);
        QVERIFY(!QFileInfo::exists(paired.item.absolutePath));
        QVERIFY(!QFileInfo::exists(paired.item.originalPath));
    }
    void externalScanStarsUsageAndNoOverwrite()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto initial = store.initialize();
        QVERIFY(initial.error.isEmpty());
        const auto group = initial.groups[0].group;
        const QString indexPath = marker(group, QStringLiteral("items.json"));
        const QByteArray before = readBytes(indexPath);
        const QDir folder(group.absolutePath);
        const QString fakeName = QStringLiteral("WaibuSnap_20261020_120000_000_abcdef.png");
        const auto pixels = image();
        QVERIFY(pixels.save(folder.filePath(fakeName), "PNG"));
        QVERIFY(pixels.save(folder.filePath(QStringLiteral("外部.JPG")), "JPEG"));
        QVERIFY(pixels.save(folder.filePath(QStringLiteral("另一个.JPEG")), "JPEG"));
        QVERIFY(writeBytes(folder.filePath(QStringLiteral("不是图片.txt")), "保留"));
        QVERIFY(QDir(group.absolutePath).mkdir(QStringLiteral("nested")));
        QVERIFY(pixels.save(folder.filePath(QStringLiteral("nested/不递归.png")), "PNG"));
        auto queried = store.itemsForGroup(group.id);
        QVERIFY(queried.success);
        QCOMPARE(queried.items.size(), 3);
        QCOMPARE(queried.usage.managedCount, 0);
        QCOMPARE(queried.usage.externalCount, 3);
        qint64 expectedBytes = 0;
        for (const auto& external : queried.items)
        {
            QVERIFY(!external.managed);
            QCOMPARE(external.source, HistorySource::External);
            expectedBytes += QFileInfo(external.absolutePath).size();
        }
        QCOMPARE(queried.usage.externalBytes, expectedBytes);
        QCOMPARE(readBytes(indexPath), before);
        const auto fakeBytes = readBytes(folder.filePath(fakeName));
        QVERIFY(
            !store.writeManagedPng(group.id, image(), HistorySource::Overlay, fakeName).success);
        QCOMPARE(readBytes(folder.filePath(fakeName)), fakeBytes);
        const auto planned = store.cleanupCandidates(group.id, 9999999999999);
        QVERIFY(planned.error.isEmpty());
        QVERIFY(planned.candidates.isEmpty());
        QCOMPARE(planned.protectedCount, 0);
        QVERIFY(store.setStarred(group.id, fakeName, true).success);
        queried = HistoryStore(temporary.path()).itemsForGroup(group.id);
        QCOMPARE(queried.usage.starredCount, 1);
        for (const auto& external : queried.items)
            if (external.fileName == fakeName)
                QVERIFY(external.starred && !external.managed);
        QVERIFY(store.setStarred(group.id, fakeName, false).success);
        QCOMPARE(readBytes(indexPath), before);
        QVERIFY(!store.setStarred(group.id, QStringLiteral("../丢失.png"), true).success);
    }
    void corruptMetadataFailClosed_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("broken");
        QTest::newRow("items-json") << QStringLiteral("items.json") << QByteArray("{broken");
        QTest::newRow("group-json") << QStringLiteral("group.json") << QByteArray("{broken");
        QTest::newRow("items-schema")
            << QStringLiteral("items.json") << QByteArray(R"({"schemaVersion":2,"items":{}})");
        QTest::newRow("items-fields")
            << QStringLiteral("items.json") << QByteArray(R"({"schemaVersion":1,"items":[]})");
        QTest::newRow("unsafe-index-path")
            << QStringLiteral("items.json")
            << QByteArray(
                   R"({"schemaVersion":1,"items":{"../user.png":{"managed":true,"starred":false,"source":"overlay","createdAt":1}}})");
        QTest::newRow("invalid-group-id")
            << QStringLiteral("group.json")
            << QByteArray(
                   R"({"schemaVersion":1,"id":"bad","default":false,"createdAt":0,"cleanup":{}})");
    }
    void corruptMetadataFailClosed()
    {
        QFETCH(QString, file);
        QFETCH(QByteArray, broken);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto initial = store.initialize();
        QVERIFY(initial.error.isEmpty());
        const auto group = initial.groups[0].group;
        const auto written = store.writeManagedPng(group.id, image(), HistorySource::Overlay);
        QVERIFY(written.success);
        const QByteArray png = readBytes(written.item.absolutePath);
        QVERIFY(writeBytes(marker(group, file), broken));
        const auto scanned = store.scan();
        QVERIFY(!scanned.error.isEmpty());
        QCOMPARE(scanned.groups.size(), 1);
        QVERIFY(scanned.groups[0].cleanupPaused);
        QCOMPARE(scanned.groups[0].items.size(), 1);
        QVERIFY(!scanned.groups[0].items[0].managed);
        const auto plan = store.cleanupCandidates(group.id, 9999999999999);
        QVERIFY(!plan.error.isEmpty());
        QVERIFY(plan.candidates.isEmpty());
        QVERIFY(!store.setStarred(group.id, written.item.fileName, true).success);
        QVERIFY(!store.writeManagedPng(group.id, image(), HistorySource::Overlay).success);
        QVERIFY(!store.deleteGroup(group.id).success);
        QVERIFY(!store.initialize().error.isEmpty());
        QCOMPARE(readBytes(written.item.absolutePath), png);
        QCOMPARE(readBytes(marker(group, file)), broken);
    }
    void unreadableMetadataAndMissingIndex()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto initialized = store.initialize();
        QVERIFY(initialized.error.isEmpty());
        const auto group = initialized.groups[0].group;
        const auto written = store.writeManagedPng(group.id, image(), HistorySource::Overlay);
        QVERIFY(written.success);
        const QString path = marker(group, QStringLiteral("items.json"));
        QVERIFY(QFile::remove(path));
        // 无索引仍可浏览，但不能推断文件所有权。
        auto queried = store.itemsForGroup(group.id);
        QVERIFY(queried.success);
        QCOMPARE(queried.usage.externalCount, 1);
        QVERIFY(QDir().mkdir(path));
        const auto scanned = store.scan();
        QVERIFY(!scanned.error.isEmpty());
        QVERIFY(scanned.groups[0].cleanupPaused);
        QVERIFY(QFile::exists(written.item.absolutePath));
    }
    void trashFailurePreservesGroupAndFakeSuccess()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString root = temporary.filePath(QStringLiteral("history"));
        HistoryStore store(root);
        QVERIFY(store.initialize().error.isEmpty());
        const auto created = store.createGroup(QStringLiteral("课程"));
        QVERIFY(created.success);
        const auto written =
            store.writeManagedPng(created.group.id, image(), HistorySource::Overlay);
        QVERIFY(written.success);
        const QByteArray png = readBytes(written.item.absolutePath);
        const QByteArray index = readBytes(marker(created.group, QStringLiteral("items.json")));
        QVERIFY(!store.deleteGroup(created.group.id).success);
        int calls = 0;
        QString callbackPath;
        HistoryStore failure(root, {},
                             [&](const QString& path)
                             {
                                 ++calls;
                                 callbackPath = path;
                                 return HistoryStatus{false, QStringLiteral("测试回收站拒绝")};
                             });
        const auto failed = failure.deleteGroup(created.group.id);
        QVERIFY(!failed.success);
        QVERIFY(failed.error.contains(QStringLiteral("测试回收站拒绝")));
        QCOMPARE(calls, 1);
        QCOMPARE(callbackPath, created.group.absolutePath);
        QVERIFY(store.itemsForGroup(created.group.id).success);
        QCOMPARE(readBytes(written.item.absolutePath), png);
        QCOMPARE(readBytes(marker(created.group, QStringLiteral("items.json"))), index);
        const QString trash = temporary.filePath(QStringLiteral("fake-trash"));
        HistoryStore success(root, {},
                             [&](const QString& path)
                             {
                                 ++calls;
                                 const bool moved = QDir().rename(path, trash);
                                 return HistoryStatus{
                                     moved, moved ? QString() : QStringLiteral("替身移动失败")};
                             });
        QVERIFY(success.deleteGroup(created.group.id).success);
        QCOMPARE(calls, 2);
        QVERIFY(!QFile::exists(created.group.absolutePath));
        QVERIFY(QFile::exists(QDir(trash).filePath(written.item.fileName)));
        QCOMPARE(store.scan().groups.size(), 1);
    }
    void lockAndInvalidRootReturnErrors()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.path());
        const auto initialized = store.initialize();
        QVERIFY(initialized.error.isEmpty());
        QLockFile lock(temporary.filePath(QStringLiteral(".waibusnap.lock")));
        QVERIFY(lock.tryLock(0));
        QVERIFY(!store.scan().error.isEmpty());
        QVERIFY(!store.createGroup(QStringLiteral("不可创建")).success);
        QVERIFY(
            !store.writeManagedPng(initialized.groups[0].group.id, image(), HistorySource::Overlay)
                 .success);
        QVERIFY(!HistoryStore(QStringLiteral("relative/path")).initialize().error.isEmpty());
        const QString fileRoot = temporary.filePath(QStringLiteral("file"));
        QVERIFY(writeBytes(fileRoot, "原样保留"));
        QVERIFY(!HistoryStore(fileRoot).initialize().error.isEmpty());
        QCOMPARE(readBytes(fileRoot), QByteArray("原样保留"));
    }
    void settingsDefaultsRoundTripAndOldKeysUnchanged()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("设置/settings.ini"));
        AppSettings settings(path);
        const auto defaults = settings.loadHistoryPreferences();
        QCOMPARE(defaults.rootDirectory, HistoryStore::defaultRootDirectory());
        QVERIFY(defaults.currentGroupId.isEmpty());
        QVERIFY(!defaults.enabled);
        QCOMPARE(defaults.defaultCleanup.maxCount, 100);
        QVERIFY(!QFile::exists(path));
        QVERIFY(settings.saveHotkey(QKeySequence(Qt::Key_F3)).isEmpty());
        QVERIFY(settings.saveSavePreferences({temporary.path(), ImageFormat::Jpeg}).isEmpty());
        QVERIFY(settings.saveStickerCloseReminder(false).isEmpty());
        QMap<QString, QVariant> oldKeys;
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("future/custom"), QStringLiteral("保留未知键"));
            ini.sync();
            for (const auto& key : ini.allKeys())
                oldKeys.insert(key, ini.value(key));
        }
        HistoryPreferences preferences{temporary.filePath(QStringLiteral("图片 库")),
                                       QUuid::createUuid().toString(QUuid::WithoutBraces),
                                       {}};
        preferences.enabled = true;
        preferences.defaultCleanup.maxAgeDays = 90;
        preferences.defaultCleanup.countEnabled = false;
        preferences.defaultCleanup.maxBytes = 200 * 1024 * 1024;
        QVERIFY(settings.saveHistoryPreferences(preferences).isEmpty());
        QString notice;
        const auto loaded = AppSettings(path).loadHistoryPreferences(&notice);
        QVERIFY(notice.isEmpty());
        QCOMPARE(loaded.rootDirectory, preferences.rootDirectory);
        QCOMPARE(loaded.currentGroupId, preferences.currentGroupId);
        QCOMPARE(cleanupPolicyToJson(loaded.defaultCleanup),
                 cleanupPolicyToJson(preferences.defaultCleanup));
        QCOMPARE(loaded.enabled, true);
        {
            QSettings ini(path, QSettings::IniFormat);
            QCOMPARE(ini.allKeys().size(), oldKeys.size() + 4);
            for (auto it = oldKeys.begin(); it != oldKeys.end(); ++it)
                QCOMPARE(ini.value(it.key()), it.value());
        }
        preferences.rootDirectory.clear();
        preferences.currentGroupId.clear();
        preferences.enabled = false;
        preferences.defaultCleanup.enabled = false;
        preferences.defaultCleanup.ageEnabled = preferences.defaultCleanup.countEnabled =
            preferences.defaultCleanup.bytesEnabled = false;
        QVERIFY(settings.saveHistoryPreferences(preferences).isEmpty());
        QCOMPARE(settings.loadHistoryPreferences().rootDirectory,
                 HistoryStore::defaultRootDirectory());
        QVERIFY(!settings.loadHistoryPreferences().defaultCleanup.enabled);
    }
    void settingsCorruptionAndWriteFailure()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        const QByteArray corrupted =
            "[history]\nrootDirectory=relative\ncurrentGroupId=bad\ndefaultCleanup=garbage\n";
        QVERIFY(writeBytes(path, corrupted));
        AppSettings settings(path);
        QString notice;
        const auto loaded = settings.loadHistoryPreferences(&notice);
        QVERIFY(!notice.isEmpty());
        QCOMPARE(loaded.rootDirectory, HistoryStore::defaultRootDirectory());
        QVERIFY(loaded.currentGroupId.isEmpty());
        QCOMPARE(loaded.defaultCleanup.maxAgeDays, 30);
        QCOMPARE(readBytes(path), corrupted);
        HistoryPreferences preferences{temporary.path(), {}, {}};
        preferences.rootDirectory = QStringLiteral("relative");
        QVERIFY(!settings.saveHistoryPreferences(preferences).isEmpty());
        preferences.rootDirectory = temporary.path();
        preferences.currentGroupId = QStringLiteral("bad");
        QVERIFY(!settings.saveHistoryPreferences(preferences).isEmpty());
        preferences.currentGroupId.clear();
        preferences.defaultCleanup.ageEnabled = preferences.defaultCleanup.countEnabled =
            preferences.defaultCleanup.bytesEnabled = false;
        QVERIFY(!settings.saveHistoryPreferences(preferences).isEmpty());
        QCOMPARE(readBytes(path), corrupted);
        preferences.defaultCleanup = {};
        const QString blocked = temporary.filePath(QStringLiteral("作为文件的目录"));
        QVERIFY(writeBytes(blocked, "用户文件"));
        AppSettings failure(QDir(blocked).filePath(QStringLiteral("settings.ini")));
        QVERIFY(!failure.saveHistoryPreferences(preferences).isEmpty());
        QCOMPARE(failure.loadHistoryPreferences().rootDirectory,
                 HistoryStore::defaultRootDirectory());
        QVERIFY(QFile::remove(blocked));
        QVERIFY(failure.saveHistoryPreferences(preferences).isEmpty());
        QCOMPARE(failure.loadHistoryPreferences().rootDirectory, preferences.rootDirectory);
        const QString directoryTarget = temporary.filePath(QStringLiteral("目录目标.ini"));
        QVERIFY(QDir().mkdir(directoryTarget));
        QVERIFY(!AppSettings(directoryTarget).saveHistoryPreferences(preferences).isEmpty());
        QVERIFY(writeBytes(path, "[broken\n"));
        QVERIFY(!AppSettings(path).loadHistoryPreferences(&notice).rootDirectory.isEmpty());
        QVERIFY(!notice.isEmpty());
    }
};
QTEST_APPLESS_MAIN(HistoryStoreTest)
#include "history_store_test.moc"
