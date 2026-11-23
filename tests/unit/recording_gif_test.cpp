#include "core/gif_encoder.h"
#include <QFile>
#include <QImageReader>
#include <QMovie>
#include <QRandomGenerator>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
using namespace waibusnap;
class RecordingGifTest : public QObject
{
    Q_OBJECT
  private slots:
    void framesAndTiming_data()
    {
        QTest::addColumn<int>("fps");
        for (int fps : {10, 12, 15})
            QTest::newRow(qPrintable(QString::number(fps))) << fps;
    }
    void framesAndTiming()
    {
        QFETCH(int, fps);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("colors.gif"));
        GifEncoder encoder;
        QString error;
        QVERIFY2(encoder.start(path, QSize(31, 17), fps, &error), qPrintable(error));
        const int count = fps == 10 ? 8 : 16;
        for (int i = 0; i < count; ++i)
        {
            QImage frame(31, 17, QImage::Format_RGB32);
            frame.fill(i % 2 ? Qt::green : Qt::red);
            QVERIFY2(encoder.addFrame(frame, &error), qPrintable(error));
        }
        QCOMPARE(encoder.frameCount(), count);
        QVERIFY2(encoder.finish(&error), qPrintable(error));
        QImageReader reader(path);
        QCOMPARE(reader.imageCount(), count);
        QCOMPARE(reader.loopCount(), -1);
        int delay = 0;
        for (int i = 0; i < count; ++i)
        {
            const QImage frame = reader.read();
            QVERIFY(!frame.isNull());
            QCOMPARE(frame.size(), QSize(31, 17));
            QCOMPARE(frame.pixelColor(10, 10), QColor(i % 2 ? Qt::green : Qt::red));
            QVERIFY(qAbs(reader.nextImageDelay() - 1000.0 / fps) <= 10);
            delay += reader.nextImageDelay();
        }
        QVERIFY(qAbs(delay - count * 1000.0 / fps) <= 5);
        QMovie movie(path);
        QVERIFY(movie.jumpToFrame(count - 1));
        QCOMPARE(movie.frameCount(), count);
        QCOMPARE(movie.loopCount(), -1);
    }
    void lzwWidthsAndResets()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("dictionary.gif"));
        GifEncoder encoder;
        QString error;
        QVERIFY(encoder.start(path, QSize(320, 180), 12, &error));
        QRandomGenerator rng(32);
        QImage frame(320, 180, QImage::Format_RGB32);
        for (int y = 0; y < frame.height(); ++y)
            for (int x = 0; x < frame.width(); ++x)
            {
                const int v = int(rng.bounded(256));
                frame.setPixel(x, y, qRgb((v & 7) * 32, ((v >> 3) & 7) * 32, (v >> 6) * 64));
            }
        for (int i = 0; i < 3; ++i)
            QVERIFY(encoder.addFrame(frame, &error));
        QVERIFY(encoder.finish(&error));
        QImageReader reader(path);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(reader.read().convertToFormat(QImage::Format_RGB32), frame);
    }
    void moviePlaysAndLoops()
    {
        QTemporaryDir dir;
        QString error;
        const auto path = dir.filePath(QStringLiteral("loop.gif"));
        GifEncoder encoder;
        QVERIFY(encoder.start(path, QSize(8, 8), 10, &error));
        QImage frame(8, 8, QImage::Format_RGB32);
        frame.fill(Qt::red);
        QVERIFY(encoder.addFrame(frame, &error));
        frame.fill(Qt::blue);
        QVERIFY(encoder.addFrame(frame, &error));
        QVERIFY(encoder.finish(&error));
        QMovie movie(path);
        QSignalSpy changed(&movie, &QMovie::frameChanged);
        movie.start();
        QTRY_VERIFY_WITH_TIMEOUT(changed.size() >= 3, 1500);
        QCOMPARE(changed[0][0].toInt(), 0);
        QCOMPARE(changed[1][0].toInt(), 1);
        QCOMPARE(changed[2][0].toInt(), 0);
        movie.stop();
    }
    void gradientKeepsColorRange()
    {
        QTemporaryDir dir;
        QString error;
        const auto path = dir.filePath(QStringLiteral("gradient.gif"));
        QImage source(256, 256, QImage::Format_RGB32);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                source.setPixel(x, y, qRgb(x, y, (x + y) / 2));
        GifEncoder encoder;
        QVERIFY(encoder.start(path, source.size(), 12, &error));
        QVERIFY(encoder.addFrame(source, &error));
        QVERIFY(encoder.finish(&error));
        const auto decoded = QImageReader(path).read();
        QVERIFY(!decoded.isNull());
        qint64 errorR = 0, errorG = 0, errorB = 0;
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
            {
                const auto p = decoded.pixel(x, y);
                errorR += qAbs(qRed(p) - x);
                errorG += qAbs(qGreen(p) - y);
                errorB += qAbs(qBlue(p) - (x + y) / 2);
            }
        QVERIFY(errorR / 65536.0 < 35);
        QVERIFY(errorG / 65536.0 < 35);
        QVERIFY(errorB / 65536.0 < 35);
    }
    void smallSaturatedColorSurvives()
    {
        // BUG-0008 回归：小面积高饱和色（红色飞机 / 小图标）曾被 popularity 选色整块替换成
        // 海水蓝或树丛绿。带噪声的平滑渐变背景上，1x1 到 8x8 的红块都必须保留红色。
        QTemporaryDir dir;
        QRandomGenerator rng(5);
        for (int side : {1, 2, 3, 4, 6, 8})
        {
            QImage source(320, 180, QImage::Format_RGB32);
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x)
                {
                    const int noise = int(rng.bounded(9)) - 4;
                    source.setPixel(x, y,
                                    qRgb(qBound(0, 30 + x * 70 / 320 + noise, 255),
                                         qBound(0, 100 + y * 90 / 180 + noise, 255),
                                         qBound(0, 165 + (x + y) * 70 / 500 + noise, 255)));
                }
            const QRect patch(150, 85, side, side);
            for (int y = patch.top(); y <= patch.bottom(); ++y)
                for (int x = patch.left(); x <= patch.right(); ++x)
                    source.setPixel(x, y, qRgb(225, 45, 30));
            const QString path = dir.filePath(QStringLiteral("small-%1.gif").arg(side));
            GifEncoder encoder;
            QString error;
            QVERIFY2(encoder.start(path, source.size(), 12, &error), qPrintable(error));
            QVERIFY2(encoder.addFrame(source, &error), qPrintable(error));
            QVERIFY2(encoder.finish(&error), qPrintable(error));
            const QImage decoded = QImageReader(path).read();
            QVERIFY(!decoded.isNull());
            int kept = 0;
            for (int y = patch.top(); y <= patch.bottom(); ++y)
                for (int x = patch.left(); x <= patch.right(); ++x)
                {
                    const QColor c = decoded.pixelColor(x, y);
                    kept += c.red() > c.green() + 60 && c.red() > c.blue() + 60;
                }
            QVERIFY2(kept >= side * side * 9 / 10,
                     qPrintable(QStringLiteral("%1x%1 红块仅保留 %2 / %3 像素")
                                    .arg(side)
                                    .arg(kept)
                                    .arg(side * side)));
        }
    }
    void paletteSpreadsAcrossColorRange()
    {
        // 同一问题的另一面：popularity 选色把 256 个槽位重复分配给大面积近似色，
        // 于是画面里一批小面积高饱和色（小图标 / 图例 / 彩色标记）全部失真。
        QTemporaryDir dir;
        QImage source(320, 180, QImage::Format_RGB32);
        QRandomGenerator rng(7);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x)
            {
                const int noise = int(rng.bounded(9)) - 4;
                source.setPixel(x, y,
                                qRgb(qBound(0, 30 + x * 70 / 320 + noise, 255),
                                     qBound(0, 100 + y * 90 / 180 + noise, 255),
                                     qBound(0, 165 + (x + y) * 70 / 500 + noise, 255)));
            }
        int index = 0;
        for (int y = 10; y + 8 <= 172; y += 20)
            for (int x = 10; x + 8 <= 312; x += 20, ++index)
            {
                const QColor color = QColor::fromHsv((index * 23) % 360, 235, 235);
                for (int dy = 0; dy < 8; ++dy)
                    for (int dx = 0; dx < 8; ++dx)
                        source.setPixel(x + dx, y + dy, color.rgb());
            }
        const QString path = dir.filePath(QStringLiteral("spread.gif"));
        GifEncoder encoder;
        QString error;
        QVERIFY2(encoder.start(path, source.size(), 12, &error), qPrintable(error));
        QVERIFY2(encoder.addFrame(source, &error), qPrintable(error));
        QVERIFY2(encoder.finish(&error), qPrintable(error));
        QImageReader reader(path);
        QCOMPARE(reader.imageCount(), 1);
        const QImage decoded = reader.read();
        QVERIFY(!decoded.isNull());
        int total = 0, faithful = 0;
        for (int y = 10; y + 8 <= 172; y += 20)
            for (int x = 10; x + 8 <= 312; x += 20)
            {
                const QColor expected = source.pixelColor(x + 4, y + 4);
                const QColor actual = decoded.pixelColor(x + 4, y + 4);
                const int delta = qMax(qAbs(expected.red() - actual.red()),
                                       qMax(qAbs(expected.green() - actual.green()),
                                            qAbs(expected.blue() - actual.blue())));
                ++total;
                faithful += delta <= 24;
            }
        QVERIFY2(faithful * 10 >= total * 9,
                 qPrintable(QStringLiteral("彩色小块仅 %1 / %2 保真").arg(faithful).arg(total)));
    }
    void lifecycleAndErrors()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cancel.gif.part"));
        QString error;
        GifEncoder encoder;
        QVERIFY(!encoder.start(QStringLiteral("relative.gif"), QSize(2, 2), 12, &error));
        QVERIFY(!encoder.start(path, QSize(0, 2), 12, &error));
        QVERIFY(!encoder.start(path, QSize(65536, 2), 12, &error));
        QVERIFY(!encoder.start(path, QSize(2, 2), 30, &error));
        QVERIFY(
            !encoder.start(dir.filePath(QStringLiteral("missing/a.gif")), QSize(2, 2), 12, &error));
        QVERIFY(encoder.start(path, QSize(2, 2), 12, &error));
        QVERIFY(!encoder.finish(&error));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(encoder.start(path, QSize(2, 2), 12, &error));
        QVERIFY(!encoder.addFrame(QImage(), &error));
        QImage frame(2, 2, QImage::Format_RGB32);
        frame.fill(Qt::blue);
        QVERIFY(encoder.addFrame(frame, &error));
        encoder.cancel();
        encoder.cancel();
        QVERIFY(!QFileInfo::exists(path));
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("用户文件");
        existing.close();
        QVERIFY(!encoder.start(path, QSize(2, 2), 12, &error));
        encoder.cancel();
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("用户文件"));
    }
};
QTEST_MAIN(RecordingGifTest)
#include "recording_gif_test.moc"
