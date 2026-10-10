#include "core/sticker_geometry.h"
#include <QTest>
#include <limits>
using namespace waibusnap;
class StickerGeometryTest final : public QObject
{
    Q_OBJECT
  private slots:
    void clipboardCascade_data()
    {
        QTest::addColumn<QRect>("available");
        QTest::addColumn<QSize>("windowSize");
        QTest::addColumn<quint64>("sequence");
        QTest::addColumn<QPoint>("expected");
        const QRect screen(0, 30, 1000, 800);
        QTest::newRow("center") << screen << QSize(200, 100) << quint64(0) << QPoint(400, 380);
        QTest::newRow("second") << screen << QSize(200, 100) << quint64(1) << QPoint(424, 404);
        QTest::newRow("third") << screen << QSize(200, 100) << quint64(2) << QPoint(448, 428);
        QTest::newRow("last-fitting")
            << screen << QSize(200, 100) << quint64(14) << QPoint(736, 716);
        QTest::newRow("wrap-fitting")
            << screen << QSize(200, 100) << quint64(15) << QPoint(400, 380);
        QTest::newRow("repeat-wrap")
            << screen << QSize(200, 100) << quint64(31) << QPoint(424, 404);
        QTest::newRow("negative-screen") << QRect(-1500, -900, 1000, 800) << QSize(200, 100)
                                         << quint64(1) << QPoint(-1076, -526);
        QTest::newRow("too-wide") << screen << QSize(1200, 100) << quint64(0) << QPoint(24, 54);
        QTest::newRow("too-tall") << screen << QSize(200, 900) << quint64(1) << QPoint(48, 78);
        QTest::newRow("huge") << screen << QSize(10000, 10000) << quint64(32) << QPoint(792, 822);
        QTest::newRow("wrap-huge")
            << screen << QSize(10000, 10000) << quint64(33) << QPoint(24, 54);
        QTest::newRow("exact-fit") << screen << screen.size() << quint64(1) << screen.topLeft();
        QTest::newRow("tiny-screen")
            << QRect(-10, -20, 12, 8) << QSize(100, 100) << quint64(1) << QPoint(1, -13);
        QTest::newRow("large-sequence")
            << screen << QSize(200, 100) << std::numeric_limits<quint64>::max() << QPoint(400, 380);
        QTest::newRow("empty-screen") << QRect() << QSize(100, 100) << quint64(1) << QPoint();
        QTest::newRow("empty-image") << screen << QSize() << quint64(1) << screen.topLeft();
    }
    void clipboardCascade()
    {
        QFETCH(QRect, available);
        QFETCH(QSize, windowSize);
        QFETCH(quint64, sequence);
        QFETCH(QPoint, expected);
        const QPoint result = clipboardStickerPosition(available, windowSize, sequence);
        QCOMPARE(result, expected);
        if (!available.isEmpty() && !windowSize.isEmpty())
        {
            QVERIFY(available.contains(result));
            if (windowSize.width() <= available.width() &&
                windowSize.height() <= available.height())
                QVERIFY(available.contains(QRect(result, windowSize)));
        }
    }
    void scaleStepsAndClamping()
    {
        qreal scale = 1;
        for (int index = 0; index < 20; ++index)
            scale = steppedStickerScale(scale, 1);
        QCOMPARE(scale, maximumStickerScale);
        QCOMPARE(steppedStickerScale(scale, 1), qreal(4));
        for (int index = 0; index < 20; ++index)
            scale = steppedStickerScale(scale, -1);
        QCOMPARE(scale, minimumStickerScale);
        QCOMPARE(steppedStickerScale(scale, -1), qreal(0.25));
        QCOMPARE(clampedStickerScale(1), qreal(1));
        QCOMPARE(clampedStickerScale(-1), qreal(0.25));
        QCOMPARE(clampedStickerScale(10), qreal(4));
        QCOMPARE(clampedStickerScale(std::numeric_limits<qreal>::quiet_NaN()), qreal(1));
        QCOMPARE(steppedStickerScale(1, std::numeric_limits<int>::max()), qreal(4));
        QCOMPARE(steppedStickerScale(1, std::numeric_limits<int>::min()), qreal(0.25));
        QCOMPARE(steppedStickerScale(1.25, -1), qreal(1));
    }
    void physicalPixelsToLogicalSize_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("one") << qreal(1);
        QTest::newRow("one-and-half") << qreal(1.5);
        QTest::newRow("retina") << qreal(2);
    }
    void physicalPixelsToLogicalSize()
    {
        QFETCH(qreal, dpr);
        for (qreal scale : {qreal(0.25), qreal(1), qreal(4)})
        {
            const QSizeF expected(1920 * scale / dpr, 1080 * scale / dpr);
            QCOMPARE(stickerLogicalSize({1920, 1080}, scale, dpr), expected);
            QCOMPARE(stickerWindowSize({1920, 1080}, scale, dpr), expected.toSize());
        }
    }
    void anchorPreservesImagePoint()
    {
        const QPointF before(-1500, -300);
        const QSizeF oldSize(960, 540);
        const QPointF anchor = before + QPointF(123, 234);
        const QSizeF newSize(480, 270);
        const auto after = anchoredStickerPosition(before, oldSize, newSize, anchor);
        QCOMPARE((anchor.x() - before.x()) / oldSize.width(),
                 (anchor.x() - after.x()) / newSize.width());
        QCOMPARE((anchor.y() - before.y()) / oldSize.height(),
                 (anchor.y() - after.y()) / newSize.height());
        const auto center = before + QPointF(480, 270);
        QCOMPARE(anchoredStickerPosition(before, oldSize, newSize, center) + QPointF(240, 135),
                 center);
    }
    void minimumAndInvalidInputs()
    {
        QCOMPARE(stickerWindowSize({1, 1}, 0.25, 2), QSize(1, 1));
        QCOMPARE(stickerWindowSize({5, 7}, 1, 1.5), QSize(4, 5));
        for (qreal dpr : {qreal(0), qreal(-1), std::numeric_limits<qreal>::infinity(),
                          std::numeric_limits<qreal>::quiet_NaN()})
            QVERIFY(stickerLogicalSize({100, 100}, 1, dpr).isEmpty());
        QVERIFY(stickerLogicalSize({}, 1, 1).isEmpty());
        QVERIFY(stickerLogicalSize({100, 100}, 0, 1).isEmpty());
        QVERIFY(
            stickerLogicalSize({100, 100}, std::numeric_limits<qreal>::quiet_NaN(), 1).isEmpty());
        QVERIFY(stickerWindowSize({100, 100}, 1, 1e-300).isEmpty());
        QCOMPARE(anchoredStickerPosition({10, 20}, {}, {100, 100}, {30, 40}), QPointF(10, 20));
    }
    void removedScreenRecovery()
    {
        const QRect primary(0, 30, 1920, 1050);
        const QRect secondary(-1280, -100, 1280, 800);
        const QRect window(-1200, 0, 960, 540);
        QCOMPARE(recoveredStickerPosition(window, {primary, secondary}, primary), window.topLeft());
        QCOMPARE(recoveredStickerPosition(window, {primary}, primary), QPoint(0, 30));
        QCOMPARE(recoveredStickerPosition({2200, 1200, 960, 540}, {primary}, primary),
                 QPoint(960, 540));
        QCOMPARE(recoveredStickerPosition({2200, 1200, 4000, 2000}, {primary}, primary),
                 primary.topLeft());
        QCOMPARE(recoveredStickerPosition(window, {}, {}), window.topLeft());
    }
};
QTEST_APPLESS_MAIN(StickerGeometryTest)
#include "sticker_geometry_test.moc"
