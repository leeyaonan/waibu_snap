#include "app/app_settings.h"
#include "app/watermark_service.h"
#include "ui/selection_overlay.h"
#include <QImage>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
namespace
{
using namespace waibusnap;
CaptureFrame frame()
{
    CaptureFrame result;
    result.display.logicalGeometry = QRect(0, 0, 320, 200);
    result.display.devicePixelRatio = 2;
    result.pixels = QImage(640, 400, QImage::Format_ARGB32_Premultiplied);
    result.pixels.fill(Qt::white);
    result.pixels.setDevicePixelRatio(2);
    return result;
}
int bluePixels(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).blue() > 180 && image.pixelColor(x, y).red() < 100)
                ++count;
    return count;
}
WatermarkPreferences preferences(const QString& fileName)
{
    WatermarkPreferences result;
    WatermarkAsset asset;
    asset.id = QStringLiteral("asset");
    asset.name = QStringLiteral("蓝色块");
    asset.kind = WatermarkKind::Image;
    asset.imageFileName = fileName;
    result.assets = {asset};
    WatermarkPreset preset;
    preset.id = QStringLiteral("preset");
    preset.name = QStringLiteral("居中");
    preset.assetId = asset.id;
    preset.layout = WatermarkLayout::Center;
    preset.opacity = 100;
    preset.scale = 100;
    result.presets = {preset};
    result.defaultPresetId = preset.id;
    return result;
}
void selectRegion(SelectionOverlay& overlay)
{
    QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, {50, 50});
    QTest::mouseMove(&overlay, {250, 150});
    QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, {250, 150});
}
}
class WatermarkOverlayTest final : public QObject
{
    Q_OBJECT
  private slots:
    void manualWatermarkAppliesToOutput()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings, directory.filePath(QStringLiteral("assets")));
        QVERIFY(service.initialize());
        const QString sourcePath = directory.filePath(QStringLiteral("blue.png"));
        QImage source(40, 20, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(20, 80, 240));
        QVERIFY(source.save(sourcePath, "PNG"));
        QString managed;
        QString error;
        QCOMPARE(service.importImage(sourcePath, &managed, &error), QString());
        QVERIFY(!managed.isEmpty());
        QVERIFY(service.savePreferences(preferences(managed)).isEmpty());
        QCOMPARE(service.currentPresetId(), QStringLiteral("preset"));

        OverlayActions actions;
        actions.watermarks = &service;
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        QTest::qWait(10);
        selectRegion(overlay);
        QVERIFY(!overlay.selection().isEmpty());
        auto* tool = overlay.findChild<QPushButton*>(QStringLiteral("watermarkToolButton"));
        QVERIFY(tool);
        QVERIFY(tool->isEnabled());
        QTest::mouseClick(tool, Qt::LeftButton);
        QTRY_VERIFY(overlay.hasWatermark());
        QVERIFY(overlay.isWatermarkToolActive());
        const QString outputPath = directory.filePath(QStringLiteral("output.png"));
        QVERIFY(overlay.exportToPath(outputPath));
        QImage output(outputPath);
        QVERIFY(!output.isNull());
        QCOMPARE(output.size(), overlay.selection().size());
        QVERIFY(bluePixels(output) > 100);
        tool->click();
        QVERIFY(!overlay.isWatermarkToolActive());
        QVERIFY(overlay.hasWatermark());
    }
    void noPresetDisablesTool()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings, directory.filePath(QStringLiteral("assets")));
        QVERIFY(service.initialize());
        OverlayActions actions;
        actions.watermarks = &service;
        SelectionOverlay overlay(frame(), actions);
        overlay.show();
        QTest::qWait(10);
        selectRegion(overlay);
        auto* tool = overlay.findChild<QPushButton*>(QStringLiteral("watermarkToolButton"));
        QVERIFY(tool);
        QVERIFY(!tool->isEnabled());
        QTest::mouseClick(tool, Qt::LeftButton);
        QVERIFY(!overlay.hasWatermark());
    }
};
QTEST_MAIN(WatermarkOverlayTest)
#include "watermark_overlay_test.moc"
