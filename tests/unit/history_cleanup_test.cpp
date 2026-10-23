#include "core/history_cleanup.h"
#include <QTest>
#include <limits>
using namespace waibusnap;
namespace
{
constexpr qint64 day = 86400000;
HistoryItem item(const QString& name, qint64 time, qint64 bytes = 10)
{
    HistoryItem result;
    result.groupId = QStringLiteral("g");
    result.fileName = name;
    result.managed = true;
    result.source = HistorySource::Overlay;
    result.createdAtMs = time;
    result.bytes = bytes;
    return result;
}
}
class HistoryCleanupTest final : public QObject
{
    Q_OBJECT
  private slots:
    void orRules_data()
    {
        QTest::addColumn<int>("rule");
        QTest::newRow("age-only-exceeded") << 0;
        QTest::newRow("count-only-exceeded") << 1;
        QTest::newRow("bytes-only-exceeded") << 2;
    }
    void orRules()
    {
        QFETCH(int, rule);
        CleanupPolicy policy;
        policy.maxCount = rule == 1 ? 2 : 100;
        policy.maxBytes = rule == 2 ? 20 : 1000;
        const qint64 now = 40 * day;
        const auto a = item(QStringLiteral("a"), rule == 0 ? day : 39 * day);
        const auto b = item(QStringLiteral("b"), 39 * day + 1);
        const auto c = item(QStringLiteral("c"), 39 * day + 2);
        const auto result = selectCleanupCandidates(policy, {c, a, b}, now);
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.candidates.size(), 1);
        QCOMPARE(result.candidates[0].fileName, a.fileName);
        QCOMPARE(result.releasedBytes, 10);
        QCOMPARE(result.protectedCount, 1);
        QVERIFY(!result.protectedOverLimit);
    }
    void mixedRulesContinueUntilSatisfied()
    {
        CleanupPolicy policy;
        policy.maxCount = 3;
        policy.maxBytes = 20;
        const auto result = selectCleanupCandidates(
            policy,
            {item(QStringLiteral("c"), 39 * day), item(QStringLiteral("a"), day),
             item(QStringLiteral("d"), 40 * day), item(QStringLiteral("b"), 2 * day)},
            40 * day);
        QCOMPARE(result.candidates.size(), 2);
        QCOMPARE(result.candidates[0].fileName, QStringLiteral("a"));
        QCOMPARE(result.candidates[1].fileName, QStringLiteral("b"));
        QCOMPARE(result.releasedBytes, 20);
        QVERIFY(!result.protectedOverLimit);
    }
    void originalBytesCountTowardQuota()
    {
        CleanupPolicy policy;
        policy.maxCount = 10;
        policy.maxBytes = 25;
        policy.ageEnabled = false;
        auto old = item(QStringLiteral("old"), day, 10);
        old.originalBytes = 10;
        const auto latest = item(QStringLiteral("latest"), 2 * day, 10);
        auto result = selectCleanupCandidates(policy, {old, latest}, 40 * day);
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.candidates.size(), 1);
        QCOMPARE(result.candidates[0].fileName, old.fileName);
        QCOMPARE(result.releasedBytes, 20);
        QVERIFY(!result.protectedOverLimit);
        old.originalBytes = std::numeric_limits<qint64>::max();
        result = selectCleanupCandidates(policy, {old, latest}, 40 * day);
        QVERIFY(!result.error.isEmpty());
    }
    void protectionsAndExternalNeverCandidates()
    {
        CleanupPolicy policy;
        policy.maxCount = 1;
        policy.maxBytes = 100;
        auto starred = item(QStringLiteral("starred"), day, 20);
        starred.starred = true;
        const auto selected = item(QStringLiteral("selected"), 2 * day, 30);
        const auto oversized = item(QStringLiteral("oversized"), 3 * day, 101);
        auto external = item(QStringLiteral("external"), 0, 1000000);
        external.managed = false;
        external.source = HistorySource::External;
        const auto removable = item(QStringLiteral("removable"), 4 * day, 10);
        const auto latest = item(QStringLiteral("latest"), 5 * day, 40);
        const auto result = selectCleanupCandidates(
            policy, {starred, latest, external, removable, oversized, selected}, 40 * day,
            historyItemId(selected));
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.candidates.size(), 1);
        QCOMPARE(result.candidates[0].fileName, removable.fileName);
        QCOMPARE(result.releasedBytes, 10);
        QCOMPARE(result.protectedCount, 4);
        QCOMPARE(result.protectedBytes, 191);
        QVERIFY(result.protectedOverLimit);
    }
    void disabledEmptyExactLimitsAndNoCandidates()
    {
        CleanupPolicy policy;
        policy.maxCount = 2;
        policy.maxBytes = 20;
        const auto a = item(QStringLiteral("a"), 10 * day);
        const auto b = item(QStringLiteral("b"), 11 * day);
        auto result = selectCleanupCandidates(policy, {b, a}, 40 * day);
        QVERIFY(result.candidates.isEmpty());
        QVERIFY(!result.protectedOverLimit);
        policy.enabled = false;
        result = selectCleanupCandidates(policy, {a, b}, 100 * day);
        QVERIFY(result.candidates.isEmpty());
        QVERIFY(!result.protectedOverLimit);
        QCOMPARE(result.protectedCount, 1);
        result = selectCleanupCandidates(CleanupPolicy{}, {}, 0);
        QVERIFY(result.candidates.isEmpty());
        QCOMPARE(result.protectedBytes, 0);
        policy = {};
        policy.maxBytes = 1;
        result = selectCleanupCandidates(policy, {b, a}, 100 * day);
        QCOMPARE(result.protectedCount, 2);
        QVERIFY(result.candidates.isEmpty());
        QVERIFY(result.protectedOverLimit);
    }
    void protectedAgeDoesNotEvictFreshImages()
    {
        auto old = item(QStringLiteral("old"), day);
        old.starred = true;
        const auto fresh = item(QStringLiteral("fresh"), 39 * day);
        const auto latest = item(QStringLiteral("latest"), 40 * day);
        const auto result = selectCleanupCandidates({}, {old, fresh, latest}, 40 * day);
        QVERIFY(result.candidates.isEmpty());
        QVERIFY(result.protectedOverLimit);
        QCOMPARE(result.protectedCount, 2);
    }
    void tieBreakAndDeduplicatedProtection()
    {
        CleanupPolicy policy;
        policy.ageEnabled = false;
        policy.bytesEnabled = false;
        policy.maxCount = 1;
        auto b = item(QStringLiteral("b"), 0);
        b.starred = true;
        const auto a = item(QStringLiteral("a"), 0);
        const auto result = selectCleanupCandidates(policy, {b, a}, 1, historyItemId(b));
        QCOMPARE(result.candidates.size(), 1);
        QCOMPARE(result.candidates[0].fileName, a.fileName);
        QCOMPARE(result.protectedCount, 1);
        QCOMPARE(result.protectedBytes, 10);
        QVERIFY(!result.protectedOverLimit);
    }
    void invalidPolicyOrRecordsFailClosed()
    {
        CleanupPolicy policy;
        policy.ageEnabled = policy.countEnabled = policy.bytesEnabled = false;
        QVERIFY(!selectCleanupCandidates(policy, {}, 0).error.isEmpty());
        auto a = item(QStringLiteral("a"), 0);
        auto b = item(QStringLiteral("b"), 1);
        a.bytes = -1;
        QVERIFY(!selectCleanupCandidates({}, {a}, 1).error.isEmpty());
        a.bytes = std::numeric_limits<qint64>::max();
        QVERIFY(!selectCleanupCandidates({}, {a, b}, 1).error.isEmpty());
        a.bytes = 1;
        b.groupId = QStringLiteral("other");
        QVERIFY(!selectCleanupCandidates({}, {a, b}, 1).error.isEmpty());
        QVERIFY(!selectCleanupCandidates({}, {a, a}, 1).error.isEmpty());
        a.createdAtMs = -1;
        QVERIFY(!selectCleanupCandidates({}, {a}, 1).error.isEmpty());
        a.createdAtMs = 0;
        a.source = HistorySource::External;
        QVERIFY(!selectCleanupCandidates({}, {a}, 1).error.isEmpty());
    }
};
QTEST_APPLESS_MAIN(HistoryCleanupTest)
#include "history_cleanup_test.moc"
