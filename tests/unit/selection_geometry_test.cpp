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
    void movementKeepsSizeAndClamps()
    {
        const QRect initial(20, 30, 40, 50);
        QCOMPARE(waibusnap::movedPixelSelection(initial, {-100, -100}, 2, {200, 200}),
                 QRect(0, 0, 40, 50));
        QCOMPARE(waibusnap::movedPixelSelection(initial, {100, 100}, 2, {200, 200}),
                 QRect(160, 150, 40, 50));
        QCOMPARE(waibusnap::movedPixelSelection(initial, {0.34, -0.34}, 1.5, {200, 200}),
                 QRect(21, 29, 40, 50));
        QVERIFY(waibusnap::movedPixelSelection(initial, {}, 0, {200, 200}).isEmpty());
        QCOMPARE(waibusnap::movedPixelSelection(initial, {0.25, -0.25}, 2, {200, 200}),
                 QRect(21, 29, 40, 50));
        QCOMPARE(waibusnap::movedPixelSelection(initial, {0.24, -0.24}, 2, {200, 200}), initial);
    }
    void resizingClampsAtOppositeEdge()
    {
        using waibusnap::SelectionEdge;
        const QRect initial(20, 30, 40, 50);
        QCOMPARE(waibusnap::resizedPixelSelection(initial, SelectionEdge::Left | SelectionEdge::Top,
                                                  {100, 100}, 2, {200, 200}),
                 QRect(59, 79, 1, 1));
        QCOMPARE(waibusnap::resizedPixelSelection(initial,
                                                  SelectionEdge::Right | SelectionEdge::Bottom,
                                                  {-100, -100}, 2, {200, 200}),
                 QRect(20, 30, 1, 1));
        QCOMPARE(waibusnap::resizedPixelSelection(initial, SelectionEdge::Left | SelectionEdge::Top,
                                                  {-100, -100}, 2, {200, 200}),
                 QRect(0, 0, 60, 80));
        QCOMPARE(waibusnap::resizedPixelSelection(initial,
                                                  SelectionEdge::Right | SelectionEdge::Bottom,
                                                  {100, 100}, 2, {200, 200}),
                 QRect(20, 30, 180, 170));
        QCOMPARE(waibusnap::resizedPixelSelection(initial, SelectionEdge::Right, {0.34, 10}, 1.5,
                                                  {200, 200}),
                 QRect(20, 30, 41, 50));
        // 从同一个按下快照回拖时可恢复尺寸，最小尺寸不会累积改变对侧边缘。
        QCOMPARE(waibusnap::resizedPixelSelection(initial, SelectionEdge::Left | SelectionEdge::Top,
                                                  {}, 2, {200, 200}),
                 initial);
        QCOMPARE(waibusnap::resizedPixelSelection({5, 5, 1, 1},
                                                  SelectionEdge::Left | SelectionEdge::Top,
                                                  {-0.25, -0.25}, 2, {200, 200}),
                 QRect(4, 4, 2, 2));
        QVERIFY(waibusnap::resizedPixelSelection({}, SelectionEdge::Right, {}, 2, {200, 200})
                    .isEmpty());
        QVERIFY(waibusnap::resizedPixelSelection(initial, SelectionEdge::Right, {}, 0, {200, 200})
                    .isEmpty());
    }
    void eachEdgeUsesNearestPhysicalRounding_data()
    {
        QTest::addColumn<int>("edges");
        QTest::addColumn<QRect>("expected");
        QTest::newRow("left") << 1 << QRect(25, 30, 35, 50);
        QTest::newRow("top") << 2 << QRect(20, 24, 40, 56);
        QTest::newRow("right") << 4 << QRect(20, 30, 45, 50);
        QTest::newRow("bottom") << 8 << QRect(20, 30, 40, 44);
        QTest::newRow("top-left") << 3 << QRect(25, 24, 35, 56);
        QTest::newRow("top-right") << 6 << QRect(20, 24, 45, 56);
        QTest::newRow("bottom-left") << 9 << QRect(25, 30, 35, 44);
        QTest::newRow("bottom-right") << 12 << QRect(20, 30, 45, 44);
    }
    void eachEdgeUsesNearestPhysicalRounding()
    {
        QFETCH(int, edges);
        QFETCH(QRect, expected);
        QCOMPARE(waibusnap::resizedPixelSelection({20, 30, 40, 50},
                                                  waibusnap::SelectionEdges::fromInt(edges),
                                                  {3, -4}, 1.5, {200, 200}),
                 expected);
    }
};
QTEST_APPLESS_MAIN(SelectionGeometryTest)
#include "selection_geometry_test.moc"
