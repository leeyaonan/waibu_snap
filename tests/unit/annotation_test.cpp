#include "core/annotation.h"
#include "core/selection_geometry.h"
#include <QTest>
#include <cmath>
#include <limits>
#include <utility>
namespace
{
inline constexpr std::array<int, 3> sampleLineWidths = {2, 4, 8};

waibusnap::Annotation sample(waibusnap::AnnotationType type, int index = 0)
{
    waibusnap::Annotation annotation;
    annotation.type = type;
    annotation.first = {qreal(10 + index), 20};
    annotation.last = {qreal(50 + index), 60};
    annotation.points = {annotation.first, {30, 40}, annotation.last};
    annotation.text = QStringLiteral("截图说明 ABC 123\n第二行");
    return annotation;
}
void compare(const waibusnap::Annotation& actual, const waibusnap::Annotation& expected)
{
    QCOMPARE(actual.type, expected.type);
    QCOMPARE(actual.first, expected.first);
    QCOMPARE(actual.last, expected.last);
    QCOMPARE(actual.points, expected.points);
    QCOMPARE(actual.text, expected.text);
    QCOMPARE(actual.style.color, expected.style.color);
    QCOMPARE(actual.style.lineWidth, expected.style.lineWidth);
    QCOMPARE(actual.style.textSize, expected.style.textSize);
    QCOMPARE(actual.style.fontFamily, expected.style.fontFamily);
    QCOMPARE(actual.style.bold, expected.style.bold);
    QCOMPARE(actual.style.italic, expected.style.italic);
    QCOMPARE(actual.style.underline, expected.style.underline);
}
}
class AnnotationTest final : public QObject
{
    Q_OBJECT
  private slots:
    void paletteAndWidthDefaults()
    {
        using namespace waibusnap;
        QCOMPARE(annotationColors().size(), size_t(9));
        QCOMPARE(annotationColorNames().size(), annotationColors().size());
        QCOMPARE(AnnotationStyle{}.color, QColor(255, 70, 70));
        QCOMPARE(AnnotationStyle{}.lineWidth, 4);
        QCOMPARE(AnnotationStyle{}.textSize, 28);
        const QStringList names = {"红", "橙", "黄", "绿", "青", "蓝", "紫", "黑", "白"};
        for (int index = 0; index < int(annotationColors().size()); ++index)
        {
            QCOMPARE(annotationColorNames()[index], names[index]);
            QCOMPARE(annotationColors()[index].alpha(), 255);
            for (int other = 0; other < index; ++other)
                QVERIFY(annotationColors()[index] != annotationColors()[other]);
        }
        for (int type = 0; type < 8; ++type)
            QCOMPARE(annotationUsesLineWidth(static_cast<AnnotationType>(type)), type < 5);
    }
    void wheelStepsAndBounds_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<QPoint>("angle");
        QTest::addColumn<QPoint>("pixel");
        QTest::addColumn<int>("expected");
        QTest::newRow("up") << 4 << QPoint(0, 120) << QPoint() << 5;
        QTest::newRow("down") << 4 << QPoint(0, -120) << QPoint() << 3;
        QTest::newRow("multiple") << 4 << QPoint(0, 720) << QPoint() << 10;
        QTest::newRow("max") << 23 << QPoint(0, 9600) << QPoint() << 24;
        QTest::newRow("min") << 2 << QPoint(0, -9600) << QPoint() << 1;
        QTest::newRow("int-max") << 4 << QPoint(0, std::numeric_limits<int>::max()) << QPoint()
                                 << 24;
        QTest::newRow("int-min") << 4 << QPoint(0, std::numeric_limits<int>::min()) << QPoint()
                                 << 1;
        QTest::newRow("horizontal") << 4 << QPoint(120, 0) << QPoint() << 4;
        QTest::newRow("partial") << 4 << QPoint(0, 60) << QPoint() << 4;
        QTest::newRow("pixel-up") << 4 << QPoint() << QPoint(0, 15) << 5;
        QTest::newRow("pixel-down") << 4 << QPoint() << QPoint(0, -30) << 2;
        QTest::newRow("angle-priority") << 4 << QPoint(0, 120) << QPoint(0, 300) << 5;
        QTest::newRow("empty") << 4 << QPoint() << QPoint() << 4;
    }
    void wheelStepsAndBounds()
    {
        QFETCH(int, width);
        QFETCH(QPoint, angle);
        QFETCH(QPoint, pixel);
        QFETCH(int, expected);
        waibusnap::AnnotationWidthWheel wheel;
        QCOMPARE(wheel.adjust(width, angle, pixel), expected);
    }
    void wheelAccumulationAndReset()
    {
        using namespace waibusnap;
        AnnotationWidthWheel wheel;
        QCOMPARE(wheel.adjust(4, {0, 60}), 4);
        QCOMPARE(wheel.adjust(4, {0, 60}), 5);
        QCOMPARE(wheel.adjust(5, {0, -40}), 5);
        QCOMPARE(wheel.adjust(5, {0, -80}), 4);
        QCOMPARE(wheel.adjust(24, {0, 9601}), 24);
        QCOMPARE(wheel.adjust(24, {0, -120}), 23);
        QCOMPARE(wheel.adjust(1, {0, -9601}), 1);
        QCOMPARE(wheel.adjust(1, {0, 120}), 2);
        QCOMPARE(wheel.adjust(4, {0, 60}), 4);
        wheel.reset();
        QCOMPARE(wheel.adjust(4, {0, 60}), 4);
        // 切换输入单位不串入上一种事件的半格。
        QCOMPARE(wheel.adjust(4, {}, {0, 7}), 4);
        QCOMPARE(wheel.adjust(4, {}, {0, 8}), 5);
        QCOMPARE(wheel.adjust(5, {0, 60}), 5);
    }
    void eightTypesAndStyles()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        const AnnotationType types[] = {AnnotationType::Rectangle, AnnotationType::Ellipse,
                                        AnnotationType::Line,      AnnotationType::Arrow,
                                        AnnotationType::Freehand,  AnnotationType::Text,
                                        AnnotationType::Cover,     AnnotationType::Mosaic};
        for (int type = 0; type < 8; ++type)
        {
            QCOMPARE(static_cast<int>(types[type]), type);
            Annotation annotation = sample(static_cast<AnnotationType>(type));
            annotation.style = {annotationColors()[type % annotationColors().size()],
                                sampleLineWidths[type % 3], annotationTextSizes[type % 3],
                                QStringLiteral("测试字体")};
            QVERIFY(annotation.isVisible());
            QVERIFY(history.add(annotation));
            compare(history.annotations().last(), annotation);
        }
        QCOMPARE(history.annotations().size(), qsizetype(8));
        QCOMPARE(history.annotations().last().text, QStringLiteral("截图说明 ABC 123\n第二行"));
        for (int index = 1; index < 3; ++index)
        {
            QVERIFY(annotationColors()[index] != annotationColors()[index - 1]);
            QVERIFY(sampleLineWidths[index] > sampleLineWidths[index - 1]);
            QVERIFY(annotationTextSizes[index] > annotationTextSizes[index - 1]);
        }
    }
    void coverRejectsZeroAreaAndRestoresHistory()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        const auto rectangle = sample(AnnotationType::Rectangle);
        auto cover = sample(AnnotationType::Cover);
        QVERIFY(cover.isVisible());
        QVERIFY(history.add(rectangle));
        QVERIFY(history.add(cover));
        QVERIFY(history.undo());
        compare(history.annotations().last(), rectangle);
        for (const QPointF last : {cover.first, QPointF(cover.first.x(), cover.last.y()),
                                   QPointF(cover.last.x(), cover.first.y())})
        {
            auto empty = cover;
            empty.last = last;
            QVERIFY(!empty.isVisible());
            QVERIFY(!history.add(empty));
            QVERIFY(history.canRedo());
        }
        QVERIFY(history.redo());
        compare(history.annotations().last(), cover);
        std::swap(cover.first, cover.last);
        QVERIFY(cover.isVisible());
        QVERIFY(history.add(cover));
        QVERIFY(history.undo());
        QVERIFY(history.redo());
        compare(history.annotations().last(), cover);
    }
    void mosaicRejectsZeroAreaAndRestoresHistory()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        const auto rectangle = sample(AnnotationType::Rectangle);
        auto mosaic = sample(AnnotationType::Mosaic);
        // 马赛克不依赖颜色 / 线宽 / 字号，即使样式无效也不影响补丁。
        mosaic.style = {QColor(), 0, 0, {}};
        QVERIFY(mosaic.isVisible());
        QVERIFY(history.add(rectangle));
        QVERIFY(history.add(mosaic));
        QVERIFY(history.undo());
        compare(history.annotations().last(), rectangle);
        for (const QPointF last : {mosaic.first, QPointF(mosaic.first.x(), mosaic.last.y()),
                                   QPointF(mosaic.last.x(), mosaic.first.y())})
        {
            auto empty = mosaic;
            empty.last = last;
            QVERIFY(!empty.isVisible());
            QVERIFY(!history.add(empty));
            QVERIFY(history.canRedo());
        }
        QVERIFY(history.redo());
        compare(history.annotations().last(), mosaic);
        std::swap(mosaic.first, mosaic.last);
        QVERIFY(mosaic.isVisible());
        QVERIFY(history.add(mosaic));
        QVERIFY(history.undo());
        QVERIFY(history.redo());
        compare(history.annotations().last(), mosaic);
        QVERIFY(history.undo());
        QVERIFY(history.add(sample(AnnotationType::Mosaic, 1)));
        QVERIFY(!history.canRedo());
    }
    void adjustLastLineWidth()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        QVERIFY(!history.setLastLineWidth(8));
        QVERIFY(history.add(sample(AnnotationType::Line)));
        QCOMPARE(history.annotations().last().style.lineWidth, 4);
        QVERIFY(history.setLastLineWidth(9));
        QCOMPARE(history.annotations().last().style.lineWidth, 9);
        QVERIFY(!history.setLastLineWidth(9));
        QVERIFY(history.setLastLineWidth(100));
        QCOMPARE(history.annotations().last().style.lineWidth, annotationMaximumWidth);
        QVERIFY(history.setLastLineWidth(0));
        QCOMPARE(history.annotations().last().style.lineWidth, annotationMinimumWidth);
        // 改宽不新增历史项，撤销 / 重做保留最终宽度。
        QCOMPARE(history.annotations().size(), qsizetype(1));
        QVERIFY(history.undo());
        QVERIFY(history.redo());
        QCOMPARE(history.annotations().last().style.lineWidth, annotationMinimumWidth);
        // 文本等非线宽标注拒绝修改；新编辑清空重做栈。
        QVERIFY(history.add(sample(AnnotationType::Text)));
        QVERIFY(!history.setLastLineWidth(8));
        QVERIFY(history.undo());
        QVERIFY(history.canRedo());
        QVERIFY(history.setLastLineWidth(7));
        QVERIFY(!history.canRedo());
    }
    void textStyleAndWheelBounds()
    {
        using namespace waibusnap;
        AnnotationStyle style;
        QFont font = annotationFont(style);
        QVERIFY(!font.bold());
        QVERIFY(!font.italic());
        QVERIFY(!font.underline());
        style.bold = true;
        style.italic = true;
        style.underline = true;
        font = annotationFont(style);
        QVERIFY(font.bold());
        QVERIFY(font.italic());
        QVERIFY(font.underline());
        AnnotationWidthWheel wheel;
        QCOMPARE(
            wheel.adjust(28, {0, 120}, {}, annotationMinimumTextSize, annotationMaximumTextSize),
            29);
        QCOMPARE(
            wheel.adjust(29, {0, -240}, {}, annotationMinimumTextSize, annotationMaximumTextSize),
            27);
        QCOMPARE(
            wheel.adjust(9, {0, -120}, {}, annotationMinimumTextSize, annotationMaximumTextSize),
            annotationMinimumTextSize);
        QCOMPARE(
            wheel.adjust(96, {0, 120}, {}, annotationMinimumTextSize, annotationMaximumTextSize),
            annotationMaximumTextSize);
        AnnotationHistory history;
        QVERIFY(history.add(sample(AnnotationType::Text)));
        QVERIFY(history.setLastTextStyle(29, true, false, true));
        QCOMPARE(history.annotations().last().style.textSize, 29);
        QVERIFY(history.annotations().last().style.bold);
        QVERIFY(!history.annotations().last().style.italic);
        QVERIFY(history.annotations().last().style.underline);
        QVERIFY(!history.setLastTextStyle(29, true, false, true));
        QVERIFY(history.setLastTextStyle(1, false, false, false));
        QCOMPARE(history.annotations().last().style.textSize, annotationMinimumTextSize);
        QVERIFY(history.setLastTextStyle(100, false, false, false));
        QCOMPARE(history.annotations().last().style.textSize, annotationMaximumTextSize);
        QVERIFY(history.add(sample(AnnotationType::Line)));
        QVERIFY(!history.setLastTextStyle(30, true, true, true));
    }
    void arrowGeometryScalesAndRotates()
    {
        using namespace waibusnap;
        Annotation arrow = sample(AnnotationType::Arrow);
        arrow.first = {10, 20};
        arrow.last = {110, 20};
        arrow.style.lineWidth = 4;
        const auto head = arrowHead(arrow);
        QCOMPARE(head, QVector<QPointF>({{110, 20}, {94, 27.2}, {94, 12.8}}));
        arrow.style.lineWidth = 8;
        const auto thick = arrowHead(arrow);
        QVERIFY(thick[1].x() < head[1].x());
        QVERIFY(thick[1].y() > head[1].y());
        arrow.last = {10, 120};
        const auto vertical = arrowHead(arrow);
        QCOMPARE(vertical.first(), arrow.last);
        QCOMPARE(vertical[1].y(), qreal(88));
        QCOMPARE(vertical[2].y(), qreal(88));
        arrow.last = {13, 24};
        const auto shortHead = arrowHead(arrow);
        QCOMPARE((shortHead[1] + shortHead[2]) / 2, arrow.first);
        arrow.last = arrow.first;
        QVERIFY(arrowHead(arrow).isEmpty());
    }
    void arrowGeometryMatrix_data()
    {
        QTest::addColumn<int>("angle");
        QTest::addColumn<int>("width");
        QTest::addColumn<qreal>("length");
        for (int angle : {0, 45, 90, 135, 180, 225, 270, 315})
            for (int width : {1, 2, 8, std::numeric_limits<int>::max()})
                for (qreal length : {qreal(0.001), qreal(2), qreal(40), qreal(400)})
                    QTest::newRow(
                        qPrintable(QStringLiteral("%1-%2-%3").arg(angle).arg(width).arg(length)))
                        << angle << width << length;
    }
    void arrowGeometryMatrix()
    {
        QFETCH(int, angle);
        QFETCH(int, width);
        QFETCH(qreal, length);
        using namespace waibusnap;
        const qreal radians = angle * std::acos(qreal(-1)) / 180;
        const QPointF unit(std::cos(radians), std::sin(radians));
        Annotation arrow = sample(AnnotationType::Arrow);
        arrow.first = {-25, 37};
        arrow.last = arrow.first + unit * length;
        arrow.style.lineWidth = width;
        const auto head = arrowHead(arrow);
        QCOMPARE(head.size(), qsizetype(3));
        QCOMPARE(head[0], arrow.last);
        const QPointF base = (head[1] + head[2]) / 2;
        const qreal expected = std::min(length, std::max(qreal(10), qreal(width) * 4));
        QVERIFY(std::hypot((arrow.last - base).x(), (arrow.last - base).y()) <= length + 1e-8);
        QVERIFY(std::abs(QPointF::dotProduct(arrow.last - base, unit) - expected) < 1e-8);
        const QPointF wing = head[1] - base;
        QVERIFY(std::abs(QPointF::dotProduct(wing, unit)) < 1e-8);
        QVERIFY(std::abs(std::hypot(wing.x(), wing.y()) - expected * 0.45) < 1e-8);
        QVERIFY(std::hypot((head[2] - base + wing).x(), (head[2] - base + wing).y()) < 1e-8);
        // 两翼相对尖端的夹角由固定宽长比决定，且都位于尖端后方。
        for (int index : {1, 2})
        {
            const QPointF backward = head[index] - arrow.last;
            QVERIFY(QPointF::dotProduct(backward, unit) < 0);
            const qreal cosine =
                -QPointF::dotProduct(backward, unit) / std::hypot(backward.x(), backward.y());
            QVERIFY(std::abs(cosine - 1 / std::hypot(qreal(1), qreal(0.45))) < 1e-8);
        }
    }
    void arrowInvalidGeometry()
    {
        using namespace waibusnap;
        auto arrow = sample(AnnotationType::Arrow);
        for (int width : {0, -1, std::numeric_limits<int>::min()})
        {
            arrow.style.lineWidth = width;
            QVERIFY(arrowHead(arrow).isEmpty());
        }
        arrow.style.lineWidth = 8;
        for (qreal invalid :
             {std::numeric_limits<qreal>::infinity(), std::numeric_limits<qreal>::quiet_NaN()})
        {
            arrow.last = {invalid, 20};
            QVERIFY(arrowHead(arrow).isEmpty());
        }
        arrow.last = arrow.first;
        QVERIFY(arrowHead(arrow).isEmpty());
    }
    void freehandThinsInPhysicalPixels()
    {
        waibusnap::Annotation annotation;
        annotation.type = waibusnap::AnnotationType::Freehand;
        waibusnap::appendFreehandPoint(annotation, {10, 20});
        waibusnap::appendFreehandPoint(annotation, {10.2, 20.2});
        QCOMPARE(annotation.points.size(), qsizetype(1));
        waibusnap::appendFreehandPoint(annotation, {11, 20});
        QCOMPARE(annotation.points, QVector<QPointF>({{10, 20}, {11, 20}}));
        QVERIFY(annotation.isVisible());
    }
    void invalidAndEmptyBoundaries()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        QVERIFY(!history.undo());
        QVERIFY(!history.redo());
        QVERIFY(!history.add({}));
        Annotation emptyText = sample(AnnotationType::Text);
        emptyText.text = QStringLiteral(" \n\t");
        QVERIFY(!history.add(emptyText));
        Annotation invalid = sample(AnnotationType::Line);
        invalid.style.lineWidth = 0;
        QVERIFY(!history.add(invalid));
        invalid = sample(AnnotationType::Ellipse);
        invalid.last.setX(invalid.first.x());
        QVERIFY(!history.add(invalid));
        QVERIFY(!history.canUndo());
        QVERIFY(!history.canRedo());
    }
    void capacityKeepsOldArtworkAndSequence()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        for (int index = 0; index < 25; ++index)
            QVERIFY(history.add(sample(static_cast<AnnotationType>(index % 8), index)));
        const auto original = history.annotations();
        QCOMPARE(original.size(), qsizetype(25));
        for (int index = 0; index < AnnotationHistory::capacity; ++index)
            QVERIFY(history.undo());
        QVERIFY(!history.undo());
        QCOMPARE(history.annotations().size(), qsizetype(5));
        for (int index = 0; index < 5; ++index)
            compare(history.annotations()[index], original[index]);
        for (int index = 0; index < AnnotationHistory::capacity; ++index)
            QVERIFY(history.redo());
        QVERIFY(!history.redo());
        for (int index = 0; index < original.size(); ++index)
            compare(history.annotations()[index], original[index]);
        history.clear();
        QVERIFY(history.annotations().isEmpty());
        QVERIFY(!history.undo());
        QVERIFY(!history.redo());
    }
    void newAnnotationClearsBranch()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        QVERIFY(history.add(sample(AnnotationType::Rectangle)));
        QVERIFY(history.add(sample(AnnotationType::Ellipse)));
        QVERIFY(history.undo());
        QVERIFY(history.canRedo());
        // 无效 / 空标注不算新步骤，也不能丢弃分叉。
        QVERIFY(!history.add({}));
        QVERIFY(history.canRedo());
        const auto branch = sample(AnnotationType::Text);
        QVERIFY(history.add(branch));
        QVERIFY(!history.canRedo());
        QVERIFY(!history.redo());
        compare(history.annotations().last(), branch);
        QVERIFY(history.undo());
        QVERIFY(history.undo());
        QVERIFY(!history.undo());
        QVERIFY(history.redo());
        QVERIFY(history.redo());
        compare(history.annotations().last(), branch);
    }
    void cropWindowDoesNotChangeCoordinates()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        const auto annotation = sample(AnnotationType::Rectangle);
        QVERIFY(history.add(annotation));
        const QRect selection(10, 10, 60, 60);
        const QRect moved = movedPixelSelection(selection, {10, 20}, 2, {200, 200});
        QCOMPARE(moved, QRect(30, 50, 60, 60));
        QCOMPARE(resizedPixelSelection(moved, SelectionEdge::Right, {20, 0}, 2, {200, 200}),
                 QRect(30, 50, 100, 60));
        compare(history.annotations().first(), annotation);
    }
};
QTEST_APPLESS_MAIN(AnnotationTest)
#include "annotation_test.moc"
