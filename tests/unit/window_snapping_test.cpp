#include "core/window_snapping.h"
#include <QTest>
class WindowSnappingTest final : public QObject
{
    Q_OBJECT
  private slots:
    void clipsToDisplayWithoutReordering()
    {
        const QVector<QRect> windows = {
            {90, 40, 70, 80}, {500, 300, 40, 50}, {150, 80, 100, 60}, {200, 150, 90, 80}};
        const auto local = waibusnap::localWindowRects(windows, {100, 50, 180, 150});
        QCOMPARE(local, QVector<QRect>({{0, 0, 60, 70}, {50, 30, 100, 60}, {100, 100, 80, 50}}));
        QVERIFY(waibusnap::localWindowRects(windows, {}).isEmpty());
        QCOMPARE(waibusnap::localWindowRects({{-80, -40, 50, 30}}, {-100, -60, 100, 60}),
                 QVector<QRect>({{20, 20, 50, 30}}));
    }
    void frontmostHitAndHalfOpenGaps()
    {
        const QVector<QRect> windows = {{10, 10, 50, 40}, {30, 20, 100, 100}, {150, 0, 40, 40}};
        QCOMPARE(waibusnap::windowAt(windows, {40, 30}), 0);
        QCOMPARE(waibusnap::windowAt(windows, {60, 30}), 1);
        QCOMPARE(waibusnap::windowAt(windows, {59.9, 49.9}), 0);
        QCOMPARE(waibusnap::windowAt(windows, {10, 10}), 0);
        QCOMPARE(waibusnap::windowAt(windows, {130, 30}), -1);
        QCOMPARE(waibusnap::windowAt(windows, {140, 30}), -1);
        QCOMPARE(waibusnap::windowAt(windows, {190, 30}), -1);
        QCOMPARE(waibusnap::windowAt(windows, {0, 0}), -1);
        QCOMPARE(waibusnap::windowAt({}, {10, 10}), -1);
    }
    void pixelEdges_data()
    {
        QTest::addColumn<QRect>("logical");
        QTest::addColumn<qreal>("scale");
        QTest::addColumn<QRect>("pixels");
        QTest::newRow("retina") << QRect(3, 5, 7, 9) << qreal(2) << QRect(6, 10, 14, 18);
        QTest::newRow("half-up") << QRect(1, 3, 1, 1) << qreal(1.5) << QRect(2, 5, 1, 1);
        QTest::newRow("half-open-width") << QRect(2, 4, 1, 1) << qreal(1.5) << QRect(3, 6, 2, 2);
        QTest::newRow("clip") << QRect(-2, -3, 20, 15) << qreal(2) << QRect(0, 0, 30, 24);
        QTest::newRow("outside") << QRect(30, 30, 10, 10) << qreal(2) << QRect(30, 30, 0, 0);
        QTest::newRow("empty") << QRect() << qreal(2) << QRect();
        QTest::newRow("invalid-scale") << QRect(1, 1, 4, 4) << qreal(0) << QRect();
    }
    void pixelEdges()
    {
        QFETCH(QRect, logical);
        QFETCH(qreal, scale);
        QFETCH(QRect, pixels);
        QCOMPARE(waibusnap::windowPixelSelection(logical, scale, {30, 30}), pixels);
        QVERIFY(waibusnap::windowPixelSelection(logical, scale, {}).isEmpty());
    }
};
QTEST_APPLESS_MAIN(WindowSnappingTest)
#include "window_snapping_test.moc"
