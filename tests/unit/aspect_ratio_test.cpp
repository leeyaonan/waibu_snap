#include "core/aspect_ratio.h"
#include <QTest>
#include <cmath>
using namespace waibusnap;
namespace
{
void verifyRatio(QRect rect, AspectRatio ratio, QSize bounds)
{
    QVERIFY2(!rect.isEmpty(), "选区至少为 1×1 像素");
    QVERIFY(QRect(QPoint(), bounds).contains(rect));
    const double r = ratio.width / ratio.height;
    QVERIFY(std::min(std::abs(rect.width() - rect.height() * r),
                     std::abs(rect.height() - rect.width() / r)) <= 1.0 + 1e-8);
}
QPointF center(QRect rect)
{
    return {rect.x() + rect.width() / 2.0, rect.y() + rect.height() / 2.0};
}
}
class AspectRatioTest final : public QObject
{
    Q_OBJECT
  private slots:
    void parsingAndFormatting()
    {
        for (const auto& text : {"16:9", "2.35:1", "2.5:1", "999.99:0.01"})
        {
            const auto parsed = parseAspectRatio(QString::fromLatin1(text));
            QVERIFY(parsed);
            QCOMPARE(formatAspectRatio(*parsed), QString::fromLatin1(text));
        }
        QCOMPARE(formatAspectRatio(*parseAspectRatio(QStringLiteral(" 2.50:1.00 "))),
                 QStringLiteral("2.5:1"));
        for (const auto& text : {"0:1", "-1:2", "abc", "1.001:1", "1:1.234", "nan:1", "1:inf",
                                 "1000:1", "1:0.00", "1e2:1", "1:1:1", "1:1junk", ""})
            QVERIFY(!parseAspectRatio(QString::fromLatin1(text)));
        QCOMPARE(serializeRatioList(defaultCaptureRatios()),
                 QStringLiteral("1:1,16:9,9:16,4:3,3:4,3:2,2:3,2.35:1"));
    }
    void duplicateTolerance()
    {
        QVERIFY(sameAspectRatio({16, 9}, {32, 18}));
        QVERIFY(sameAspectRatio({1, 1}, {1, 0.999}));
        QVERIFY(sameAspectRatio({1, 0.999}, {1, 1}));
        QVERIFY(!sameAspectRatio({1, 1}, {1, 0.9989}));
        QVERIFY(!sameAspectRatio({16, 9}, {9, 16}));
        QVERIFY(!sameAspectRatio({0, 1}, {1, 1}));
    }
    void createdSelection_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::addColumn<int>("direction");
        QTest::addColumn<QString>("ratioText");
        QTest::addColumn<QPoint>("delta");
        for (qreal scale : {1.0, 1.25, 1.5, 2.0})
            for (int direction = 0; direction < 4; ++direction)
                for (const auto& ratio : {"16:9", "9:16", "999.99:0.01", "0.01:999.99"})
                    for (QPoint delta : {QPoint(35, 15), QPoint(1000, 1000), QPoint(0, 0)})
                        QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4-%5")
                                                     .arg(scale)
                                                     .arg(direction)
                                                     .arg(ratio)
                                                     .arg(delta.x())
                                                     .arg(delta.y())))
                            << scale << direction << QString::fromLatin1(ratio) << delta;
    }
    void createdSelection()
    {
        QFETCH(qreal, scale);
        QFETCH(int, direction);
        QFETCH(QString, ratioText);
        QFETCH(QPoint, delta);
        const AspectRatio ratio = *parseAspectRatio(ratioText);
        const QPointF anchor(200, 160);
        const int sx = direction & 1 ? -1 : 1;
        const int sy = direction & 2 ? -1 : 1;
        const QRect result = ratioCreatedSelection(
            anchor / scale, (anchor + QPointF(delta.x() * sx, delta.y() * sy)) / scale, ratio,
            scale, {400, 320});
        verifyRatio(result, ratio, {400, 320});
        QCOMPARE(sx < 0 && delta.x() ? result.x() + result.width() : result.x(), 200);
        QCOMPARE(sy < 0 && delta.y() ? result.y() + result.height() : result.y(), 160);
    }
    void anchoredCreationAndInvalid()
    {
        QCOMPARE(ratioCreatedSelection({20, 30}, {50, 90}, {16, 9}, 1, {400, 300}),
                 QRect(20, 30, 107, 60));
        verifyRatio(ratioCreatedSelection({400, 300}, {400, 300}, {1, 1}, 1, {400, 300}), {1, 1},
                    {400, 300});
        QVERIFY(ratioCreatedSelection({}, {}, {1, 1}, 0, {400, 300}).isEmpty());
        QVERIFY(ratioCreatedSelection({}, {}, {0, 1}, 1, {400, 300}).isEmpty());
        QVERIFY(ratioCreatedSelection({}, {}, {1, 1}, 1, {}).isEmpty());
    }
    void resizing_data()
    {
        QTest::addColumn<int>("edges");
        QTest::addColumn<qreal>("scale");
        QTest::addColumn<QPointF>("delta");
        QTest::addColumn<QString>("ratioText");
        for (int edge : {1, 2, 4, 8, 3, 6, 9, 12})
            for (qreal scale : {1.0, 1.5, 2.0})
                for (QPointF delta : {QPointF(15, 20), QPointF(-1000, -1000), QPointF(1000, 1000)})
                    for (const auto& ratio : {"16:9", "9:16"})
                        QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4-%5")
                                                     .arg(edge)
                                                     .arg(scale)
                                                     .arg(delta.x())
                                                     .arg(delta.y())
                                                     .arg(ratio)))
                            << edge << scale << delta << QString::fromLatin1(ratio);
    }
    void resizing()
    {
        QFETCH(int, edges);
        QFETCH(qreal, scale);
        QFETCH(QPointF, delta);
        QFETCH(QString, ratioText);
        const QRect initial(100, 80, 160, 90);
        const auto ratio = *parseAspectRatio(ratioText);
        const auto flags = SelectionEdges::fromInt(edges);
        const QRect result = ratioResizedSelection(initial, flags, delta, ratio, scale, {400, 320});
        verifyRatio(result, ratio, {400, 320});
        if (edges & 1)
            QCOMPARE(result.x() + result.width(), initial.x() + initial.width());
        if (edges & 4)
            QCOMPARE(result.x(), initial.x());
        if (edges & 2)
            QCOMPARE(result.y() + result.height(), initial.y() + initial.height());
        if (edges & 8)
            QCOMPARE(result.y(), initial.y());
        if (edges == 1 || edges == 4)
            QVERIFY(std::abs(center(result).y() - center(initial).y()) <= 0.5);
        if (edges == 2 || edges == 8)
            QVERIFY(std::abs(center(result).x() - center(initial).x()) <= 0.5);
    }
    void edgeFollowsPrimaryDimension()
    {
        const QRect initial(100, 80, 160, 90);
        QCOMPARE(
            ratioResizedSelection(initial, SelectionEdge::Right, {16, 999}, {16, 9}, 1, {400, 320}),
            QRect(100, 76, 176, 99));
        QCOMPARE(
            ratioResizedSelection(initial, SelectionEdge::Bottom, {999, 9}, {16, 9}, 1, {400, 320}),
            QRect(92, 80, 176, 99));
        QVERIFY(
            ratioResizedSelection({}, SelectionEdge::Right, {}, {1, 1}, 1, {400, 320}).isEmpty());
    }
    void keyboardNudges_data()
    {
        QTest::addColumn<int>("edge");
        QTest::addColumn<bool>("shrink");
        for (int edge : {1, 2, 4, 8})
            for (bool shrink : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(edge).arg(shrink)))
                    << edge << shrink;
    }
    void keyboardNudges()
    {
        QFETCH(int, edge);
        QFETCH(bool, shrink);
        const QRect initial(100, 80, 160, 90);
        const auto result = ratioNudgedSelection(
            initial, static_cast<SelectionEdge>(edge),
            shrink ? SelectionNudge::Shrink : SelectionNudge::Expand, {16, 9}, {400, 320});
        verifyRatio(result, {16, 9}, {400, 320});
        QCOMPARE(edge == 1 || edge == 4 ? result.width() : result.height(),
                 (edge == 1 || edge == 4 ? initial.width() : initial.height()) + (shrink ? -1 : 1));
        const auto move = ratioNudgedSelection(initial, static_cast<SelectionEdge>(edge),
                                               SelectionNudge::Move, {16, 9}, {400, 320});
        QCOMPARE(move.size(), initial.size());
        QCOMPARE(move, nudgedPixelSelection(initial, static_cast<SelectionEdge>(edge),
                                            SelectionNudge::Move, {400, 320}));
    }
    void inscribedAndAdjusted()
    {
        QCOMPARE(largestInscribedRatioRect({10, 20, 200, 200}, {16, 9}), QRect(10, 64, 200, 113));
        QCOMPARE(largestInscribedRatioRect({10, 20, 320, 90}, {16, 9}), QRect(90, 20, 160, 90));
        const QRect current(100, 80, 160, 160);
        const QRect adjusted = ratioAdjustedSelection(current, {16, 9}, {400, 320});
        QCOMPARE(adjusted, QRect(100, 115, 160, 90));
        QCOMPARE(center(adjusted), center(current));
        // 保持中心时高度不足：回缩宽度，而非把选区向屏内平移。
        const QRect nearTop(100, 0, 160, 20);
        const QRect limited = ratioAdjustedSelection(nearTop, {1, 1}, {400, 320});
        QCOMPARE(limited, QRect(170, 0, 20, 20));
        QCOMPARE(center(limited), center(nearTop));
        verifyRatio(ratioAdjustedSelection({0, 0, 1, 1}, {0.01, 999.99}, {400, 320}),
                    {0.01, 999.99}, {400, 320});
    }
    void settingsSanitizeAndFallback()
    {
        CaptureRatioSettings settings{{{16, 9}, {32, 18}, {1, 1}, {0, 1}, {1.001, 1}},
                                      {{2, 2}, {3, 4}, {6, 8}},
                                      AspectRatio{3, 4}};
        auto clean = sanitizedRatioSettings(settings);
        QCOMPARE(serializeRatioList(clean.enabled), QStringLiteral("16:9,1:1"));
        QCOMPARE(serializeRatioList(clean.disabled), QStringLiteral("3:4"));
        QCOMPARE(formatAspectRatio(*clean.quick), QStringLiteral("1:1"));
        settings.enabled = {{16, 9}};
        clean = sanitizedRatioSettings(settings);
        QCOMPARE(formatAspectRatio(*clean.quick), QStringLiteral("16:9"));
        settings.enabled.clear();
        QVERIFY(!sanitizedRatioSettings(settings).quick);
        settings.enabled = {{16, 9}, {1, 1}};
        settings.quick = AspectRatio{32, 18};
        QCOMPARE(formatAspectRatio(*sanitizedRatioSettings(settings).quick),
                 QStringLiteral("16:9"));
        QCOMPARE(serializeRatioList(parseRatioList(serializeRatioList(defaultCaptureRatios()))),
                 serializeRatioList(defaultCaptureRatios()));
        QCOMPARE(serializeRatioList(parseRatioList(QStringLiteral("bad,1:1,,0:1,2.35:1,1.001:1"))),
                 QStringLiteral("1:1,2.35:1"));
    }
};
QTEST_APPLESS_MAIN(AspectRatioTest)
#include "aspect_ratio_test.moc"
