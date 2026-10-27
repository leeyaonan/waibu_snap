#include "app/hotkey_rules.h"
#include "app/quick_gunner_hotkey.h"
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
namespace
{
constexpr auto shutter = HotkeyAction::QuickGunnerShutter;
class FakeHotkey final : public GlobalHotkey
{
  public:
    QStringList rejected;
    QVector<QKeySequence> attempts;
    QVector<HotkeyAction> actions, released;
    QKeySequence enabled;
    std::function<void()> handler, beforeRegistration;
    HotkeyRegistration registerHotkey(HotkeyAction action, const QKeySequence& sequence,
                                      std::function<void()> callback) override
    {
        actions.append(action);
        attempts.append(sequence);
        if (beforeRegistration)
            beforeRegistration();
        if (rejected.contains(sequence.toString(QKeySequence::PortableText)))
            return {false, QStringLiteral("测试占用，原绑定保留。")};
        enabled = sequence;
        handler = std::move(callback);
        return {true, QStringLiteral("测试快门键已启用。")};
    }
    void unregister(HotkeyAction action) override
    {
        released.append(action);
        enabled = {};
        handler = {};
    }
};
}
class QuickGunnerHotkeyTest final : public QObject
{
    Q_OBJECT
  private slots:
    void loadDoesNotRegisterOrWrite()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("设置.ini"));
        FakeHotkey fake;
        QuickGunnerHotkey key(AppSettings(path), fake, [] {});
        key.load();
        QCOMPARE(key.currentSequence(), QKeySequence(Qt::Key_F2));
        QVERIFY(!key.registered());
        QVERIFY(key.explanation().isEmpty());
        QVERIFY(fake.attempts.isEmpty());
        QVERIFY(fake.released.isEmpty());
        QVERIFY(!QFile::exists(path));
    }
    void activateAndDeactivate()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        FakeHotkey fake;
        int calls = 0;
        QuickGunnerHotkey key(AppSettings(temp.filePath(QStringLiteral("设置.ini"))), fake,
                              [&] { ++calls; });
        key.load();
        QVERIFY(key.activate().isEmpty());
        QVERIFY(key.registered());
        QCOMPARE(fake.enabled, QKeySequence(Qt::Key_F2));
        QCOMPARE(fake.actions, QVector<HotkeyAction>{shutter});
        fake.handler();
        QCOMPARE(calls, 1);
        QVERIFY(key.activate().isEmpty());
        QCOMPARE(fake.attempts.size(), 1);
        key.deactivate();
        QVERIFY(!key.registered());
        QVERIFY(fake.enabled.isEmpty());
        QVERIFY(!fake.handler);
        QCOMPARE(fake.released, QVector<HotkeyAction>{shutter});
        key.deactivate();
        QCOMPARE(fake.released, (QVector<HotkeyAction>{shutter, shutter}));
        QVERIFY(key.activate().isEmpty());
        QCOMPARE(fake.enabled, QKeySequence(Qt::Key_F2));
    }
    void occupiedPreferredDoesNotFallBack_data()
    {
        QTest::addColumn<int>("preferred");
        QTest::newRow("default") << int(Qt::Key_F2);
        QTest::newRow("custom") << int(Qt::Key_F3);
    }
    void occupiedPreferredDoesNotFallBack()
    {
        QFETCH(int, preferred);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        AppSettings settings(temp.filePath(QStringLiteral("设置.ini")));
        const QKeySequence sequence(preferred);
        QVERIFY(settings.saveHotkey(sequence, shutter).isEmpty());
        FakeHotkey fake;
        fake.rejected.append(sequence.toString(QKeySequence::PortableText));
        QuickGunnerHotkey key(settings, fake, [] {});
        key.load();
        const auto error = key.activate();
        QVERIFY(error.contains(QStringLiteral("占用")));
        QCOMPARE(key.explanation(), error);
        QVERIFY(!key.registered());
        QVERIFY(fake.enabled.isEmpty());
        QCOMPARE(fake.attempts, QVector<QKeySequence>{sequence});
        QCOMPARE(key.currentSequence(), sequence);
        QCOMPARE(settings.loadHotkey(shutter).sequence, sequence);
    }
    void inactiveChangeOnlySavesAndReloadsCustomKey()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("中文 设置/settings.ini"));
        AppSettings settings(path);
        FakeHotkey fake;
        QuickGunnerHotkey key(settings, fake, [] {});
        key.load();
        QVERIFY(!key.change(QKeySequence(Qt::Key_A)).isEmpty());
        QVERIFY(!QFile::exists(path));
        const QKeySequence chosen(Qt::CTRL | Qt::SHIFT | Qt::Key_2);
        QVERIFY(key.change(chosen).isEmpty());
        QCOMPARE(key.currentSequence(), chosen);
        QVERIFY(!key.registered());
        QVERIFY(fake.attempts.isEmpty());
        QCOMPARE(settings.loadHotkey().sequence, defaultScreenshotHotkey());
        QSettings ini(path, QSettings::IniFormat);
        QCOMPARE(ini.allKeys(), QStringList{QStringLiteral("hotkey/quickGunner")});
        QCOMPARE(ini.value(QStringLiteral("hotkey/quickGunner")).toString(),
                 chosen.toString(QKeySequence::PortableText));
        QuickGunnerHotkey reloaded(AppSettings(path), fake, [] {});
        reloaded.load();
        QCOMPARE(reloaded.currentSequence(), chosen);
        QVERIFY(fake.attempts.isEmpty());
        QVERIFY(reloaded.activate().isEmpty());
        QCOMPARE(fake.attempts, QVector<QKeySequence>{chosen});
    }
    void activeChangeRegistersBeforeSaveAndFailurePreservesOld()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        AppSettings settings(temp.filePath(QStringLiteral("设置.ini")));
        const QKeySequence original(Qt::Key_F2), chosen(Qt::Key_F3);
        QVERIFY(settings.saveHotkey(original, shutter).isEmpty());
        FakeHotkey fake;
        int calls = 0;
        QuickGunnerHotkey key(settings, fake, [&] { ++calls; });
        key.load();
        QVERIFY(key.activate().isEmpty());
        const auto attempts = fake.attempts.size();
        QVERIFY(!key.change(QKeySequence(Qt::Key_A)).isEmpty());
        QCOMPARE(fake.attempts.size(), attempts);
        fake.rejected.append(QStringLiteral("F3"));
        QVERIFY(!key.change(chosen).isEmpty());
        QVERIFY(key.registered());
        QCOMPARE(key.currentSequence(), original);
        QCOMPARE(fake.enabled, original);
        QCOMPARE(settings.loadHotkey(shutter).sequence, original);
        fake.handler();
        QCOMPARE(calls, 1);
        fake.rejected.clear();
        fake.beforeRegistration = [&]
        {
            QCOMPARE(fake.enabled, original);
            QCOMPARE(settings.loadHotkey(shutter).sequence, original);
        };
        QVERIFY(key.change(chosen).isEmpty());
        QCOMPARE(key.currentSequence(), chosen);
        QCOMPARE(fake.enabled, chosen);
        QCOMPARE(settings.loadHotkey(shutter).sequence, chosen);
        QCOMPARE(fake.actions, (QVector<HotkeyAction>{shutter, shutter, shutter}));
        QVERIFY(fake.released.isEmpty());
        fake.handler();
        QCOMPARE(calls, 2);
    }
    void saveFailureKeepsActiveNewKey()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("blocker"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        FakeHotkey fake;
        QuickGunnerHotkey key(AppSettings(path + QStringLiteral("/settings.ini")), fake, [] {});
        key.load();
        QVERIFY(key.activate().isEmpty());
        const QKeySequence chosen(Qt::Key_F3);
        const auto error = key.change(chosen);
        QVERIFY(error.contains(QStringLiteral("重启后可能无法保留")));
        QVERIFY(key.registered());
        QCOMPARE(key.currentSequence(), chosen);
        QCOMPARE(fake.enabled, chosen);
    }
    void damagedConfigFallsBackWithoutWriting_data()
    {
        QTest::addColumn<QByteArray>("contents");
        QTest::newRow("empty") << QByteArray("[hotkey]\nquickGunner=\n");
        QTest::newRow("unmodified-letter") << QByteArray("[hotkey]\nquickGunner=A\n");
        QTest::newRow("garbage") << QByteArray("[hotkey]\nquickGunner=broken\n");
        QTest::newRow("trailing-garbage") << QByteArray("[hotkey]\nquickGunner=F2+garbage\n");
        QTest::newRow("multiple") << QByteArray("[hotkey]\nquickGunner=F2, F3\n");
        QTest::newRow("malformed") << QByteArray("[hotkey\nquickGunner=F3\n");
    }
    void damagedConfigFallsBackWithoutWriting()
    {
        QFETCH(QByteArray, contents);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("设置.ini"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();
        FakeHotkey fake;
        QuickGunnerHotkey key(AppSettings(path), fake, [] {});
        key.load();
        QCOMPARE(key.currentSequence(), QKeySequence(Qt::Key_F2));
        QVERIFY(!key.explanation().isEmpty());
        QVERIFY(!key.registered());
        QVERIFY(fake.attempts.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), contents);
    }
    void fiveActionConflictsAndFourActionCompatibility()
    {
        std::array<QKeySequence, 5> sequences;
        for (size_t i = 0; i < sequences.size(); ++i)
            sequences[i] = defaultHotkey(hotkeyActions[i]);
        QVERIFY(hotkeyConflictError(sequences).isEmpty());
        for (size_t i = 1; i < sequences.size(); ++i)
            for (size_t j = 0; j < i; ++j)
            {
                auto duplicate = sequences;
                duplicate[i] = duplicate[j];
                const auto error = hotkeyConflictError(duplicate);
                QVERIFY(error.contains(hotkeyActionName(hotkeyActions[i])));
                QVERIFY(error.contains(hotkeyActionName(hotkeyActions[j])));
            }
        sequences.back() = QKeySequence(Qt::Key_A);
        QVERIFY(hotkeyConflictError(sequences).contains(QStringLiteral("快枪手快门")));
        sequences[0] = QKeySequence(Qt::Key_F2);
        const std::array<QKeySequence, 4> existing = {sequences[0], sequences[1], sequences[2],
                                                      sequences[3]};
        QVERIFY(hotkeyConflictError(existing).isEmpty());
        sequences.back() = QKeySequence(Qt::Key_F2);
        QVERIFY(!hotkeyConflictError(sequences).isEmpty());
    }
};
QTEST_GUILESS_MAIN(QuickGunnerHotkeyTest)
#include "quick_gunner_hotkey_test.moc"
