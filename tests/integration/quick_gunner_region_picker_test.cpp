#include "app/quick_gunner.h"
#include "ui/quick_gunner_region_picker.h"
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
using namespace waibusnap;
namespace
{
CaptureFrame frame()
{
    QImage pixels(800, 600, QImage::Format_RGB32);
    pixels.fill(Qt::green);
    return {{42, {-400, 20, 400, 300}, 2, 123, {-400, 40, 400, 280}}, pixels, 0};
}
class Displays final : public DisplayTopology
{
  public:
    DisplayResult result{frame().display, {}};
    bool matches = true;
    DisplayResult cursorDisplay() const override { return result; }
    bool stillMatches(const DisplayTarget& target) const override
    {
        return matches && target.id == result.target.id &&
               target.logicalGeometry == result.target.logicalGeometry &&
               target.devicePixelRatio == result.target.devicePixelRatio &&
               target.layoutVersion == result.target.layoutVersion;
    }
};
class Capture final : public CaptureProvider
{
  public:
    bool permission = true;
    int calls = 0;
    QVector<std::function<void(CaptureResult)>> callbacks;
    bool hasPermission() const override { return permission; }
    bool requestPermission() override { return false; }
    void capture(const DisplayTarget&, std::function<void(CaptureResult)> callback) override
    {
        ++calls;
        callbacks.append(std::move(callback));
    }
    void complete(int index = 0, CaptureResult result = {frame(), CaptureError::None, {}})
    {
        callbacks.at(index)(std::move(result));
    }
};
class Picker final : public QuickGunnerRegionPicker
{
  public:
    using QuickGunnerRegionPicker::QuickGunnerRegionPicker;
    void select(QRect rect) { emit regionSelected(rect); }
    void cancel() { emit cancelled(); }
};
struct Harness
{
    QTemporaryDir temp;
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    Displays displays;
    Capture capture;
    QPointer<Picker> picker;
    QuickGunnerController controller{settings, displays, capture,
                                     [this](CaptureFrame frozen, QWidget* parent)
                                     {
                                         picker = new Picker(std::move(frozen), parent);
                                         return picker.data();
                                     }};
};
QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
void drag(QWidget* picker, QPointF from, QPointF to)
{
    QMouseEvent press(QEvent::MouseButtonPress, from, picker->mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(picker, &press);
    QMouseEvent move(QEvent::MouseMove, to, picker->mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(picker, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, picker->mapToGlobal(to), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(picker, &release);
}
}
class QuickGunnerRegionPickerTest final : public QObject
{
    Q_OBJECT
  private slots:
    void settingsRoundTripAndReadOnly()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        QString notice = QStringLiteral("旧提示");
        QVERIFY(!settings.loadQuickGunnerPreferences(&notice).region);
        QVERIFY(notice.isEmpty());
        QVERIFY(!QFile::exists(path));
        const QuickGunnerPreferences expected{
            temp.path(), QuickGunnerRegion{frame().display, {20, 40, 100, 80}}};
        QVERIFY(settings.saveQuickGunnerPreferences(expected).isEmpty());
        const auto before = read(path);
        const auto result = AppSettings(path).loadQuickGunnerPreferences(&notice);
        QCOMPARE(result.directory, expected.directory);
        QCOMPARE(result.region->toJson(), expected.region->toJson());
        QVERIFY(notice.isEmpty());
        QCOMPARE(read(path), before);
        QSettings ini(path, QSettings::IniFormat);
        QCOMPARE(ini.value("quickGunner/directory").toString(), temp.path());
        QVERIFY(ini.value("quickGunner/region").toString().startsWith('{'));
        QVERIFY(settings.saveQuickGunnerPreferences({}).isEmpty());
        QVERIFY(!AppSettings(path).loadQuickGunnerPreferences().region);
    }
    void corruptSettings_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("json") << QByteArray("[quickGunner]\ndirectory=/tmp\nregion=broken\n");
        QTest::newRow("directory-type") << QByteArray("[quickGunner]\ndirectory=@Bool(true)\n");
        QTest::newRow("relative") << QByteArray("[quickGunner]\ndirectory=relative\n");
        QTest::newRow("ini") << QByteArray("[broken\n");
    }
    void corruptSettings()
    {
        QFETCH(QByteArray, content);
        QTemporaryDir temp;
        const auto path = temp.filePath(QStringLiteral("settings.ini"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
        file.close();
        QString notice;
        const auto result = AppSettings(path).loadQuickGunnerPreferences(&notice);
        QVERIFY(!notice.isEmpty());
        QVERIFY(result.directory.isEmpty());
        QVERIFY(!result.region);
        QCOMPARE(read(path), content);
    }
    void directoryValidation()
    {
        Harness h;
        const auto root = h.temp.path();
        const auto directory = h.temp.filePath(QStringLiteral("拍摄 目录"));
        const auto reserved = h.temp.filePath(QStringLiteral("历史"));
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(QDir().mkpath(reserved + QStringLiteral("/分组")));
        QVERIFY(h.controller.setDirectory(directory + QStringLiteral("/../拍摄 目录/"), {reserved})
                    .isEmpty());
        QCOMPARE(h.controller.preferences().directory, QDir::cleanPath(directory));
        QVERIFY(QDir(directory).entryList(QDir::Files | QDir::Hidden).isEmpty());
        const auto filePath = h.temp.filePath(QStringLiteral("file"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        for (const auto& invalid :
             {QStringLiteral("relative"), filePath, h.temp.filePath(QStringLiteral("missing")),
              reserved, reserved + QStringLiteral("/分组"), root})
        {
            QVERIFY2(!h.controller.setDirectory(invalid, {reserved}).isEmpty(),
                     qPrintable(invalid));
            QCOMPARE(h.controller.preferences().directory, directory);
            QCOMPARE(h.settings.loadQuickGunnerPreferences().directory, directory);
        }
        // 相邻前缀不是父子目录。
        QVERIFY(QDir().mkdir(directory + QStringLiteral("后缀")));
        QVERIFY(
            h.controller.setDirectory(directory, {directory + QStringLiteral("后缀")}).isEmpty());
    }
    void directorySymlink()
    {
        Harness h;
        const auto link = h.temp.filePath(QStringLiteral("link"));
        if (!QFile::link(h.temp.path(), link) || !QFileInfo(link).isSymLink())
            QSKIP("当前平台不能创建目录符号链接。");
        QVERIFY(!h.controller.setDirectory(link, {}).isEmpty());
        const auto child = h.temp.filePath(QStringLiteral("child"));
        QVERIFY(QDir().mkdir(child));
        QVERIFY(!h.controller.setDirectory(child, {link}).isEmpty());
    }
    void missingReservedDirectory()
    {
        Harness h;
        const auto chosen = h.temp.filePath(QStringLiteral("chosen"));
        const auto missing = h.temp.filePath(QStringLiteral("missing/history"));
        QVERIFY(QDir().mkdir(chosen));
        QVERIFY(h.controller.setDirectory(chosen, {missing}).isEmpty());
        QVERIFY(!h.controller.setDirectory(h.temp.path(), {missing}).isEmpty());
        QVERIFY(!QFile::exists(missing));
    }
    void settingsKeepUnrelatedKeysAndRejectInvalidWrites()
    {
        Harness h;
        const auto path = h.temp.filePath(QStringLiteral("settings.ini"));
        QSettings existing(path, QSettings::IniFormat);
        existing.setValue(QStringLiteral("other/value"), 42);
        existing.sync();
        QuickGunnerPreferences initial{h.temp.path(),
                                       QuickGunnerRegion{frame().display, {20, 20, 50, 50}}};
        QVERIFY(h.settings.saveQuickGunnerPreferences(initial).isEmpty());
        const auto before = read(path);
        auto invalid = initial;
        invalid.region->pixelRect.setWidth(15);
        QVERIFY(!h.settings.saveQuickGunnerPreferences(invalid).isEmpty());
        QCOMPARE(read(path), before);
        invalid = initial;
        invalid.directory = QStringLiteral("relative");
        QVERIFY(!h.settings.saveQuickGunnerPreferences(invalid).isEmpty());
        QCOMPARE(read(path), before);
        auto next = initial;
        next.region->pixelRect.translate(10, 0);
        QVERIFY(h.settings.saveQuickGunnerPreferences(next).isEmpty());
        QCOMPARE(h.settings.loadQuickGunnerPreferences().region->pixelRect, next.region->pixelRect);
        existing.sync();
        QCOMPARE(existing.value(QStringLiteral("other/value")).toInt(), 42);
        existing.setValue(QStringLiteral("other/value"), 43);
        existing.sync();
        QCOMPARE(h.settings.loadQuickGunnerPreferences().region->pixelRect, next.region->pixelRect);
    }
    void directoryNotWritable()
    {
        Harness h;
        const auto path = h.temp.filePath(QStringLiteral("readonly"));
        QVERIFY(QDir().mkdir(path));
        const auto old = QFileInfo(path).permissions();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QTemporaryFile permissionProbe(QDir(path).filePath(QStringLiteral("probe-XXXXXX")));
        if (permissionProbe.open())
        {
            permissionProbe.remove();
            QVERIFY(QFile::setPermissions(path, old));
            QSKIP("当前文件系统或运行身份无法通过 Qt 权限位制造不可写目录。");
        }
        const auto error = h.controller.setDirectory(path, {});
        QVERIFY(QFile::setPermissions(path, old));
        QVERIFY(!error.isEmpty());
        QVERIFY(h.controller.preferences().directory.isEmpty());
    }
    void settingsSameSizeAndTimestampUpdates()
    {
        Harness h;
        const auto path = h.temp.filePath(QStringLiteral("settings.ini"));
        QuickGunnerPreferences previous{h.temp.path(),
                                        QuickGunnerRegion{frame().display, {20, 20, 50, 50}}};
        QVERIFY(h.settings.saveQuickGunnerPreferences(previous).isEmpty());
        QCOMPARE(h.settings.loadQuickGunnerPreferences().region->pixelRect,
                 previous.region->pixelRect);
        const auto timestamp = QFileInfo(path).lastModified();
        auto next = previous;
        next.region->pixelRect.translate(10, 0);
        QVERIFY(h.settings.saveQuickGunnerPreferences(next).isEmpty());
        // 模拟 Windows 快速连续保存落在相同文件时间戳内，尺寸也保持一致。
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(timestamp, QFileDevice::FileModificationTime));
        file.close();
        QCOMPARE(h.settings.loadQuickGunnerPreferences().region->pixelRect, next.region->pixelRect);
    }
    void settingsAtomicSyncFailureKeepsOldValues()
    {
        QTemporaryDir temp;
        const auto folder = temp.filePath(QStringLiteral("settings"));
        QVERIFY(QDir().mkdir(folder));
        const auto path = QDir(folder).filePath(QStringLiteral("settings.ini"));
        AppSettings settings(path);
        const QuickGunnerPreferences old{temp.path(),
                                         QuickGunnerRegion{frame().display, {20, 20, 50, 50}}};
        QVERIFY(settings.saveQuickGunnerPreferences(old).isEmpty());
        const auto before = read(path);
        const auto permissions = QFileInfo(folder).permissions();
        if (!QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner))
            QSKIP("当前文件系统不支持通过 Qt 权限位制造原子同步失败。");
        QTemporaryFile probe(QDir(folder).filePath(QStringLiteral("probe-XXXXXX")));
        if (probe.open())
        {
            probe.remove();
            QVERIFY(QFile::setPermissions(folder, permissions));
            QSKIP("当前文件系统或运行身份仍允许写入目录，不能制造原子同步失败。");
        }
        auto next = old;
        next.region->pixelRect.translate(10, 0);
        const auto error = settings.saveQuickGunnerPreferences(next);
        QVERIFY(QFile::setPermissions(folder, permissions));
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(path), before);
        // 无关设置的后续同步也不能把失败的机位夹带落盘。
        QVERIFY(settings.saveStickerCloseReminder(false).isEmpty());
        QCOMPARE(settings.loadQuickGunnerPreferences().region->pixelRect, old.region->pixelRect);
    }
    void savingFailureKeepsOldValues()
    {
        Harness h;
        QVERIFY(h.controller.setDirectory(h.temp.path(), {}).isEmpty());
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.capture.complete();
        const QRect previous(80, 60, 50, 40);
        h.picker->select(previous);
        const auto path = h.temp.filePath(QStringLiteral("settings.ini"));
        const auto bytes = read(path);
        QVERIFY(QFile::rename(path, path + QStringLiteral(".old")));
        QVERIFY(QDir().mkdir(path));
        const auto other = h.temp.filePath(QStringLiteral("other"));
        QVERIFY(QDir().mkdir(other));
        QVERIFY(!h.controller.setDirectory(other, {}).isEmpty());
        QCOMPARE(h.controller.preferences().directory, h.temp.path());
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.capture.complete(1);
        QSignalSpy notice(&h.controller, &QuickGunnerController::noticeChanged);
        h.picker->select({20, 20, 40, 40});
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        QCOMPARE(h.controller.preferences().region->pixelRect, previous);
        QCOMPARE(notice.size(), 1);
        QVERIFY(QDir().rmdir(path));
        QVERIFY(QFile::rename(path + QStringLiteral(".old"), path));
        QCOMPARE(read(path), bytes);
        QCOMPARE(h.settings.loadQuickGunnerPreferences().directory, h.temp.path());
        QCOMPARE(h.settings.loadQuickGunnerPreferences().region->pixelRect, previous);
    }
    void pickerGeometryAndReplacement()
    {
        QPointer<QuickGunnerRegionPicker> picker = new QuickGunnerRegionPicker(frame());
        picker->show();
        QCOMPARE(picker->geometry(), frame().display.logicalGeometry);
        QVERIFY(picker->windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(picker->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QSignalSpy selected(picker, &QuickGunnerRegionPicker::regionSelected);
        QSignalSpy cancelled(picker, &QuickGunnerRegionPicker::cancelled);
        drag(picker, {10, 20}, {110, 100});
        QCOMPARE(picker->selection(), QRect(20, 40, 200, 160));
        drag(picker, {150, 120}, {120, 100});
        QCOMPARE(picker->selection(), QRect(240, 200, 60, 40));
        QTest::keyClick(picker, Qt::Key_Return);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.at(0).at(0).toRect(), QRect(240, 200, 60, 40));
        QCOMPARE(cancelled.size(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!picker);
    }
    void pickerClampMinimumAndEscape()
    {
        auto* picker = new QuickGunnerRegionPicker(frame());
        picker->show();
        QSignalSpy selected(picker, &QuickGunnerRegionPicker::regionSelected);
        QSignalSpy cancelled(picker, &QuickGunnerRegionPicker::cancelled);
        QTest::keyClick(picker, Qt::Key_Enter);
        drag(picker, {20, 20}, {27, 28});
        QTest::keyClick(picker, Qt::Key_Return);
        QCOMPARE(selected.size(), 0);
        QVERIFY(picker->findChild<QLabel*>()->text().contains(QStringLiteral("16")));
        drag(picker, {-20, -30}, {450, 350});
        QCOMPARE(picker->selection(), QRect(0, 0, 800, 600));
        drag(picker, {0, 0}, {8, 8});
        QCOMPARE(picker->selection(), QRect(0, 0, 16, 16));
        QTest::keyClick(picker, Qt::Key_Escape);
        QCOMPARE(selected.size(), 0);
        QCOMPARE(cancelled.size(), 1);
    }
    void pickerPaintAndClose()
    {
        auto* picker = new QuickGunnerRegionPicker(frame());
        picker->show();
        drag(picker, {100, 100}, {200, 200});
        const auto image = picker->grab().toImage();
        const qreal scale = image.devicePixelRatio();
        QCOMPARE(image.pixelColor(qRound(150 * scale), qRound(150 * scale)), QColor(Qt::green));
        QVERIFY(image.pixelColor(qRound(50 * scale), qRound(150 * scale)).green() < 200);
        QSignalSpy cancelled(picker, &QuickGunnerRegionPicker::cancelled);
        picker->close();
        QCOMPARE(cancelled.size(), 1);
    }
    void framingSuccessCancelAndActivation()
    {
        Harness h;
        QVERIFY(!h.controller.active());
        QVERIFY(!h.controller.activate({}).isEmpty());
        QVERIFY(h.controller.setDirectory(h.temp.path(), {}).isEmpty());
        QVERIFY(!h.controller.activate({}).isEmpty());
        QVERIFY(h.controller.beginFraming().isEmpty());
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Framing);
        QVERIFY(!h.controller.beginFraming().isEmpty());
        QVERIFY(!h.controller.activate({}).isEmpty());
        h.capture.complete();
        QVERIFY(h.picker);
        h.picker->select({20, 40, 100, 80});
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        QCOMPARE(h.settings.loadQuickGunnerPreferences().region->pixelRect, QRect(20, 40, 100, 80));
        QVERIFY(h.controller.activate({}).isEmpty());
        QVERIFY(h.controller.active());
        h.controller.deactivate();
        QVERIFY(!h.controller.active());
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.capture.complete(1);
        h.picker->cancel();
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        QCOMPARE(h.controller.preferences().region->pixelRect, QRect(20, 40, 100, 80));
        QuickGunnerController restored(h.settings, h.displays, h.capture, {});
        QVERIFY(!restored.active());
        QVERIFY(restored.preferences().region);
        for (int field = 0; field < 4; ++field)
        {
            h.displays.result.target = frame().display;
            if (field == 0)
                ++h.displays.result.target.id;
            if (field == 1)
                h.displays.result.target.logicalGeometry.translate(1, 0);
            if (field == 2)
                h.displays.result.target.devicePixelRatio = 1;
            if (field == 3)
                ++h.displays.result.target.layoutVersion;
            QVERIFY(!restored.activate({}).isEmpty());
            QVERIFY(!restored.active());
        }
        h.displays.result.target = frame().display;
        QVERIFY(!restored.activate({h.temp.path()}).isEmpty());
        QVERIFY(restored.activate({}).isEmpty());
        QCOMPARE(h.capture.calls, 2);
    }
    void framingFailuresAndLateCallbacks()
    {
        Harness h;
        h.capture.permission = false;
        QVERIFY(!h.controller.beginFraming().isEmpty());
        QCOMPARE(h.capture.calls, 0);
        h.capture.permission = true;
        h.displays.result.error = QStringLiteral("没有显示器");
        QVERIFY(!h.controller.beginFraming().isEmpty());
        h.displays.result.error.clear();
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.controller.deactivate();
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.capture.complete(0);
        QVERIFY(!h.picker);
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Framing);
        h.capture.complete(1, {{}, CaptureError::Unavailable, QStringLiteral("采集失败")});
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.capture.complete(2);
        auto old = h.picker;
        h.controller.deactivate();
        QVERIFY(h.controller.beginFraming().isEmpty());
        old->select({0, 0, 40, 40});
        QVERIFY(!h.controller.preferences().region);
        h.capture.complete(3);
        h.displays.matches = false;
        h.picker->select({0, 0, 40, 40});
        QVERIFY(!h.controller.preferences().region);
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
    }
    void invalidFramePickerAndLifetime()
    {
        Harness h;
        QVERIFY(h.controller.beginFraming().isEmpty());
        auto bad = frame();
        bad.display.id++;
        h.capture.complete(0, {bad, CaptureError::None, {}});
        QVERIFY(!h.picker);
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        QVERIFY(h.controller.beginFraming().isEmpty());
        h.capture.complete(1);
        h.picker->select({0, 0, 15, 16});
        QVERIFY(!h.controller.preferences().region);
        auto* parent = new QWidget;
        QVERIFY(h.controller.beginFraming(parent).isEmpty());
        delete parent;
        h.capture.complete(2);
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        auto* controller = new QuickGunnerController(h.settings, h.displays, h.capture, {});
        QVERIFY(controller->beginFraming().isEmpty());
        delete controller;
        h.capture.complete(3);
    }
    void duplicateCallbackAndParentClosesPicker()
    {
        Harness h;
        auto* owner = new QWidget;
        QVERIFY(h.controller.beginFraming(owner).isEmpty());
        h.capture.complete();
        const auto first = h.picker;
        h.capture.complete();
        QCOMPARE(h.picker, first);
        delete owner;
        QCOMPARE(h.controller.state(), QuickGunnerController::State::Idle);
        QVERIFY(!h.picker);
    }
    void factoryFailureAndFrameSize()
    {
        Harness h;
        QuickGunnerController controller(h.settings, h.displays, h.capture,
                                         [](CaptureFrame, QWidget*) -> QuickGunnerRegionPicker*
                                         { return nullptr; });
        QSignalSpy notices(&controller, &QuickGunnerController::noticeChanged);
        QVERIFY(controller.beginFraming().isEmpty());
        h.capture.complete();
        QCOMPARE(controller.state(), QuickGunnerController::State::Idle);
        QCOMPARE(notices.size(), 1);
        QVERIFY(controller.beginFraming().isEmpty());
        auto invalid = frame();
        invalid.pixels = QImage(400, 300, QImage::Format_RGB32);
        h.capture.complete(1, {invalid, CaptureError::None, {}});
        QCOMPARE(controller.state(), QuickGunnerController::State::Idle);
        QCOMPARE(notices.size(), 2);
    }
};
QTEST_MAIN(QuickGunnerRegionPickerTest)
#include "quick_gunner_region_picker_test.moc"
