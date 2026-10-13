#include "core/scroll_stitcher.h"
#include <QColorSpace>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
namespace
{
QImage document(int width = 320, int height = 1600)
{
    QImage image(width, height, QImage::Format_RGB32);
    quint32 random = 42;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            random = random * 1664525 + 1013904223;
            image.setPixel(x, y, qRgb(random >> 24, (random >> 16) & 255, (random >> 8) & 255));
        }
    return image;
}
QImage fixedBands(QImage frame, int top, int bottom)
{
    for (int y = 0; y < frame.height(); ++y)
        if (y < top || y >= frame.height() - bottom)
            for (int x = 0; x < frame.width(); ++x)
                frame.setPixel(x, y, qRgb(x % 255, y < top ? 120 : 200, 90));
    return frame;
}
}
class ScrollStitcherTest : public QObject
{
    Q_OBJECT
  private slots:
    void verticalAndBidirectional()
    {
        const QImage source = document();
        ScrollStitcher stitcher;
        for (int position : {240, 300, 360, 300, 240, 180, 120})
            QCOMPARE(stitcher.addFrame(source.copy(0, position, 320, 400)),
                     ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.result(), source.copy(0, 120, 320, 640));
        QCOMPARE(stitcher.stats().frames, 7);
        QCOMPARE(stitcher.stats().accepted, 7);
        QCOMPARE(stitcher.stats().lengthPx, 640);
        QCOMPARE(stitcher.stats().axis, ScrollAxis::Vertical);
        QVERIFY(stitcher.stats().motionDetected);
    }
    void horizontalAndBidirectional()
    {
        const QImage source = document(1800, 300);
        ScrollStitcher stitcher;
        for (int position : {240, 303, 366, 303, 240, 177})
            QCOMPARE(stitcher.addFrame(source.copy(position, 0, 480, 300)),
                     ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.stats().axis, ScrollAxis::Horizontal);
        QCOMPARE(stitcher.result(), source.copy(177, 0, 669, 300));
    }
    void stationaryBandsRemoveFirstFrame()
    {
        const QImage source = document();
        ScrollStitcher stitcher;
        for (int position : {0, 60, 120})
            QCOMPARE(stitcher.addFrame(fixedBands(source.copy(0, position, 320, 400), 28, 24)),
                     ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.result(), source.copy(0, 28, 320, 468));
    }
    void lowConfidenceAndGapKeepAcceptedReference()
    {
        const QImage source = document();
        ScrollStitcher stitcher;
        stitcher.addFrame(source.copy(0, 0, 320, 400));
        const QImage initial = stitcher.result();
        QImage blank(320, 400, QImage::Format_RGB32);
        blank.fill(Qt::white);
        QCOMPARE(stitcher.addFrame(blank), ScrollFrameOutcome::Skipped);
        QCOMPARE(stitcher.addFrame(source.copy(0, 500, 320, 400)), ScrollFrameOutcome::Skipped);
        QCOMPARE(stitcher.result(), initial);
        QCOMPARE(stitcher.stats().warnings, 1);
        QCOMPARE(stitcher.addFrame(source.copy(0, 60, 320, 400)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.result(), source.copy(0, 0, 320, 460));
        QCOMPARE(stitcher.stats().skipped, 2);
    }
    void staticAndAxisLock()
    {
        const QImage source = document(1200, 1600);
        ScrollStitcher stitcher;
        stitcher.addFrame(source.copy(0, 0, 320, 400));
        QCOMPARE(stitcher.addFrame(source.copy(0, 0, 320, 400)), ScrollFrameOutcome::Skipped);
        QCOMPARE(stitcher.stats().warnings, 0);
        QCOMPARE(stitcher.addFrame(source.copy(0, 60, 320, 400)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.addFrame(source.copy(60, 60, 320, 400)), ScrollFrameOutcome::AxisLocked);
        QCOMPARE(stitcher.stats().axis, ScrollAxis::Vertical);
        QCOMPARE(stitcher.stats().warnings, 1);
    }
    void limits_data()
    {
        QTest::addColumn<qint64>("lengthLimit");
        QTest::addColumn<qint64>("areaLimit");
        QTest::newRow("length") << qint64(450) << qint64(30000000);
        QTest::newRow("area") << qint64(20000) << qint64(320 * 450);
    }
    void limits()
    {
        QFETCH(qint64, lengthLimit);
        QFETCH(qint64, areaLimit);
        const QImage source = document();
        ScrollStitcher stitcher({lengthLimit, areaLimit});
        stitcher.addFrame(source.copy(0, 0, 320, 400));
        QCOMPARE(stitcher.addFrame(source.copy(0, 60, 320, 400)), ScrollFrameOutcome::LimitReached);
        QCOMPARE(stitcher.result(), source.copy(0, 0, 320, 450));
        const int count = stitcher.stats().frames;
        QCOMPARE(stitcher.addFrame(source.copy(0, 120, 320, 400)),
                 ScrollFrameOutcome::LimitReached);
        QCOMPARE(stitcher.stats().frames, count);
        QVERIFY(stitcher.stats().limitReached);
    }
    void periodicRowsAndDiagonalAreRejected()
    {
        QImage periodic(320, 1200, QImage::Format_RGB32);
        for (int row = 0; row < periodic.height(); ++row)
            for (int column = 0; column < periodic.width(); ++column)
                periodic.setPixel(column, row, qRgb(row % 24 * 10, column % 16 * 10, row % 24 * 8));
        ScrollStitcher ambiguous;
        ambiguous.addFrame(periodic.copy(0, 0, 320, 400));
        const QImage before = ambiguous.result();
        QCOMPARE(ambiguous.addFrame(periodic.copy(0, 12, 320, 400)), ScrollFrameOutcome::Skipped);
        QCOMPARE(ambiguous.result(), before);
        QVERIFY(ambiguous.stats().warnings);
        const QImage source = document(1200, 1600);
        ScrollStitcher diagonal;
        diagonal.addFrame(source.copy(0, 0, 320, 400));
        QCOMPARE(diagonal.addFrame(source.copy(60, 60, 320, 400)), ScrollFrameOutcome::Skipped);
        QVERIFY(!diagonal.stats().motionDetected);
    }
    void horizontalStaticBandsAndAreaLimit()
    {
        const QImage vertical = document();
        QTransform rotate;
        rotate.rotate(90);
        ScrollStitcher stitcher({20000, 320 * 430});
        for (int index = 0; index < 2; ++index)
        {
            const QImage frame =
                fixedBands(vertical.copy(0, index * 60, 320, 400), 28, 24).transformed(rotate);
            QCOMPARE(stitcher.addFrame(frame), ScrollFrameOutcome::Accepted);
        }
        QCOMPARE(stitcher.stats().lengthPx, 408);
        QCOMPARE(stitcher.stats().axis, ScrollAxis::Horizontal);
        const QImage third =
            fixedBands(vertical.copy(0, 120, 320, 400), 28, 24).transformed(rotate);
        QCOMPARE(stitcher.addFrame(third), ScrollFrameOutcome::LimitReached);
        QCOMPARE(stitcher.result().size(), QSize(430, 320));
        QCOMPARE(stitcher.result(), vertical.copy(0, 28, 320, 430).transformed(rotate));
    }
    void finalizedPixelsOutliveStitcher()
    {
        QImage source = document();
        source.setColorSpace(QColorSpace::SRgb);
        QImage result;
        {
            ScrollStitcher stitcher;
            stitcher.addFrame(source.copy(0, 0, 320, 400));
            stitcher.addFrame(source.copy(0, 60, 320, 400));
            result = stitcher.takeResult();
            QCOMPARE(result.size(), QSize(320, 460));
        }
        QCOMPARE(result, source.copy(0, 0, 320, 460));
        QCOMPARE(result.devicePixelRatio(), qreal(1));
        QCOMPARE(result.colorSpace(), QColorSpace(QColorSpace::SRgb));
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("长图.png"));
        QVERIFY(result.save(path));
        QCOMPARE(QImage(path), result);
    }
    void matchingTiming()
    {
        const QImage source = document(2560, 1800);
        ScrollStitcher stitcher;
        const QImage first = source.copy(0, 0, 2560, 1440);
        const QImage next = source.copy(0, 60, 2560, 1440);
        stitcher.addFrame(first);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(stitcher.addFrame(next), ScrollFrameOutcome::Accepted);
        qInfo() << "合成 2560×1440 首个位移帧处理耗时 ms：" << timer.nsecsElapsed() / 1e6;
    }
    void invalidAndSmallFrames()
    {
        ScrollStitcher stitcher;
        QCOMPARE(stitcher.addFrame({}), ScrollFrameOutcome::Skipped);
        const QImage source = document(64, 320);
        QCOMPARE(stitcher.addFrame(source.copy(0, 0, 64, 64)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.addFrame(source.copy(0, 12, 64, 64)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.result(), source.copy(0, 0, 64, 76));
        QCOMPARE(stitcher.addFrame(source.copy(0, 0, 64, 80)), ScrollFrameOutcome::Skipped);
    }
};
QTEST_GUILESS_MAIN(ScrollStitcherTest)
#include "scroll_stitcher_test.moc"
