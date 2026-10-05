#include "core/annotation.h"
#include "core/selection_geometry.h"
#include <QTest>
#include <cmath>
namespace
{
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
}
}
class AnnotationTest final : public QObject
{
    Q_OBJECT
  private slots:
    void sixTypesAndStyles()
    {
        using namespace waibusnap;
        AnnotationHistory history;
        for (int type = 0; type < 6; ++type)
        {
            Annotation annotation = sample(static_cast<AnnotationType>(type));
            annotation.style = {annotationColors()[type % 3], annotationLineWidths[type % 3],
                                annotationTextSizes[type % 3], QStringLiteral("测试字体")};
            QVERIFY(annotation.isVisible());
            QVERIFY(history.add(annotation));
            compare(history.annotations().last(), annotation);
        }
        QCOMPARE(history.annotations().size(), qsizetype(6));
        QCOMPARE(history.annotations().last().text, QStringLiteral("截图说明 ABC 123\n第二行"));
        for (int index = 1; index < 3; ++index)
        {
            QVERIFY(annotationColors()[index] != annotationColors()[index - 1]);
            QVERIFY(annotationLineWidths[index] > annotationLineWidths[index - 1]);
            QVERIFY(annotationTextSizes[index] > annotationTextSizes[index - 1]);
        }
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
            QVERIFY(history.add(sample(static_cast<AnnotationType>(index % 6), index)));
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
