#include "core/shape_mask.h"
#include <QPainter>
#include <QTest>
#include <cmath>
using namespace waibusnap;
class ShapeMaskTest final : public QObject
{
    Q_OBJECT
  private slots:
    void pathsAndNaturalRatios_data()
    {
        QTest::addColumn<int>("shape");
        QTest::addColumn<QRectF>("box");
        for (int shape = 0; shape < 6; ++shape)
            for (QRectF box : {QRectF(0, 0, 200, 200), QRectF(31, 47, 320, 180)})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(shape).arg(box.width())))
                    << shape << box;
    }
    void pathsAndNaturalRatios()
    {
        QFETCH(int, shape);
        QFETCH(QRectF, box);
        const auto id = static_cast<CaptureShape>(shape);
        const auto path = captureShapePath(id, box);
        QVERIFY(!path.isEmpty());
        QVERIFY(box.adjusted(-0.001, -0.001, 0.001, 0.001).contains(path.boundingRect()));
        QVERIFY(path.contains(box.center()));
        QVERIFY(!path.contains(box.topLeft()));
        QCOMPARE(captureShapeNaturalRatio(id).width, 1.0);
        QCOMPARE(captureShapeNaturalRatio(id).height, 1.0);
        QVERIFY(!captureShapeLabel(id).isEmpty());
        QVERIFY(captureShapePath(id, {}).isEmpty());
    }
    void starVertices()
    {
        const auto path = captureShapePath(CaptureShape::Star, {0, 0, 200, 200});
        QCOMPARE(path.elementCount(), 11);
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < 10; ++i)
        {
            const auto point = path.elementAt(i);
            const double radius = i % 2 ? 38.2 : 100.0;
            const double angle = -pi / 2 + i * pi / 5;
            QVERIFY(std::abs(point.x - (100 + std::cos(angle) * radius)) < 1e-8);
            QVERIFY(std::abs(point.y - (100 + std::sin(angle) * radius)) < 1e-8);
        }
        QVERIFY(std::abs(path.boundingRect().width() - 200 * std::cos(pi / 10)) < 1e-8);
        QVERIFY(std::abs(path.boundingRect().height() - 100 * (1 + std::cos(pi / 5))) < 1e-8);
    }
    void smileyAndRoundedBoundaries()
    {
        const auto face = captureShapePath(CaptureShape::Smiley, {0, 0, 200, 200});
        QVERIFY(face.contains({100, 100}));
        QVERIFY(!face.contains({68, 76}));
        QVERIFY(!face.contains({132, 76}));
        QVERIFY(!face.contains({100, 176}));
        QVERIFY(face.contains({100, 190}));
        const auto rounded = captureShapePath(CaptureShape::RoundedRect, {0, 0, 200, 100});
        QVERIFY(!rounded.contains({1, 1}));
        QVERIFY(rounded.contains({22, 1}));
        QVERIFY(rounded.contains({1, 22}));
    }
    void pixelsAndWhiteBackground()
    {
        QImage source(101, 81, QImage::Format_RGB32);
        source.fill(QColor(20, 70, 130));
        source.setDevicePixelRatio(2);
        QImage mask(source.size(), QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        {
            QPainter painter(&mask);
            painter.fillPath(captureShapePath(CaptureShape::Ellipse, mask.rect()), Qt::white);
        }
        for (bool white : {false, true})
        {
            const auto image = applyShapeMask(source, mask, white);
            QCOMPARE(image.size(), source.size());
            QCOMPARE(image.devicePixelRatio(), 1.0);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (mask.pixelColor(x, y).alpha() == 255)
                        QCOMPARE(image.pixelColor(x, y), source.pixelColor(x, y));
                    else
                        QCOMPARE(image.pixelColor(x, y),
                                 white ? QColor(Qt::white) : QColor(Qt::transparent));
            QCOMPARE(image.pixelColor(0, 0).alpha(), white ? 255 : 0);
            QCOMPARE(image.pixelColor(50, 40).alpha(), 255);
        }
        mask.fill(QColor(255, 255, 255, 128));
        QCOMPARE(applyShapeMask(source, mask, false).pixelColor(50, 40).alpha(), 128);
        QVERIFY(applyShapeMask({}, mask, false).isNull());
        QVERIFY(applyShapeMask(source, {}, false).isNull());
    }
    void serializationAndTransparency()
    {
        const auto entries = parseShapeTemplates(QStringLiteral(
            "a.png:on,b.png:off,a.png:off,A.PNG:on,bad,c.png:bad,../x.png:on,x.jpg:on,"
            "x\\y.png:on,:on,圆环.png:on"));
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries[2].label, QStringLiteral("圆环"));
        QCOMPARE(serializeShapeTemplates(entries),
                 QStringLiteral("a.png:on,b.png:off,圆环.png:on"));
        QImage image(30, 20, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QVERIFY(!hasTransparentPixels(image));
        image.setPixelColor(29, 19, QColor(0, 0, 0, 254));
        QVERIFY(hasTransparentPixels(image));
        QVERIFY(!hasTransparentPixels(image.convertToFormat(QImage::Format_RGB32)));
        QVERIFY(!hasTransparentPixels({}));
    }
};
QTEST_APPLESS_MAIN(ShapeMaskTest)
#include "shape_mask_test.moc"
