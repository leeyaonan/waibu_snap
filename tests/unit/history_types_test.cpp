#include "core/history_types.h"
#include <QTest>
using namespace waibusnap;
class HistoryTypesTest final : public QObject
{
    Q_OBJECT
  private slots:
    void names_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("valid");
        for (const auto& name :
             {"",     " ",        ".",    "..",   ".waibusnap", ".WAIBUSNAP", ".hidden", "a/b",
              "a\\b", "a:b",      "a?b",  "a*b",  "a|b",        "a<b",        "a>b",     "a\"b",
              "a\n",  "\tname",   "a.",   "con",  "CON.png",    "prn",        "AUX",     "NUL",
              "com1", "COM9.jpg", "lpt1", "LPT9", "CLOCK$"})
            QTest::newRow(qPrintable(QString::fromUtf8(name).toHtmlEscaped()))
                << QString::fromUtf8(name) << false;
        for (const auto& name :
             {"课程笔记", "  课程笔记  ", "hello.world", "COM10", "LPT0", "会议-20261020"})
            QTest::newRow(name) << QString::fromUtf8(name) << true;
    }
    void names()
    {
        QFETCH(QString, name);
        QFETCH(bool, valid);
        QCOMPARE(historyGroupNameError(name).isEmpty(), valid);
    }
    void normalizationAndDuplicates()
    {
        QCOMPARE(normalizedHistoryGroupName(QStringLiteral("  课程  ")), QStringLiteral("课程"));
        QVERIFY(
            !historyGroupNameError(QStringLiteral("notes"), {QStringLiteral("Notes")}).isEmpty());
        QVERIFY(!historyGroupNameError(QString::fromUtf8("e\xcc\x81"), {QString::fromUtf8("é")})
                     .isEmpty());
        QVERIFY(!historyGroupNameError(QStringLiteral("COM¹.txt")).isEmpty());
        QVERIFY(!historyGroupNameError(QString(QChar(0)) + QStringLiteral("a")).isEmpty());
    }
    void policyDefaultsValidationAndJson()
    {
        CleanupPolicy policy;
        QVERIFY(policy.enabled && policy.ageEnabled && policy.countEnabled && policy.bytesEnabled);
        QCOMPARE(policy.maxAgeDays, 30);
        QCOMPARE(policy.maxCount, 100);
        QCOMPARE(policy.maxBytes, 104857600);
        CleanupPolicy restored;
        QVERIFY(cleanupPolicyFromJson(cleanupPolicyToJson(policy), restored));
        QCOMPARE(cleanupPolicyToJson(restored), cleanupPolicyToJson(policy));
        policy.ageEnabled = policy.countEnabled = policy.bytesEnabled = false;
        QVERIFY(!cleanupPolicyError(policy).isEmpty());
        policy.enabled = false;
        QVERIFY(cleanupPolicyError(policy).isEmpty());
        QVERIFY(cleanupPolicyFromJson(cleanupPolicyToJson(policy), restored));
        for (const auto* key : {"enabled", "maxCount", "maxAgeDays", "maxBytes"})
        {
            auto json = cleanupPolicyToJson(CleanupPolicy{});
            json.insert(QLatin1String(key), QStringLiteral("错误类型"));
            QVERIFY(!cleanupPolicyFromJson(json, restored));
            json.insert(QLatin1String(key), -1);
            QVERIFY(!cleanupPolicyFromJson(json, restored));
            json.insert(QLatin1String(key), 1.25);
            QVERIFY(!cleanupPolicyFromJson(json, restored));
            json.remove(QLatin1String(key));
            QVERIFY(!cleanupPolicyFromJson(json, restored));
        }
    }
    void deterministicOrderingAndUsage()
    {
        HistoryItem a;
        a.groupId = QStringLiteral("g");
        a.fileName = QStringLiteral("a.png");
        a.managed = true;
        a.starred = true;
        a.bytes = 100;
        a.createdAtMs = 10;
        HistoryItem b = a;
        b.fileName = QStringLiteral("b.png");
        b.managed = false;
        b.bytes = 200;
        HistoryItem c = a;
        c.fileName = QStringLiteral("c.png");
        c.starred = false;
        c.createdAtMs = 20;
        const auto oldest = sortedHistoryItems({c, b, a}, HistorySort::OldestFirst);
        QCOMPARE(oldest[0].fileName, a.fileName);
        QCOMPARE(oldest[1].fileName, b.fileName);
        QCOMPARE(oldest[2].fileName, c.fileName);
        const auto newest = sortedHistoryItems({a, b, c}, HistorySort::NewestFirst);
        QCOMPARE(newest[0].fileName, c.fileName);
        QCOMPARE(newest[2].fileName, a.fileName);
        const auto usage = historyUsage(oldest);
        QCOMPARE(usage.managedCount, 2);
        QCOMPARE(usage.externalCount, 1);
        QCOMPARE(usage.starredCount, 2);
        QCOMPARE(usage.managedBytes, 200);
        QCOMPARE(usage.externalBytes, 200);
        QCOMPARE(historyItemId(a), QStringLiteral("g/a.png"));
    }
};
QTEST_APPLESS_MAIN(HistoryTypesTest)
#include "history_types_test.moc"
