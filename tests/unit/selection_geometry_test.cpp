#include "core/selection_geometry.h"
#include "session/session_metrics.h"
#include <QTest>
class SelectionGeometryTest final : public QObject
{
    Q_OBJECT
  private slots:
    void reversedAndRetina()
    {
        QCOMPARE(waibusnap::normalizedPixelSelection({30, 40}, {10, 20}, 2, {200, 200}),
                 QRect(20, 40, 40, 40));
    }
    void edgesAreHalfOpenAndClamped()
    {
        QCOMPARE(waibusnap::normalizedPixelSelection({-3, -5}, {200, 100}, 1.5, {300, 150}),
                 QRect(0, 0, 300, 150));
        QCOMPARE(waibusnap::normalizedPixelSelection({0.2, 0.2}, {0.3, 0.3}, 2, {100, 100}),
                 QRect(0, 0, 1, 1));
        QVERIFY(waibusnap::normalizedPixelSelection({5, 5}, {5, 8}, 2, {100, 100}).isEmpty());
        QVERIFY(waibusnap::normalizedPixelSelection({0, 0}, {5, 8}, 0, {100, 100}).isEmpty());
    }
    void incompleteTimingIsNotSuccess()
    {
        waibusnap::SessionMetrics metrics;
        metrics.sequence = 1;
        metrics.t0 = 1000000;
        metrics.captureStart = 2000000;
        metrics.captureComplete = 4000000;
        const auto json = metrics.toJson();
        QVERIFY(json.value("cold").toBool());
        QCOMPARE(json.value("capture_ms").toDouble(), 2.0);
        QVERIFY(!json.contains("nf01_proxy_ms"));
        QVERIFY(!json.contains("interactive_ns"));
    }
};
QTEST_APPLESS_MAIN(SelectionGeometryTest)
#include "selection_geometry_test.moc"
