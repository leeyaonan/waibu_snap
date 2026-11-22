#include "core/multi_display.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
using namespace waibusnap;
namespace
{
DisplayTarget screen(quint32 id, QRect geometry, qreal dpr = 1.0)
{
    DisplayTarget target;
    target.id = id;
    target.logicalGeometry = geometry;
    target.devicePixelRatio = dpr;
    target.layoutVersion = 7;
    return target;
}
void writeFile(const QString& directory, const QString& name)
{
    QFile file(QDir(directory).filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}
}
class MultiDisplayTest final : public QObject
{
    Q_OBJECT
  private slots:
    void invalidInputs()
    {
        QVERIFY(buildMultiDisplayLayout({}).ordered.isEmpty());
        // id 为 0、尺寸为空、DPR 非法一律过滤；全部无效时布局为空。
        QVERIFY(buildMultiDisplayLayout({screen(0, {0, 0, 100, 100})}).ordered.isEmpty());
        QVERIFY(buildMultiDisplayLayout({screen(1, {})}).ordered.isEmpty());
        QVERIFY(buildMultiDisplayLayout({screen(1, {0, 0, 100, 100}, 0)}).ordered.isEmpty());
        const auto layout = buildMultiDisplayLayout({screen(1, {0, 0, 100, 100}), screen(2, {})});
        QCOMPARE(layout.ordered.size(), 1);
        QCOMPARE(layout.ordered.first().target.id, quint32(1));
    }
    void horizontalOrderFollowsLayout()
    {
        // 三屏横排，枚举顺序故意打乱；结果必须按 left 升序。
        const auto layout = buildMultiDisplayLayout({screen(30, {3840, 0, 1920, 1080}),
                                                     screen(10, {-3840, 0, 3840, 2160}),
                                                     screen(20, {0, 0, 1920, 1080})});
        QCOMPARE(layout.ordered.size(), 3);
        QCOMPARE(layout.ordered[0].target.id, quint32(10));
        QCOMPARE(layout.ordered[1].target.id, quint32(20));
        QCOMPARE(layout.ordered[2].target.id, quint32(30));
        QCOMPARE(layout.logicalBounds, QRect(-3840, 0, 9600, 2160));
        QCOMPARE(layout.scale, qreal(1));
    }
    void twoAboveOneBelow()
    {
        // 笔记本内屏在下方居中，两台外接屏在上方：行 = [左上, 右上] / [下方]。
        const auto layout = buildMultiDisplayLayout({screen(3, {0, 1080, 1920, 1080}),
                                                     screen(1, {-1920, 0, 1920, 1080}),
                                                     screen(2, {0, 0, 1920, 1080})});
        QCOMPARE(layout.ordered.size(), 3);
        QCOMPARE(layout.ordered[0].target.id, quint32(1));
        QCOMPARE(layout.ordered[1].target.id, quint32(2));
        QCOMPARE(layout.ordered[2].target.id, quint32(3));
        // 下方屏幕左侧留空（逻辑 -1920 起）。
        QCOMPARE(layout.placements[2], QRect(1920, 1080, 1920, 1080));
    }
    void staggeredRowsStillGrouped()
    {
        // 右侧屏幕与左侧有 100px 垂直重叠：算同一行，按 left 排序。
        const auto layout = buildMultiDisplayLayout(
            {screen(2, {1920, 100, 1920, 1080}), screen(1, {0, 0, 1920, 1080})});
        QCOMPARE(layout.ordered.size(), 2);
        QCOMPARE(layout.ordered[0].target.id, quint32(1));
        QCOMPARE(layout.ordered[1].target.id, quint32(2));
        QCOMPARE(layout.logicalBounds, QRect(0, 0, 3840, 1180));
        QCOMPARE(layout.placements[1], QRect(1920, 100, 1920, 1080));
    }
    void verticalStackWithoutOverlap()
    {
        // 上下完全不重叠：分成两行，行按 top 升序。
        const auto layout = buildMultiDisplayLayout(
            {screen(2, {0, 1080, 1080, 1920}), screen(1, {0, 0, 1920, 1080})});
        QCOMPARE(layout.ordered[0].target.id, quint32(1));
        QCOMPARE(layout.ordered[1].target.id, quint32(2));
        QCOMPARE(layout.logicalBounds, QRect(0, 0, 1920, 3000));
    }
    void identicalGeometryFallsBackToId()
    {
        const auto layout =
            buildMultiDisplayLayout({screen(9, {0, 0, 1920, 1080}), screen(7, {0, 0, 1920, 1080})});
        QCOMPARE(layout.ordered.size(), 2);
        QCOMPARE(layout.ordered[0].target.id, quint32(7));
        QCOMPARE(layout.ordered[1].target.id, quint32(9));
        QCOMPARE(layout.logicalBounds, QRect(0, 0, 1920, 1080));
        QCOMPARE(layout.placements[0], QRect(0, 0, 1920, 1080));
        QCOMPARE(layout.placements[1], QRect(0, 0, 1920, 1080));
    }
    void mixedDprUsesMaximum()
    {
        // 4K@200% 与 1080p@100% 在桌面占同样大小 → 拼接图中等大。
        const auto layout = buildMultiDisplayLayout(
            {screen(1, {0, 0, 1920, 1080}, 2.0), screen(2, {1920, 0, 1920, 1080}, 1.0)});
        QCOMPARE(layout.scale, qreal(2));
        QCOMPARE(layout.canvasPixels, QSize(7680, 2160));
        QCOMPARE(layout.placements[0], QRect(0, 0, 3840, 2160));
        QCOMPARE(layout.placements[1], QRect(3840, 0, 3840, 2160));
        // 单屏物理尺寸仍按各自 DPR，不受拼接倍率影响。
        QCOMPARE(layout.ordered[0].pixelSize, QSize(3840, 2160));
        QCOMPARE(layout.ordered[1].pixelSize, QSize(1920, 1080));
    }
    void negativeCoordinatesKeepOffset()
    {
        const auto layout = buildMultiDisplayLayout(
            {screen(1, {-2560, -400, 2560, 1440}, 1.0), screen(2, {0, 0, 1920, 1080}, 1.0)});
        // 并集高度由两屏 top / bottom 决定：-400 起，到 1080 结束。
        QCOMPARE(layout.logicalBounds, QRect(-2560, -400, 4480, 1480));
        QCOMPARE(layout.placements[0], QRect(0, 0, 2560, 1440));
        QCOMPARE(layout.placements[1], QRect(2560, 400, 1920, 1080));
    }
    void canvasLimitBoundaries()
    {
        // 16384 边长可过，16385 失败。
        const auto passing = buildMultiDisplayLayout({screen(1, {0, 0, 16384, 100}, 1.0)});
        QVERIFY(!passing.stitchExceedsLimit);
        const auto failing = buildMultiDisplayLayout({screen(1, {0, 0, 16385, 100}, 1.0)});
        QVERIFY(failing.stitchExceedsLimit);
        QVERIFY(!failing.limitReason.isEmpty());
        QVERIFY(failing.limitReason.contains(QStringLiteral("16385")));
        // 面积超限：16384×16384 正好到界（= 268435456）通过，再加一列即失败。
        QVERIFY(
            !buildMultiDisplayLayout({screen(1, {0, 0, 16384, 16384}, 1.0)}).stitchExceedsLimit);
        QVERIFY(buildMultiDisplayLayout(
                    {screen(1, {0, 0, 16384, 16384}, 1.0), screen(2, {16384, 0, 1, 16384}, 1.0)})
                    .stitchExceedsLimit);
    }
    void singleDisplayStillProducesLayout()
    {
        const auto layout = buildMultiDisplayLayout({screen(1, {0, 0, 2560, 1440}, 2.0)});
        QVERIFY(layout.isValid());
        QCOMPARE(layout.ordered.size(), 1);
        QCOMPARE(layout.canvasPixels, QSize(5120, 2880));
        QCOMPARE(layout.placements.first(), QRect(0, 0, 5120, 2880));
    }
    void namesFollowIndexRules()
    {
        const auto timestamp = QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368));
        const auto base = multiDisplayBaseName(timestamp);
        QCOMPARE(base, QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368"));
        QCOMPARE(multiDisplayFileNames(base, 3, true, QStringLiteral(".png")),
                 QStringList({QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_00.png"),
                              QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_01.png"),
                              QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_02.png"),
                              QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_03.png")}));
        // 关闭拼接：没有 _00，单屏仍从 _01 起。
        QCOMPARE(multiDisplayFileNames(base, 2, false, QStringLiteral("jpg")),
                 QStringList({QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_01.jpg"),
                              QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_02.jpg")}));
        QVERIFY(multiDisplayFileNames(QString(), 1, true, QStringLiteral("png")).isEmpty());
        QVERIFY(multiDisplayFileNames(base, 0, true, QStringLiteral("png")).isEmpty());
    }
    void prefixAvoidsExistingFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto timestamp = QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368));
        // 先占据第一次与第二次尝试中的一张，必须整体顺延到下一个可用前缀。
        writeFile(directory.path(), QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_02.png"));
        writeFile(directory.path(), QStringLiteral("WaibuSnap_2026-11-21_13-05-12-369_01.png"));
        const auto prefix =
            multiDisplayPrefix(directory.path(), timestamp, 3, true, QStringLiteral("png"));
        QCOMPARE(prefix, QStringLiteral("WaibuSnap_2026-11-21_13-05-12-370"));
        // 关闭拼接的两屏组需要 _01 / _02；_02 仍被占据，同样顺延。
        QCOMPARE(multiDisplayPrefix(directory.path(), timestamp, 2, false, QStringLiteral("png")),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-370"));
        // 单屏组只要 _01，不与已占文件冲突，直接使用原始前缀。
        QCOMPARE(multiDisplayPrefix(directory.path(), timestamp, 1, false, QStringLiteral("png")),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368"));
    }
    void prefixGivesUpAfterBoundedAttempts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto timestamp = QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368));
        for (int attempt = 0; attempt < multiDisplayPrefixAttempts; ++attempt)
            writeFile(directory.path(), multiDisplayBaseName(timestamp.addMSecs(attempt)) +
                                            QStringLiteral("_00.png"));
        QVERIFY(multiDisplayPrefix(directory.path(), timestamp, 1, true, QStringLiteral("png"))
                    .isEmpty());
    }
    void prefixRejectsInvalidDirectory()
    {
        const auto timestamp = QDateTime::currentDateTime();
        QVERIFY(multiDisplayPrefix(QString(), timestamp, 1, true, QStringLiteral("png")).isEmpty());
        QVERIFY(
            multiDisplayPrefix(QString(), timestamp, 1, false, QStringLiteral("png")).isEmpty());
    }
};
QTEST_MAIN(MultiDisplayTest)
#include "multi_display_test.moc"
