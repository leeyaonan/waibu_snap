#include "core/selection_assistance.h"
#include "core/selection_geometry.h"
#include "session/session_metrics.h"
#include <QTest>
class SelectionGeometryTest final : public QObject
{
    Q_OBJECT
  private slots:
    void nudgeMatrix_data()
    {
        QTest::addColumn<QRect>("initial");
        QTest::addColumn<int>("edge");
        QTest::addColumn<int>("mode");
        QTest::addColumn<QRect>("expected");
        const QRect initial(20, 30, 40, 50);
        const int edges[] = {1, 2, 4, 8};
        const QRect normal[3][4] = {
            {{19, 30, 40, 50}, {20, 29, 40, 50}, {21, 30, 40, 50}, {20, 31, 40, 50}},
            {{19, 30, 41, 50}, {20, 29, 40, 51}, {20, 30, 41, 50}, {20, 30, 40, 51}},
            {{21, 30, 39, 50}, {20, 31, 40, 49}, {20, 30, 39, 50}, {20, 30, 40, 49}}};
        for (int mode = 0; mode < 3; ++mode)
            for (int index = 0; index < 4; ++index)
                QTest::newRow(qPrintable(QStringLiteral("step-%1-%2").arg(mode).arg(index)))
                    << initial << edges[index] << mode << normal[mode][index];
        // 四边、四角、全屏和最小矩形，覆盖移动 / 扩张夹取及收缩不翻转。
        const QRect borders[] = {{0, 30, 40, 50},   {20, 0, 40, 50},    {160, 30, 40, 50},
                                 {20, 150, 40, 50}, {0, 0, 40, 50},     {160, 0, 40, 50},
                                 {0, 150, 40, 50},  {160, 150, 40, 50}, {0, 0, 200, 200}};
        for (int row = 0; row < 9; ++row)
            for (int index = 0; index < 4; ++index)
            {
                const QRect rect = borders[row];
                const bool clamped = index == 0   ? rect.left() == 0
                                     : index == 1 ? rect.top() == 0
                                     : index == 2 ? rect.right() == 199
                                                  : rect.bottom() == 199;
                if (clamped)
                    for (int mode = 0; mode < 2; ++mode)
                        QTest::newRow(qPrintable(
                            QStringLiteral("limit-%1-%2-%3").arg(row).arg(index).arg(mode)))
                            << rect << edges[index] << mode << rect;
            }
        for (int index = 0; index < 4; ++index)
            QTest::newRow(qPrintable(QStringLiteral("minimum-%1").arg(index)))
                << QRect(50, 60, 1, 1) << edges[index] << 2 << QRect(50, 60, 1, 1);
        QTest::newRow("empty") << QRect() << 1 << 0 << QRect();
        QTest::newRow("outside") << QRect(199, 0, 2, 1) << 4 << 1 << QRect();
    }
    void nudgeMatrix()
    {
        QFETCH(QRect, initial);
        QFETCH(int, edge);
        QFETCH(int, mode);
        QFETCH(QRect, expected);
        QCOMPARE(waibusnap::nudgedPixelSelection(
                     initial, static_cast<waibusnap::SelectionEdge>(edge),
                     static_cast<waibusnap::SelectionNudge>(mode), {200, 200}),
                 expected);
    }
    void magnifierSampling_data()
    {
        QTest::addColumn<QPoint>("anchor");
        QTest::addColumn<QRect>("expected");
        const QPoint anchors[] = {{0, 0},   {99, 0}, {0, 79},  {99, 79}, {50, 0},
                                  {50, 79}, {0, 40}, {99, 40}, {50, 40}};
        const QRect samples[] = {{0, 0, 15, 15},   {85, 0, 15, 15},  {0, 65, 15, 15},
                                 {85, 65, 15, 15}, {43, 0, 15, 15},  {43, 65, 15, 15},
                                 {0, 33, 15, 15},  {85, 33, 15, 15}, {43, 33, 15, 15}};
        for (int index = 0; index < 9; ++index)
            QTest::newRow(qPrintable(QString::number(index))) << anchors[index] << samples[index];
    }
    void magnifierSampling()
    {
        QFETCH(QPoint, anchor);
        QFETCH(QRect, expected);
        const QRect sample = waibusnap::magnifierSamplingRect(anchor, {100, 80});
        QCOMPARE(sample, expected);
        QVERIFY(sample.contains(anchor));
        QVERIFY(QRect(0, 0, 100, 80).contains(sample));
    }
    void magnifierPlacementFlips_data()
    {
        QTest::addColumn<QPointF>("anchor");
        QTest::addColumn<QRect>("expected");
        QTest::newRow("bottom-right") << QPointF(100, 100) << QRect(116, 118, 128, 152);
        QTest::newRow("bottom-left") << QPointF(790, 100) << QRect(646, 118, 128, 152);
        QTest::newRow("top-right") << QPointF(100, 590) << QRect(116, 420, 128, 152);
        QTest::newRow("top-left") << QPointF(790, 590) << QRect(646, 420, 128, 152);
    }
    void magnifierPlacementFlips()
    {
        QFETCH(QPointF, anchor);
        QFETCH(QRect, expected);
        const QRect panel = waibusnap::placedMagnifier(anchor, {128, 152}, {0, 0, 800, 600});
        QCOMPARE(panel, expected);
        QVERIFY(!panel.contains(anchor.toPoint()));
    }
    void assistanceSmallAndInvalidGeometry()
    {
        QCOMPARE(waibusnap::magnifierSamplingRect({2, 3}, {4, 5}), QRect(0, 0, 4, 5));
        QVERIFY(waibusnap::magnifierSamplingRect({100, 0}, {100, 80}).isEmpty());
        QVERIFY(waibusnap::magnifierSamplingRect({}, {}).isEmpty());
        QVERIFY(waibusnap::placedMagnifier({}, {}, {0, 0, 800, 600}).isEmpty());
        using namespace waibusnap;
        QCOMPARE(selectionNudgeAnchor({20, 30, 40, 50}, SelectionEdge::Right, SelectionNudge::Move),
                 QPoint(20, 30));
        QCOMPARE(
            selectionNudgeAnchor({20, 30, 40, 50}, SelectionEdge::Left, SelectionNudge::Shrink),
            QPoint(20, 54));
        QCOMPARE(
            selectionNudgeAnchor({20, 30, 40, 50}, SelectionEdge::Right, SelectionNudge::Expand),
            QPoint(59, 54));
        QCOMPARE(selectionNudgeAnchor({20, 30, 40, 50}, SelectionEdge::Top, SelectionNudge::Shrink),
                 QPoint(39, 30));
        QCOMPARE(
            selectionNudgeAnchor({20, 30, 40, 50}, SelectionEdge::Bottom, SelectionNudge::Expand),
            QPoint(39, 79));
    }
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
