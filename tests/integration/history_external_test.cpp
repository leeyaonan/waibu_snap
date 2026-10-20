#include "app/history_manager.h"
#include "ui/history_window.h"
#include <QBuffer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
using namespace waibusnap;
namespace
{
QByteArray bytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeBytes(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QImage picture()
{
    QImage image(73, 41, QImage::Format_ARGB32);
    image.fill(QColor(30, 80, 190, 120));
    image.setPixelColor(0, 0, Qt::red);
    image.setDevicePixelRatio(2);
    return image;
}
template <class T> T* child(QObject* root, const char* name)
{
    auto* value = root->findChild<T*>(QLatin1String(name));
    if (!value)
        qFatal("缺少外部图片测试控件：%s", name);
    return value;
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings;
    HistoryManager manager;
    HistoryStore store;
    QPointer<HistoryWindow> window;
    HistoryWindowActions actions;
    QImage copied, pinned;
    QString revealed;
    int copies = 0, pins = 0, trashCalls = 0, revealCalls = 0;
    bool revealFails = false;
    Fixture()
        : settings(temp.filePath(QStringLiteral("settings.ini"))),
          manager(settings, temp.filePath(QStringLiteral("历史 根目录"))),
          store(temp.filePath(QStringLiteral("历史 根目录")))
    {
        if (!temp.isValid() || !manager.initialize().success)
            qFatal("临时历史库初始化失败，拒绝访问真实目录。");
        actions.copyImage = [this](const QImage& image)
        {
            ++copies;
            copied = image;
            return ImageOutputResult{true, {}};
        };
        actions.pinImage = [this](const QImage& image)
        {
            ++pins;
            pinned = image;
            return ImageOutputResult{true, {}};
        };
        actions.trash = [this](const QString&)
        {
            ++trashCalls;
            return HistoryStatus{false, QStringLiteral("禁止触碰真实回收站")};
        };
        actions.reveal = [this](const QString& path)
        {
            ++revealCalls;
            revealed = path;
            return HistoryStatus{!revealFails,
                                 revealFails ? QStringLiteral("定位失败替身") : QString()};
        };
    }
    ~Fixture() { delete window.data(); }
    QString path(const QString& name) const
    {
        return QDir(manager.currentGroup()->absolutePath).filePath(name);
    }
    QString index() const { return path(QStringLiteral(".waibusnap/items.json")); }
    HistoryWindow* open()
    {
        window = new HistoryWindow(manager, settings, actions);
        window->show();
        QCoreApplication::processEvents();
        return window;
    }
    QListWidget* list() const { return child<QListWidget>(window, "historyThumbnails"); }
    QString selected() const { return list()->currentItem()->data(Qt::UserRole).toString(); }
    void click(const char* name) { child<QToolButton>(window, name)->click(); }
    void view(const QString& id)
    {
        auto* combo = child<QComboBox>(window, "historyGroupCombo");
        combo->setCurrentIndex(combo->findData(id));
    }
    QString notice() const { return child<QLabel>(window, "historyNotice")->text(); }
};
void formats()
{
    QTest::addColumn<QString>("suffix");
    QTest::addColumn<QByteArray>("format");
    QTest::newRow("png") << QStringLiteral("PNG") << QByteArray("png");
    QTest::newRow("jpg") << QStringLiteral("JPG") << QByteArray("jpeg");
    QTest::newRow("jpeg") << QStringLiteral("JPEG") << QByteArray("jpeg");
    QTest::newRow("webp") << QStringLiteral("WebP") << QByteArray("webp");
}
}
class HistoryExternalTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY2(QImageReader::supportedImageFormats().contains("webp"),
                 "环境阻塞：缺少 Qt qtimageformats WebP 解码插件，请按 scripts/README.md 补装。");
        QVERIFY2(QImageWriter::supportedImageFormats().contains("webp"),
                 "环境阻塞：Qt WebP 编码插件不可用，不能跳过 WebP 验证。");
    }
    void webpFixtureAndCodecRoundTrip()
    {
        QImageReader fixture(QString::fromUtf8(WAIBUSNAP_WEBP_FIXTURE));
        QVERIFY(fixture.canRead());
        QCOMPARE(fixture.format(), QByteArray("webp"));
        const auto decoded = fixture.read();
        QCOMPARE(decoded.size(), QSize(7, 5));
        QCOMPARE(decoded.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(decoded.pixelColor(1, 1), QColor(30, 80, 190, 120));
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("原像素@2x.webp"));
        QImageWriter writer(path, "webp");
        writer.setQuality(100);
        QVERIFY2(writer.write(picture()), qPrintable(writer.errorString()));
        QImageReader reader(path);
        QVERIFY(reader.canRead());
        const auto image = reader.read();
        QCOMPARE(image.size(), picture().size());
        QCOMPARE(image.convertToFormat(QImage::Format_RGBA8888),
                 picture().convertToFormat(QImage::Format_RGBA8888));
    }
    void scanFormatsAndBoundaries()
    {
        Fixture f;
        const auto initialIndex = bytes(f.index());
        for (const auto& suffix : {"png", "JPG", "JPEG", "WEBP"})
            QVERIFY(
                picture().save(f.path(QStringLiteral("外部 图片.%1").arg(QLatin1String(suffix)))));
        QVERIFY(picture().save(f.path(QStringLiteral(".隐藏.png"))));
        QVERIFY(QDir().mkdir(f.path(QStringLiteral("嵌套"))));
        QVERIFY(picture().save(f.path(QStringLiteral("嵌套/不扫描.webp"))));
        QVERIFY(writeBytes(f.path(QStringLiteral("保留.txt")), "用户文件"));
        const auto plain = f.temp.filePath(QStringLiteral("历史 根目录/普通目录"));
        QVERIFY(QDir().mkdir(plain));
        QVERIFY(picture().save(QDir(plain).filePath(QStringLiteral("不扫描.png"))));
#ifdef Q_OS_MACOS
        // Unix 真符号链接；Windows QFile::link 生成快捷方式，不作为同一证据。
        QVERIFY(QFile::link(f.path(QStringLiteral("外部 图片.png")),
                            f.path(QStringLiteral("链接.webp"))));
        QVERIFY(QFile::link(f.manager.currentGroup()->absolutePath,
                            QDir(f.manager.rootDirectory()).filePath(QStringLiteral("分组链接"))));
#endif
        const auto scan = f.store.scan();
        QVERIFY2(scan.error.isEmpty(), qPrintable(scan.error));
        QCOMPARE(scan.groups.size(), 1);
        QCOMPARE(scan.groups[0].items.size(), 4);
        QCOMPARE(scan.groups[0].usage.externalCount, 4);
        qint64 externalBytes = 0;
        for (const auto& item : scan.groups[0].items)
        {
            QVERIFY(!item.managed);
            QCOMPARE(item.source, HistorySource::External);
            externalBytes += QFileInfo(item.absolutePath).size();
        }
        QCOMPARE(scan.groups[0].usage.externalBytes, externalBytes);
        QCOMPARE(scan.groups[0].usage.managedBytes, 0);
        QCOMPARE(bytes(f.index()), initialIndex);
        const auto scroll =
            f.store.writeManagedPng(f.manager.currentGroupId(), picture(), HistorySource::Scroll);
        QVERIFY(scroll.success);
        f.open();
        int externalLabels = 0, scrollLabels = 0;
        for (int row = 0; row < f.list()->count(); ++row)
        {
            externalLabels += f.list()->item(row)->text().contains(QStringLiteral("外部图片"));
            scrollLabels += f.list()->item(row)->text().contains(QStringLiteral("长图"));
        }
        QCOMPARE(externalLabels, 4);
        QCOMPARE(scrollLabels, 1);
    }
    void externalActions_data() { formats(); }
    void externalActions()
    {
        QFETCH(QString, suffix);
        QFETCH(QByteArray, format);
        Fixture f;
        const auto path = f.path(QStringLiteral("外部 中文@2x.%1").arg(suffix));
        QImageWriter writer(path, format);
        writer.setQuality(100);
        QVERIFY2(writer.write(picture()), qPrintable(writer.errorString()));
        const auto original = bytes(path);
        const auto initialIndex = bytes(f.index());
        auto* w = f.open();
        const auto id = f.selected();
        QCOMPARE(f.list()->count(), 1);
        QVERIFY(f.list()->currentItem()->text().contains(QStringLiteral("外部图片")));
        QVERIFY(!f.manager.findItem(id)->managed);
        QVERIFY(!child<QToolButton>(w, "historyDelete")->isEnabled());
        QCOMPARE(child<QToolButton>(w, "historyDelete")->toolTip(),
                 QStringLiteral("请在文件管理器中操作"));
        f.click("historyDelete");
        QVERIFY(!f.manager.deleteItem(id, f.actions.trash).success);
        QCOMPARE(f.trashCalls, 0);
        QImageReader reader(path);
        reader.setAutoTransform(true);
        auto expected = reader.read();
        expected.setDevicePixelRatio(1);
        const auto loaded = f.manager.loadImage(id);
        QVERIFY2(loaded.success, qPrintable(loaded.error));
        QCOMPARE(loaded.image, expected);
        QCOMPARE(loaded.image.size(), picture().size());
        QCOMPARE(loaded.image.devicePixelRatio(), qreal(1));
        QCOMPARE(child<HistoryImageView>(w, "historyImageView")->image(), expected);
        QTRY_VERIFY(!f.list()->currentItem()->icon().isNull());
        f.click("historyCopy");
        QTest::keyClick(f.list(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(f.copies, 1);
        QCOMPARE(f.pins, 1);
        QCOMPARE(f.copied, expected);
        QCOMPARE(f.pinned, expected);
        QCOMPARE(f.copied.devicePixelRatio(), qreal(1));
        QCOMPARE(f.pinned.devicePixelRatio(), qreal(1));
        QVERIFY(w->isVisible());
        QCOMPARE(bytes(f.index()), initialIndex);
        f.click("historyReveal");
        QCOMPARE(f.revealed, path);
        f.revealFails = true;
        f.click("historyReveal");
        QVERIFY(f.notice().contains(QStringLiteral("定位失败")));
        QCOMPARE(f.revealCalls, 2);
        QCOMPARE(f.selected(), id);
        f.click("historyStar");
        QVERIFY(f.manager.findItem(id)->starred);
        HistoryManager restarted(f.settings, f.manager.rootDirectory());
        QVERIFY(restarted.initialize().success);
        QVERIFY(restarted.findItem(id)->starred);
        QVERIFY(!restarted.findItem(id)->managed);
        f.view(QStringLiteral("starred"));
        QCOMPARE(f.list()->count(), 1);
        QCOMPARE(f.list()->currentItem()->checkState(), Qt::Checked);
        f.list()->currentItem()->setCheckState(Qt::Unchecked);
        QCOMPARE(f.list()->count(), 0);
        const auto index = QJsonDocument::fromJson(bytes(f.index())).object();
        QVERIFY(
            !index.value(QStringLiteral("items")).toObject().contains(QFileInfo(path).fileName()));
        QCOMPARE(bytes(f.index()), initialIndex);
        f.view(QStringLiteral("all"));
        QCOMPARE(f.list()->count(), 1);
        QTest::keyClick(w, Qt::Key_Return);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(f.window.isNull());
        QCOMPARE(f.pins, 2);
        QCOMPARE(f.store.scan().groups[0].items.size(), 1);
        QCOMPARE(bytes(path), original);
    }
    void startupOpenAndManualRefreshKeepSessionAndErrors()
    {
        Fixture f;
        const auto session = f.manager.beginSession();
        const auto recorded = f.manager.record(session, picture(), HistorySource::Overlay);
        QVERIFY(recorded.success);
        QVERIFY(picture().save(f.path(QStringLiteral("启动前.webp"))));
        HistoryManager restarted(f.settings, f.manager.rootDirectory());
        QVERIFY(restarted.initialize().success);
        QCOMPARE(restarted.snapshot().groups[0].items.size(), 2);
        QVERIFY(!f.manager.record({}, picture(), HistorySource::Overlay).success);
        const auto error = f.manager.error();
        const auto current = f.manager.currentGroupId();
        const auto persisted = f.settings.loadHistoryPreferences().currentGroupId;
        QSignalSpy errors(&f.manager, &HistoryManager::errorChanged);
        f.open();
        QCOMPARE(f.list()->count(), 2);
        QVERIFY(picture().save(f.path(QStringLiteral("手动刷新.webp"))));
        QCoreApplication::processEvents();
        QCOMPARE(f.list()->count(), 2);
        child<QPushButton>(f.window, "historyRefresh")->click();
        QCOMPARE(f.list()->count(), 3);
        QCOMPARE(f.manager.currentGroupId(), current);
        QCOMPARE(f.settings.loadHistoryPreferences().currentGroupId, persisted);
        QCOMPARE(f.manager.error(), error);
        QCOMPARE(errors.count(), 0);
        const auto repeated = f.manager.record(session, picture(), HistorySource::Overlay);
        QVERIFY(repeated.success);
        QCOMPARE(repeated.item.fileName, recorded.item.fileName);
        QCOMPARE(f.store.scan().groups[0].items.size(), 3);
    }
    void damagedAndMissingImages_data() { formats(); }
    void damagedAndMissingImages()
    {
        QFETCH(QString, suffix);
        QFETCH(QByteArray, format);
        Fixture f;
        const auto path = f.path(QStringLiteral("损坏.%1").arg(suffix));
        QVERIFY(writeBytes(path, "不是图片"));
        f.open();
        const auto id = f.selected();
        auto result = f.manager.loadImage(id);
        QVERIFY(!result.success);
        QVERIFY(result.error.contains(QStringLiteral("损坏")));
        QVERIFY(child<QLabel>(f.window, "historyEmpty")->text().contains(QStringLiteral("损坏")));
        f.click("historyCopy");
        QTest::keyClick(f.list(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(f.copies, 0);
        QCOMPARE(f.pins, 0);
        QVERIFY(f.notice().contains(QStringLiteral("损坏")));
        QVERIFY(picture().save(path, format.constData()));
        f.window->refresh();
        QVERIFY(f.manager.loadImage(id).success);
        QVERIFY(QFile::remove(path));
        result = f.manager.loadImage(id);
        QVERIFY(!result.success);
        QVERIFY(result.error.contains(QStringLiteral("丢失")));
        f.click("historyCopy");
        f.click("historyPin");
        f.click("historyReveal");
        QCOMPARE(f.copies, 0);
        QCOMPARE(f.pins, 0);
        QCOMPARE(f.revealCalls, 0);
        QVERIFY(f.window->isVisible());
        QVERIFY(picture().save(f.path(QStringLiteral("仍能查看.png"))));
        f.window->refresh();
        QCOMPARE(f.list()->count(), 1);
        QVERIFY(f.manager.loadImage(f.selected()).success);
    }
    void jpegAutoTransform()
    {
        Fixture f;
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(picture().save(&buffer, "jpeg"));
        // 最小 EXIF：小端 TIFF，Orientation=6，顺时针旋转 90 度。
        jpeg.insert(
            2, QByteArray::fromHex(
                   "ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        const auto path = f.path(QStringLiteral("旋转.jpg"));
        QVERIFY(writeBytes(path, jpeg));
        f.manager.refresh();
        const auto item = f.manager.snapshot().groups[0].items[0];
        const auto loaded = f.manager.loadImage(historyItemId(item));
        QVERIFY2(loaded.success, qPrintable(loaded.error));
        QCOMPARE(loaded.image.size(), QSize(41, 73));
        QCOMPARE(loaded.image.devicePixelRatio(), qreal(1));
    }
    void cleanupExcludesAllExternalFormatsAndSeparatesUsage()
    {
        Fixture f;
        for (int i = 0; i < 3; ++i)
            QVERIFY(f.store
                        .writeManagedPng(f.manager.currentGroupId(), picture(),
                                         i == 2 ? HistorySource::Scroll : HistorySource::Overlay)
                        .success);
        for (const auto* suffix : {"png", "jpg", "jpeg", "webp"})
            QVERIFY(picture().save(f.path(QStringLiteral("外部.%1").arg(QLatin1String(suffix)))));
        CleanupPolicy policy;
        policy.ageEnabled = false;
        policy.maxCount = 1;
        policy.bytesEnabled = false;
        QVERIFY(f.store.saveCleanupPolicy(f.manager.currentGroupId(), policy).success);
        const auto scan = f.store.scan();
        const auto& group = scan.groups[0];
        QCOMPARE(group.usage.managedCount, 3);
        QCOMPARE(group.usage.externalCount, 4);
        qint64 managedBytes = 0, externalBytes = 0;
        QString selected;
        for (const auto& item : group.items)
        {
            if (item.managed)
                managedBytes += QFileInfo(item.absolutePath).size();
            else
            {
                externalBytes += QFileInfo(item.absolutePath).size();
                selected = historyItemId(item);
            }
        }
        QCOMPARE(group.usage.managedBytes, managedBytes);
        QCOMPARE(group.usage.externalBytes, externalBytes);
        const auto plan = f.store.cleanupCandidates(group.group.id, 9999999999999, selected);
        QVERIFY(plan.error.isEmpty());
        QCOMPARE(plan.candidates.size(), 2);
        QCOMPARE(plan.protectedCount, 1);
        for (const auto& item : plan.candidates)
        {
            QVERIFY(item.managed);
            QVERIFY(item.source != HistorySource::External);
        }
        // 仅选候选；本批不执行删除，所有源文件和占用保持不变。
        QCOMPARE(f.store.scan().groups[0].items.size(), 7);
        QCOMPARE(f.store.scan().groups[0].usage.externalBytes, externalBytes);
    }
};
QTEST_MAIN(HistoryExternalTest)
#include "history_external_test.moc"
