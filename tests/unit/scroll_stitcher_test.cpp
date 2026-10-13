#include "core/scroll_stitcher.h"
#include <QColorSpace>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstring>
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
QImage textDocument(int lineHeight)
{
    QImage image(800, 8200, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(14);
    painter.setFont(font);
    quint32 random = 73;
    for (int row = 0, index = 0; row < image.height(); row += lineHeight, ++index)
    {
        QString text = QString::number(index) + QStringLiteral("  ");
        for (int word = 0; word < 10; ++word)
        {
            random = random * 1664525 + 1013904223;
            text += QString::number(random, 36) + QLatin1Char(' ');
        }
        painter.setPen(QColor(45, 45, 45));
        painter.drawText(12, row + 14, text);
        painter.setPen(QColor(220, 220, 220));
        painter.drawLine(0, row + lineHeight - 1, image.width() - 1, row + lineHeight - 1);
    }
    return image;
}
// 独立按相邻源行线性插值，模拟浏览器平滑滚动的亚像素重采样。
QImage smoothFrame(const QImage& source, double position, int height = 640)
{
    const int start = int(std::floor(position));
    const double fraction = position - start;
    QImage frame = source.copy(0, start, source.width(), height);
    for (int row = 0; row < height; ++row)
    {
        auto* pixels = reinterpret_cast<QRgb*>(frame.scanLine(row));
        const auto* next = reinterpret_cast<const QRgb*>(source.constScanLine(start + row + 1));
        for (int column = 0; column < frame.width(); ++column)
        {
            const QRgb first = pixels[column], second = next[column];
            const auto blend = [&](int a, int b) { return qRound(a + (b - a) * fraction); };
            pixels[column] =
                qRgb(blend(qRed(first), qRed(second)), blend(qGreen(first), qGreen(second)),
                     blend(qBlue(first), qBlue(second)));
        }
    }
    return frame;
}
}
class ScrollStitcherTest : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        // Windows offscreen 的字体库可能为空；只读加载系统 Arial，禁止用空白页冒充文本。
        if (!QFontDatabase::families().contains(QStringLiteral("Arial")))
            for (const QString& directory :
                 QStandardPaths::standardLocations(QStandardPaths::FontsLocation))
            {
                QDirIterator fonts(directory, {QStringLiteral("*.ttf")}, QDir::Files,
                                   QDirIterator::Subdirectories);
                while (fonts.hasNext())
                {
                    const QString path = fonts.next();
                    if (fonts.fileName().compare(QStringLiteral("arial.ttf"),
                                                 Qt::CaseInsensitive) == 0)
                    {
                        QVERIFY(QFontDatabase::addApplicationFont(path) >= 0);
                        break;
                    }
                }
            }
        QVERIFY(QFontDatabase::families().contains(QStringLiteral("Arial")));
    }
    void lightTextAndNoise_data()
    {
        QTest::addColumn<int>("lineHeight");
        QTest::addColumn<double>("speed");
        for (int lineHeight : {0, 20, 22})
            for (double speed : {1.0, 2.0, 3.0, 5.0, 12.0, 30.0, 2.3})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(lineHeight).arg(speed)))
                    << lineHeight << speed;
    }
    void lightTextAndNoise()
    {
        QFETCH(int, lineHeight);
        QFETCH(double, speed);
        const QImage source = lineHeight ? textDocument(lineHeight) : document(800, 8200);
        ScrollStitcher stitcher;
        int unreliable = 0;
        // 22px 对照从笔画中开始，避免字体度量差异把空白页边识别成固定端带。
        const int origin = lineHeight == 22 ? 6 : 0;
        for (int index = 0; index < 240; ++index)
        {
            stitcher.addFrame(source.copy(0, origin + qRound(index * speed), 800, 640));
            unreliable += !stitcher.stats().lastFrameReliable;
        }
        const int expectedLength = 640 + qRound(239 * speed);
        qInfo() << "连续滚动：行距 / 速度 / 接受 / 不可靠 / 长度 / 真值" << lineHeight << speed
                << stitcher.stats().accepted << unreliable << stitcher.stats().lengthPx
                << expectedLength;
        QCOMPARE(unreliable, 0);
        QCOMPARE(stitcher.stats().warnings, 0);
        QVERIFY(stitcher.stats().motionDetected);
        QVERIFY(std::abs(stitcher.stats().lengthPx - expectedLength) <= expectedLength * 0.005);
        // 逐像素核对全部内容，比只数行标更严格：任意重复 / 缺失带都会失败。
        const QImage result = stitcher.result();
        const int trimmed = expectedLength - result.height();
        bool continuous = false;
        for (int leading = 0; leading <= trimmed; ++leading)
            continuous |= result == source.copy(0, origin + leading, 800, result.height());
        QVERIFY(continuous);
        if (!lineHeight)
        {
            QCOMPARE(stitcher.stats().accepted, 240);
            QCOMPARE(result, source.copy(0, 0, 800, expectedLength));
        }
    }
    void smoothTextAndNoise_data()
    {
        QTest::addColumn<int>("lineHeight");
        QTest::addColumn<double>("speed");
        for (int lineHeight : {0, 20, 22})
            for (double speed : {0.2, 0.45, 2.3, 5.7, 12.4, 30.3})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(lineHeight).arg(speed)))
                    << lineHeight << speed;
    }
    void smoothTextAndNoise()
    {
        QFETCH(int, lineHeight);
        QFETCH(double, speed);
        const QImage source = lineHeight ? textDocument(lineHeight) : document(800, 8200);
        ScrollStitcher stitcher;
        int unreliable = 0;
        for (int index = 0; index < 240; ++index)
        {
            stitcher.addFrame(smoothFrame(source, 6 + index * speed));
            unreliable += !stitcher.stats().lastFrameReliable;
        }
        const int expectedLength = 640 + qRound(239 * speed);
        qInfo() << "平滑滚动：行距 / 速度 / 接受 / 不可靠 / 长度 / 真值" << lineHeight << speed
                << stitcher.stats().accepted << unreliable << stitcher.stats().lengthPx
                << expectedLength;
        QCOMPARE(unreliable, 0);
        QCOMPARE(stitcher.stats().warnings, 0);
        QVERIFY(stitcher.stats().motionDetected);
        QVERIFY(std::abs(stitcher.stats().lengthPx - expectedLength) <= expectedLength * 0.005);
        // 按原始页面的行坐标核对每一行；只容许亚像素插值造成的相邻源行混合。
        const QImage result = stitcher.result();
        QCOMPARE(result.copy(0, 0, 800, 16), source.copy(0, 6, 800, 16));
        double maximumRowError = 0;
        for (int row = 0; row < result.height(); ++row)
        {
            const auto* actual = reinterpret_cast<const QRgb*>(result.constScanLine(row));
            double best = 255;
            for (int offset : {-1, 0})
            {
                const auto* first =
                    reinterpret_cast<const QRgb*>(source.constScanLine(6 + row + offset));
                const auto* second =
                    reinterpret_cast<const QRgb*>(source.constScanLine(7 + row + offset));
                double numerator = 0, denominator = 0;
                for (int column = 0; column < 800; ++column)
                {
                    const int delta = qGray(second[column]) - qGray(first[column]);
                    numerator += (qGray(actual[column]) - qGray(first[column])) * delta;
                    denominator += delta * delta;
                }
                const double fraction =
                    denominator ? std::clamp(numerator / denominator, 0.0, 1.0) : 0.0;
                double error = 0;
                for (int column = 0; column < 800; ++column)
                {
                    const double expected =
                        qGray(first[column]) +
                        (qGray(second[column]) - qGray(first[column])) * fraction;
                    error += std::abs(qGray(actual[column]) - expected);
                }
                best = std::min(best, error / 800);
            }
            maximumRowError = std::max(maximumRowError, best);
        }
        qInfo() << "平滑滚动最大单行误差：" << maximumRowError;
        QVERIFY(maximumRowError <= 1.5);
    }
    void smoothReturnToReference()
    {
        const QImage source = document(800, 1200);
        ScrollStitcher stitcher;
        stitcher.addFrame(smoothFrame(source, 100));
        QCOMPARE(stitcher.addFrame(smoothFrame(source, 106.9)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.stats().lastMotionPx, 7);
        QCOMPARE(stitcher.addFrame(smoothFrame(source, 100)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.stats().lastMotionPx, -7);
        QCOMPARE(stitcher.addFrame(smoothFrame(source, 98)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.stats().lastMotionPx, -2);
        QCOMPARE(stitcher.stats().warnings, 0);
        QCOMPARE(stitcher.stats().lengthPx, 649);
    }
    void lightTextDiagonalAndPeriodicAreRejected_data()
    {
        QTest::addColumn<int>("shift");
        for (int shift : {0, 1, 2, 5, 12, 30})
            QTest::newRow(qPrintable(QString::number(shift))) << shift;
    }
    void lightTextDiagonalAndPeriodicAreRejected()
    {
        QFETCH(int, shift);
        const QImage source = textDocument(20);
        const QImage first = source.copy(0, 6, 760, 640);
        if (shift)
        {
            ScrollStitcher stitcher;
            stitcher.addFrame(first);
            QCOMPARE(stitcher.addFrame(source.copy(shift, 6 + shift, 760, 640)),
                     ScrollFrameOutcome::Skipped);
            QCOMPARE(stitcher.result(), first);
            QVERIFY(!stitcher.stats().motionDetected);
            return;
        }
        QImage periodic(source.size(), source.format());
        for (int row = 0; row < periodic.height(); ++row)
            std::memcpy(periodic.scanLine(row), source.constScanLine(row % 20),
                        size_t(periodic.bytesPerLine()));
        ScrollStitcher stitcher;
        stitcher.addFrame(smoothFrame(periodic, 0.2));
        const QImage original = stitcher.result();
        QCOMPARE(stitcher.addFrame(smoothFrame(periodic, 7.6)), ScrollFrameOutcome::Skipped);
        QCOMPARE(stitcher.result(), original);
        QVERIFY(!stitcher.stats().motionDetected);
    }
    void subpixelZeroKeepsReference()
    {
        const QImage source = document(800, 1200);
        ScrollStitcher stitcher;
        stitcher.addFrame(smoothFrame(source, 0));
        const QImage initial = stitcher.result();
        for (double position : {0.1, 0.2, 0.3, 0.4})
        {
            QCOMPARE(stitcher.addFrame(smoothFrame(source, position)), ScrollFrameOutcome::Skipped);
            QCOMPARE(stitcher.stats().lastMotionPx, 0);
            QVERIFY(stitcher.stats().lastFrameReliable);
            QVERIFY(!stitcher.stats().motionDetected);
            QCOMPARE(stitcher.result(), initial);
            QCOMPARE(stitcher.stats().warnings, 0);
        }
        QCOMPARE(stitcher.addFrame(smoothFrame(source, 1)), ScrollFrameOutcome::Accepted);
        QCOMPARE(stitcher.result(), source.copy(0, 0, 800, 641));
        QCOMPARE(stitcher.stats().accepted, 2);
    }
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
QTEST_MAIN(ScrollStitcherTest)
#include "scroll_stitcher_test.moc"
