#include "core/multi_display.h"
#include "output/multi_display_output.h"
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
    return target;
}
QImage solid(QSize size, QColor color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    image.setDevicePixelRatio(1);
    return image;
}
MultiDisplayStitchRequest stitchRequest(const MultiDisplayLayout& layout, QVector<QImage> frames,
                                        ImageFormat format = ImageFormat::Png)
{
    MultiDisplayStitchRequest request;
    request.layout = layout;
    request.frames = std::move(frames);
    request.format = format;
    return request;
}
}
class MultiDisplayOutputTest final : public QObject
{
    Q_OBJECT
  private slots:
    void stitchRejectsMismatchedInput()
    {
        const auto layout = buildMultiDisplayLayout({screen(1, {0, 0, 100, 100})});
        QVERIFY(composeMultiDisplayStitch(stitchRequest(layout, {})).isNull());
        QVERIFY(composeMultiDisplayStitch(stitchRequest(layout, {solid({100, 100}, Qt::red)}))
                    .isNull() == false);
        MultiDisplayLayout bad;
        QVERIFY(composeMultiDisplayStitch(stitchRequest(bad, {solid({10, 10}, Qt::red)})).isNull());
    }
    void stitchKeepsTransparentGapsForPng()
    {
        // 上两块 + 下一块：左下方（内屏左侧）必须完全透明。
        const auto layout = buildMultiDisplayLayout({screen(3, {0, 1080, 1920, 1080}),
                                                     screen(1, {-1920, 0, 1920, 1080}),
                                                     screen(2, {0, 0, 1920, 1080})});
        QVERIFY(layout.isValid());
        QCOMPARE(layout.canvasPixels, QSize(3840, 2160));
        const auto stitch = composeMultiDisplayStitch(
            stitchRequest(layout, {solid({1920, 1080}, Qt::red), solid({1920, 1080}, Qt::green),
                                   solid({1920, 1080}, Qt::blue)}));
        QVERIFY(!stitch.isNull());
        QCOMPARE(stitch.size(), QSize(3840, 2160));
        QCOMPARE(stitch.pixelColor(960, 540), QColor(Qt::red));
        QCOMPARE(stitch.pixelColor(2880, 540), QColor(Qt::green));
        // 下方屏居中：左边 1920 像素为空白。
        QCOMPARE(stitch.pixelColor(100, 1620).alpha(), 0);
        QCOMPARE(stitch.pixelColor(2880, 1620), QColor(Qt::blue));
        // 四周不留边距：最后一列仍在屏内。
        QCOMPARE(stitch.pixelColor(3839, 0), QColor(Qt::green));
    }
    void stitchFillsWhiteForJpeg()
    {
        const auto layout = buildMultiDisplayLayout(
            {screen(2, {0, 1080, 1080, 1920}), screen(1, {0, 0, 1920, 1080})});
        const auto stitch = composeMultiDisplayStitch(
            stitchRequest(layout, {solid({1920, 1080}, Qt::red), solid({1080, 1920}, Qt::blue)},
                          ImageFormat::Jpeg));
        QVERIFY(!stitch.isNull());
        QCOMPARE(stitch.size(), QSize(1920, 3000));
        // 下方屏只有 1080 宽，右侧空白必须为白色而非透明。
        QCOMPARE(stitch.pixelColor(1500, 2500), QColor(Qt::white));
    }
    void stitchScalesMixedDpr()
    {
        // 4K@2 与 1080p@1 逻辑等大：拼接后各占 3840×2160。
        const auto layout = buildMultiDisplayLayout(
            {screen(1, {0, 0, 1920, 1080}, 2.0), screen(2, {1920, 0, 1920, 1080}, 1.0)});
        const auto stitch = composeMultiDisplayStitch(
            stitchRequest(layout, {solid({3840, 2160}, Qt::red), solid({1920, 1080}, Qt::blue)}));
        QVERIFY(!stitch.isNull());
        QCOMPARE(stitch.size(), QSize(7680, 2160));
        QCOMPARE(stitch.pixelColor(1919, 1000), QColor(Qt::red));
        QCOMPARE(stitch.pixelColor(3841, 1000), QColor(Qt::blue));
        QCOMPARE(stitch.pixelColor(7670, 2150), QColor(Qt::blue));
    }
    void saveWritesWholeGroup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MultiDisplaySaveRequest request;
        request.directory = directory.path();
        request.stitch = true;
        request.stitchImage = solid({40, 10}, Qt::white);
        request.frames = {solid({20, 10}, Qt::red), solid({20, 10}, Qt::green),
                          solid({20, 10}, Qt::blue)};
        const auto result = saveMultiDisplayGroup(
            request, fixedTimestamp(QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368))));
        QVERIFY2(result.success, qPrintable(result.explanation));
        QCOMPARE(result.paths.size(), 4);
        for (const auto& path : result.paths)
            QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QFileInfo(result.paths[0]).fileName(),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_00.png"));
        QCOMPARE(QFileInfo(result.paths[3]).fileName(),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_03.png"));
        QVERIFY(QFileInfo(result.paths[0]).size() > 0);
    }
    void saveWithoutStitchStartsAtOne()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MultiDisplaySaveRequest request;
        request.directory = directory.path();
        request.frames = {solid({20, 10}, Qt::red), solid({20, 10}, Qt::green)};
        const auto result = saveMultiDisplayGroup(
            request, fixedTimestamp(QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368))));
        QVERIFY2(result.success, qPrintable(result.explanation));
        QCOMPARE(result.paths.size(), 2);
        QCOMPARE(QFileInfo(result.paths[0]).fileName(),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_01.png"));
        QCOMPARE(QFileInfo(result.paths[1]).fileName(),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_02.png"));
    }
    void saveRollsBackOnFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MultiDisplaySaveRequest request;
        request.directory = directory.path();
        request.stitch = true;
        request.stitchImage = solid({40, 10}, Qt::white);
        request.frames = {solid({20, 10}, Qt::red), solid({20, 10}, Qt::green)};
        MultiDisplaySaveActions actions =
            fixedTimestamp(QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368)));
        int calls = 0;
        actions.write = [&calls](const QImage& image, const QString& path)
        {
            // 第二张（首块屏）注入失败：拼接图已写，必须被回收。
            if (++calls == 2)
                return ImageFileResult{{false, QStringLiteral("注入的写入失败。")}, {}};
            return ImageFileResult{exportPngToPath(image, path), path};
        };
        const auto result = saveMultiDisplayGroup(request, actions);
        QVERIFY(!result.success);
        QVERIFY(result.explanation.contains(QStringLiteral("第 1 屏")));
        QVERIFY(result.paths.isEmpty());
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 0);
    }
    void saveRejectsEmptyRequests()
    {
        MultiDisplaySaveRequest request;
        QVERIFY(!saveMultiDisplayGroup(request).success);
        request.directory = QDir::tempPath();
        QVERIFY(!saveMultiDisplayGroup(request).success);
        request.frames = {solid({10, 10}, Qt::red)};
        request.stitch = true;
        QVERIFY(!saveMultiDisplayGroup(request).success);
    }
    void saveRejectsPrefixExhaustion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MultiDisplaySaveRequest request;
        request.directory = directory.path();
        request.frames = {solid({20, 10}, Qt::red)};
        auto actions = fixedTimestamp(QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368)));
        actions.reservePrefix = [](const QString&, const QDateTime&, int, bool, const QString&)
        { return QString(); };
        const auto result = saveMultiDisplayGroup(request, actions);
        QVERIFY(!result.success);
        QVERIFY(result.explanation.contains(QStringLiteral("不冲突")));
    }
    void saveUsesJpegExtension()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MultiDisplaySaveRequest request;
        request.directory = directory.path();
        request.format = ImageFormat::Jpeg;
        request.frames = {solid({20, 10}, Qt::red)};
        const auto result = saveMultiDisplayGroup(
            request, fixedTimestamp(QDateTime(QDate(2026, 11, 21), QTime(13, 5, 12, 368))));
        QVERIFY2(result.success, qPrintable(result.explanation));
        QCOMPARE(QFileInfo(result.paths[0]).fileName(),
                 QStringLiteral("WaibuSnap_2026-11-21_13-05-12-368_01.jpg"));
    }

  private:
    static MultiDisplaySaveActions fixedTimestamp(const QDateTime& timestamp)
    {
        MultiDisplaySaveActions actions;
        actions.timestamp = [timestamp] { return timestamp; };
        return actions;
    }
};
QTEST_MAIN(MultiDisplayOutputTest)
#include "multi_display_output_test.moc"
