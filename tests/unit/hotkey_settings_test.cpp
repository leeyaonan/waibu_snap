#include "app/app_settings.h"
#include "app/hotkey_rules.h"
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
class HotkeySettingsTest final : public QObject
{
    Q_OBJECT
  private slots:
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
};
QTEST_MAIN(HotkeySettingsTest)
#include "hotkey_settings_test.moc"
