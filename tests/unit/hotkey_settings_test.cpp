#include "app/app_settings.h"
#include "app/hotkey_rules.h"
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
namespace
{
class FakeHotkey final : public waibusnap::GlobalHotkey
{
  public:
    QStringList rejected;
    QVector<QKeySequence> attempts;
    QKeySequence enabled;
    std::function<void()> handler;
    std::function<void()> beforeRegistration;
    waibusnap::HotkeyRegistration registerHotkey(const QKeySequence& sequence,
                                                 std::function<void()> callback) override
    {
        attempts.append(sequence);
        if (beforeRegistration)
            beforeRegistration();
        if (rejected.contains(sequence.toString(QKeySequence::PortableText)))
            return {false, QStringLiteral("测试占用：%1，原绑定保留")
                               .arg(sequence.toString(QKeySequence::NativeText))};
        enabled = sequence;
        handler = std::move(callback);
        return {
            true,
            QStringLiteral("截图键 %1 已启用").arg(sequence.toString(QKeySequence::NativeText))};
    }
    void unregister() override
    {
        enabled = {};
        handler = {};
    }
};
}
class HotkeySettingsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void savePreferencesDefaultsRoundTripAndHotkeyIsolation()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("设置 空格/settings.ini"));
        AppSettings settings(path);
        QVERIFY(settings.loadSavePreferences().quickDirectory.isEmpty());
        QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Png);
        QVERIFY(!QFile::exists(path));
        const auto sequence = QKeySequence::fromString(QStringLiteral("Ctrl+Shift+2"));
        QVERIFY(settings.saveHotkey(sequence).isEmpty());
        const QString directory = temporary.filePath(QStringLiteral("中文 图片"));
        QVERIFY(settings.saveSavePreferences({directory, ImageFormat::Jpeg}).isEmpty());
        const auto loaded = AppSettings(path).loadSavePreferences();
        QCOMPARE(loaded.quickDirectory, directory);
        QCOMPARE(loaded.format, ImageFormat::Jpeg);
        QCOMPARE(settings.loadHotkey().sequence, sequence);
        QVERIFY(settings.saveHotkey(defaultScreenshotHotkey()).isEmpty());
        QCOMPARE(settings.loadSavePreferences().quickDirectory, directory);
        QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Jpeg);
        {
            QSettings ini(path, QSettings::IniFormat);
            QCOMPARE(ini.value(QStringLiteral("save/quickDirectory")).toString(), directory);
            QCOMPARE(ini.value(QStringLiteral("save/format")).toString(), QStringLiteral("jpeg"));
            QCOMPARE(ini.allKeys().size(), 3);
        }
        QVERIFY(settings.saveSavePreferences({}).isEmpty());
        QVERIFY(settings.loadSavePreferences().quickDirectory.isEmpty());
        QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Png);
        QCOMPARE(settings.loadHotkey().sequence, defaultScreenshotHotkey());
        QVERIFY(!settings.saveSavePreferences({QStringLiteral("relative"), ImageFormat::Jpeg})
                     .isEmpty());
        QVERIFY(!settings.saveSavePreferences({{}, static_cast<ImageFormat>(99)}).isEmpty());
        QCOMPARE(settings.loadSavePreferences().format, ImageFormat::Png);
    }
    void invalidSaveValuesFallBackWithoutRewriting_data()
    {
        QTest::addColumn<QString>("directory");
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("jpeg");
        QTest::newRow("invalid-format") << QDir::tempPath() << QStringLiteral("webp") << false;
        QTest::newRow("empty-format") << QDir::tempPath() << QString() << false;
        QTest::newRow("uppercase-format") << QDir::tempPath() << QStringLiteral("JPEG") << false;
        QTest::newRow("relative-directory")
            << QStringLiteral("relative/path") << QStringLiteral("jpeg") << true;
    }
    void invalidSaveValuesFallBackWithoutRewriting()
    {
        using namespace waibusnap;
        QFETCH(QString, directory);
        QFETCH(QString, format);
        QFETCH(bool, jpeg);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("save/quickDirectory"), directory);
            ini.setValue(QStringLiteral("save/format"), format);
            ini.setValue(QStringLiteral("hotkey/sequence"), QStringLiteral("F2"));
            ini.sync();
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray before = file.readAll();
        file.close();
        AppSettings settings(path);
        const auto loaded = settings.loadSavePreferences();
        QCOMPARE(loaded.quickDirectory, QDir::isAbsolutePath(directory) ? directory : QString());
        QCOMPARE(loaded.format, jpeg ? ImageFormat::Jpeg : ImageFormat::Png);
        QCOMPARE(settings.loadHotkey().sequence, QKeySequence(Qt::Key_F2));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), before);
    }
    void malformedSaveIniPreservesBytesAndWriteFailure()
    {
        using namespace waibusnap;
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray malformed("[save\nformat=jpeg\nquickDirectory=/tmp\n");
        QCOMPARE(file.write(malformed), qint64(malformed.size()));
        file.close();
        const auto loaded = AppSettings(path).loadSavePreferences();
        QVERIFY(loaded.quickDirectory.isEmpty());
        QCOMPARE(loaded.format, ImageFormat::Png);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), malformed);
        file.close();
        QVERIFY(
            !AppSettings(path + QStringLiteral("/settings.ini")).saveSavePreferences({}).isEmpty());
    }
    void legalMatrix_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("valid");
        for (const QString& text :
             {QStringLiteral("F1"), QStringLiteral("F12"), QStringLiteral("Shift+F2"),
              QStringLiteral("Ctrl+Shift+2"), QStringLiteral("Meta+A"), QStringLiteral("Alt+0"),
              QStringLiteral("Ctrl+Alt+Meta+Shift+Z")})
            QTest::newRow(qPrintable(text)) << text << true;
        for (const QString& text :
             {QString(), QStringLiteral("A"), QStringLiteral("2"), QStringLiteral("Shift+A"),
              QStringLiteral("Shift+2"), QStringLiteral("Space"), QStringLiteral("Tab"),
              QStringLiteral("Esc"), QStringLiteral("Ctrl+Esc"), QStringLiteral("F13"),
              QStringLiteral("Ctrl+K, Ctrl+C"), QStringLiteral("not-a-key")})
            QTest::newRow(qPrintable(text.isEmpty() ? QStringLiteral("empty") : text))
                << text << false;
    }
    void legalMatrix()
    {
        QFETCH(QString, text);
        QFETCH(bool, valid);
        const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
        QCOMPARE(waibusnap::hotkeyValidationError(sequence).isEmpty(), valid);
        if (valid)
        {
            QCOMPARE(QKeySequence::fromString(sequence.toString(QKeySequence::PortableText),
                                              QKeySequence::PortableText),
                     sequence);
            QCOMPARE(QKeySequence::fromString(sequence.toString(QKeySequence::NativeText),
                                              QKeySequence::NativeText),
                     sequence);
        }
    }
    void temporaryIniDefaultsAndRoundTrip()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("设置 空格/settings.ini"));
        waibusnap::AppSettings settings(path);
        const auto defaults = settings.loadHotkey();
        QCOMPARE(defaults.sequence, waibusnap::defaultScreenshotHotkey());
        QVERIFY(defaults.explanation.isEmpty());
        QVERIFY(!QFile::exists(path));
        const auto chosen = QKeySequence::fromString(QStringLiteral("Ctrl+Shift+2"));
        QVERIFY(settings.saveHotkey(chosen).isEmpty());
        QCOMPARE(waibusnap::AppSettings(path).loadHotkey().sequence, chosen);
        QSettings ini(path, QSettings::IniFormat);
        QCOMPARE(ini.allKeys(), QStringList({QStringLiteral("hotkey/sequence")}));
        QCOMPARE(ini.value(QStringLiteral("hotkey/sequence")).toString(),
                 QStringLiteral("Ctrl+Shift+2"));
        QVERIFY(!settings.saveHotkey(QKeySequence(Qt::Key_A)).isEmpty());
        QCOMPARE(settings.loadHotkey().sequence, chosen);
    }
    void damagedValueFallsBack_data()
    {
        QTest::addColumn<QString>("text");
        for (const QString& text :
             {QString(), QStringLiteral("A"), QStringLiteral("Shift+2"), QStringLiteral("broken"),
              QStringLiteral("F1, F2"), QStringLiteral("F1, F2, F3, F4, F5"),
              QStringLiteral("F1+garbage")})
            QTest::newRow(qPrintable(text.isEmpty() ? QStringLiteral("empty") : text)) << text;
    }
    void damagedValueFallsBack()
    {
        QFETCH(QString, text);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("hotkey/sequence"), text);
            ini.sync();
        }
        const auto loaded = waibusnap::AppSettings(path).loadHotkey();
        QCOMPARE(loaded.sequence, waibusnap::defaultScreenshotHotkey());
        QVERIFY(loaded.explanation.contains(QStringLiteral("回退默认 F1")));
        QSettings ini(path, QSettings::IniFormat);
        QCOMPARE(ini.value(QStringLiteral("hotkey/sequence")).toString(), text);
    }
    void malformedIniAndWriteFailure()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[hotkey\nsequence=F2\n");
        file.close();
        QVERIFY(!waibusnap::AppSettings(path).loadHotkey().explanation.isEmpty());
        QVERIFY(!waibusnap::AppSettings(path + QStringLiteral("/child.ini"))
                     .saveHotkey(waibusnap::defaultScreenshotHotkey())
                     .isEmpty());
    }
    void startupFallbackChain_data()
    {
        QTest::addColumn<int>("scenario");
        QTest::newRow("stored-success") << 0;
        QTest::newRow("stored-fails-default-success") << 1;
        QTest::newRow("both-fail") << 2;
        QTest::newRow("invalid-default-success") << 3;
        QTest::newRow("invalid-default-fails") << 4;
    }
    void startupFallbackChain()
    {
        QFETCH(int, scenario);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        waibusnap::AppSettings settings(path);
        const QString stored = scenario >= 3 ? QStringLiteral("A") : QStringLiteral("Ctrl+Shift+2");
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("hotkey/sequence"), stored);
            ini.sync();
        }
        FakeHotkey fake;
        if (scenario == 1 || scenario == 2)
            fake.rejected.append(QStringLiteral("Ctrl+Shift+2"));
        if (scenario == 2 || scenario == 4)
            fake.rejected.append(QStringLiteral("F1"));
        waibusnap::HotkeySettings preferences(settings, fake, [] {});
        const auto startup = preferences.initialize();
        QCOMPARE(fake.attempts.size(),
                 scenario == 1 || scenario == 2 ? qsizetype(2) : qsizetype(1));
        QCOMPARE(preferences.enabled(), scenario != 2 && scenario != 4);
        QCOMPARE(startup.warning.isEmpty(), scenario == 0);
        if (scenario == 0)
            QCOMPARE(preferences.currentSequence(), QKeySequence::fromString(stored));
        else
            QCOMPARE(preferences.currentSequence(), waibusnap::defaultScreenshotHotkey());
        if (scenario >= 3)
            QVERIFY(startup.report.contains(QStringLiteral("回退默认 F1")));
        if (scenario == 1 || scenario == 2)
            QVERIFY(startup.report.contains(QStringLiteral("尝试默认 F1")));
        if (preferences.enabled())
            QCOMPARE(preferences.tooltip(),
                     QStringLiteral("WaibuSnap · 截图键 %1")
                         .arg(fake.enabled.toString(QKeySequence::NativeText)));
        else
        {
            QVERIFY(preferences.tooltip().contains(QStringLiteral("测试占用")));
            QVERIFY(!preferences.tooltip().contains(QStringLiteral("已启用")));
        }
        QSettings ini(path, QSettings::IniFormat);
        QCOMPARE(ini.value(QStringLiteral("hotkey/sequence")).toString(), stored);
    }
    void changeRegistersBeforeWritingAndFailurePreservesState()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("settings.ini"));
        waibusnap::AppSettings settings(path);
        QVERIFY(settings.saveHotkey(waibusnap::defaultScreenshotHotkey()).isEmpty());
        FakeHotkey fake;
        int triggers = 0;
        waibusnap::HotkeySettings preferences(settings, fake, [&] { ++triggers; });
        preferences.initialize();
        const auto original = fake.enabled;
        const auto chosen = QKeySequence::fromString(QStringLiteral("Ctrl+Shift+2"));
        fake.rejected.append(QStringLiteral("Ctrl+Shift+2"));
        QVERIFY(!preferences.changeHotkey(chosen).isEmpty());
        QCOMPARE(fake.enabled, original);
        QCOMPARE(preferences.currentSequence(), original);
        QCOMPARE(settings.loadHotkey().sequence, original);
        QCOMPARE(preferences.tooltip(), QStringLiteral("WaibuSnap · 截图键 F1"));
        fake.handler();
        QCOMPARE(triggers, 1);
        const auto attempts = fake.attempts.size();
        QVERIFY(!preferences.changeHotkey(QKeySequence(Qt::Key_A)).isEmpty());
        QCOMPARE(fake.attempts.size(), attempts);
        fake.rejected.clear();
        fake.beforeRegistration = [&] { QCOMPARE(settings.loadHotkey().sequence, original); };
        QVERIFY(preferences.changeHotkey(chosen).isEmpty());
        QCOMPARE(fake.enabled, chosen);
        QCOMPARE(preferences.currentSequence(), chosen);
        QCOMPARE(settings.loadHotkey().sequence, chosen);
        QCOMPARE(waibusnap::AppSettings(path).loadHotkey().sequence, chosen);
        fake.handler();
        QCOMPARE(triggers, 2);
    }
    void saveFailureDoesNotClaimPersistence()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("blocker"));
        QFile blocker(path);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        FakeHotkey fake;
        waibusnap::HotkeySettings preferences(
            waibusnap::AppSettings(path + QStringLiteral("/settings.ini")), fake, [] {});
        const auto chosen = QKeySequence::fromString(QStringLiteral("Ctrl+Shift+2"));
        const QString error = preferences.changeHotkey(chosen);
        QVERIFY(error.contains(QStringLiteral("重启后可能无法保留")));
        QVERIFY(preferences.enabled());
        QCOMPARE(preferences.currentSequence(), chosen);
        QVERIFY(preferences.tooltip().contains(chosen.toString(QKeySequence::NativeText)));
    }
};
QTEST_MAIN(HotkeySettingsTest)
#include "hotkey_settings_test.moc"
