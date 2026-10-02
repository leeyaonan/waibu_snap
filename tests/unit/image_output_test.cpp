#include "output/image_output.h"
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
class ImageOutputTest final : public QObject
{
    Q_OBJECT
  private slots:
    void cropKeepsPhysicalPixels()
    {
        QImage source(12, 10, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x * 10, y * 10, 50));
        source.setDevicePixelRatio(2);
        const QImage cropped = waibusnap::cropFrozenSelection(source, {2, 3, 5, 4});
        QCOMPARE(cropped.size(), QSize(5, 4));
        QCOMPARE(cropped.devicePixelRatio(), qreal(1));
        QCOMPARE(source.devicePixelRatio(), qreal(2));
        for (int y = 0; y < cropped.height(); ++y)
            for (int x = 0; x < cropped.width(); ++x)
                QCOMPARE(cropped.pixelColor(x, y), source.pixelColor(x + 2, y + 3));
        QVERIFY(waibusnap::cropFrozenSelection(source, {}).isNull());
        QVERIFY(waibusnap::cropFrozenSelection(source, {-1, 0, 5, 4}).isNull());
        QVERIFY(waibusnap::cropFrozenSelection(source, {10, 9, 5, 4}).isNull());
    }
    void pngRoundTripSupportsChineseAndSpaces()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath(QStringLiteral("中文 目录"));
        QVERIFY(QDir().mkpath(directory));
        const QString path = QDir(directory).filePath(QStringLiteral("截图 文件"));
        QImage image(37, 29, QImage::Format_ARGB32);
        image.fill(QColor(20, 60, 100, 180));
        image.setPixelColor(7, 5, Qt::green);
        image.setDevicePixelRatio(2);
        const auto result = waibusnap::exportPngToPath(image, path);
        QVERIFY2(result.success, qPrintable(result.explanation));
        QImageReader reader(path + QStringLiteral(".png"));
        QCOMPARE(reader.format(), QByteArray("png"));
        QCOMPARE(reader.size(), QSize(37, 29));
        const QImage loaded = reader.read();
        QCOMPARE(loaded.devicePixelRatio(), qreal(1));
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                QCOMPARE(loaded.pixelColor(x, y), image.pixelColor(x, y));
        QCOMPARE(image.devicePixelRatio(), qreal(2));
    }
    void failuresPreserveExistingFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::blue);
        const QString missing = temporary.filePath(QStringLiteral("不存在/图.png"));
        const auto absent = waibusnap::exportPngToPath(image, missing);
        QVERIFY(!absent.success);
        QVERIFY(!absent.explanation.isEmpty());
        QVERIFY(!QFileInfo::exists(missing));
        const QString path = temporary.filePath(QStringLiteral("只读.png"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("原文件内容");
        QCOMPARE(file.write(original), qint64(original.size()));
        file.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                                QFileDevice::ReadGroup | QFileDevice::ReadOther));
        const auto denied = waibusnap::exportPngToPath(image, path);
        const bool restored =
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ReadUser | QFileDevice::WriteUser);
        QVERIFY(restored);
        QVERIFY(!denied.success);
        QVERIFY(!denied.explanation.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        file.close();
        QVERIFY(!waibusnap::exportPngToPath({}, path).success);
        QVERIFY(!waibusnap::exportPngToPath(image, {}).success);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
    }
    void suggestedNamesDoNotCollide()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDateTime time(QDate(2026, 10, 2), QTime(12, 30, 45, 123));
        const QString first = waibusnap::suggestedPngPath(temporary.path(), time);
        QVERIFY(first.endsWith(QStringLiteral("WaibuSnap_2026-10-02_12-30-45-123.png")));
        QFile existing(first);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();
        const QString second = waibusnap::suggestedPngPath(temporary.path(), time);
        QVERIFY(second != first);
        QVERIFY(!QFileInfo::exists(second));
        QCOMPARE(waibusnap::pngFilePath(QStringLiteral("图.PNG")), QStringLiteral("图.PNG"));
        QCOMPARE(waibusnap::pngFilePath(QStringLiteral("图.jpg")), QStringLiteral("图.jpg.png"));
        QVERIFY(waibusnap::pngFilePath({}).isEmpty());
    }
};
QTEST_GUILESS_MAIN(ImageOutputTest)
#include "image_output_test.moc"
