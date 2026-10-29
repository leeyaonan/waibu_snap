#include "core/gif_encoder.h"
#include <QFile>
#include <QImageReader>
#include <QMovie>
#include <QRandomGenerator>
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
