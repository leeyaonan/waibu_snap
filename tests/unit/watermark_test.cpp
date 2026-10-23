#include "core/watermark.h"
#include "output/watermark_renderer.h"
#include <QColor>
#include <QImage>
#include <QJsonDocument>
#include <QTest>
#include <cmath>
namespace
{
using namespace waibusnap;
WatermarkAsset textAsset()
{
    WatermarkAsset asset;
    asset.id = QStringLiteral("asset-text");
    asset.name = QStringLiteral("文字水印");
    asset.kind = WatermarkKind::Text;
    asset.text = QStringLiteral("WaibuSnap");
    asset.fontFamily = QStringLiteral("Helvetica");
    asset.fontPixelSize = 24;
    asset.color = QColor(20, 30, 40, 230);
    asset.bold = true;
    return asset;
}
WatermarkAsset imageAsset()
{
    WatermarkAsset asset;
    asset.id = QStringLiteral("asset-image");
    asset.name = QStringLiteral("图片水印");
    asset.kind = WatermarkKind::Image;
    asset.imageFileName = QStringLiteral("asset-image.png");
    return asset;
}
WatermarkPreferences samplePreferences()
{
    WatermarkPreferences preferences;
    preferences.assets = {textAsset(), imageAsset()};
    WatermarkPreset preset;
    preset.id = QStringLiteral("preset-default");
    preset.name = QStringLiteral("默认署名");
    preset.assetId = textAsset().id;
    preset.layout = WatermarkLayout::BottomRight;
    preset.opacity = 35;
    preset.scale = 120;
    preferences.presets = {preset};
    preferences.defaultPresetId = preset.id;
    preferences.autoEnabled = true;
    preferences.retainOriginal = true;
    return preferences;
}
QImage solidImage(QSize size, QColor color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    image.setDevicePixelRatio(1);
    return image;
}
int changedPixels(const QImage& before, const QImage& after)
{
    if (before.size() != after.size())
        return -1;
    int count = 0;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            if (before.pixel(x, y) != after.pixel(x, y))
                ++count;
    return count;
}
}
class WatermarkTest final : public QObject
{
    Q_OBJECT
  private slots:
    void jsonRoundTrip()
    {
        auto preferences = samplePreferences();
        QCOMPARE(watermarkPreferencesError(preferences), QString());
        const QJsonObject json = watermarkPreferencesToJson(preferences);
        WatermarkPreferences parsed;
        QString error;
        QVERIFY2(watermarkPreferencesFromJson(json, parsed, &error), qPrintable(error));
        QCOMPARE(parsed.assets.size(), 2);
        QCOMPARE(parsed.presets.size(), 1);
        QCOMPARE(parsed.assets[0].text, preferences.assets[0].text);
        QCOMPARE(parsed.assets[0].fontPixelSize, preferences.assets[0].fontPixelSize);
        QCOMPARE(parsed.assets[0].color, preferences.assets[0].color);
        QCOMPARE(parsed.assets[0].bold, preferences.assets[0].bold);
        QCOMPARE(parsed.assets[1].imageFileName, preferences.assets[1].imageFileName);
        QCOMPARE(parsed.presets[0].layout, preferences.presets[0].layout);
        QCOMPARE(parsed.presets[0].opacity, preferences.presets[0].opacity);
        QCOMPARE(parsed.presets[0].scale, preferences.presets[0].scale);
        QCOMPARE(parsed.defaultPresetId, preferences.defaultPresetId);
        QCOMPARE(parsed.autoEnabled, true);
        QCOMPARE(parsed.retainOriginal, true);
        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(
            QJsonDocument(json).toJson(QJsonDocument::Compact), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
    }
    void validation()
    {
        using namespace waibusnap;
        QCOMPARE(watermarkPreferencesError(samplePreferences()), QString());

        auto empty = samplePreferences();
        empty.assets[0].text.clear();
        QVERIFY(watermarkPreferencesError(empty).contains(QStringLiteral("文字")));

        auto badPath = samplePreferences();
        badPath.assets[1].imageFileName = QStringLiteral("sub/asset.png");
        QVERIFY(watermarkPreferencesError(badPath).contains(QStringLiteral("相对文件名")));

        auto badScale = samplePreferences();
        badScale.presets[0].scale = watermarkMaximumScale + 1;
        QVERIFY(watermarkPreferencesError(badScale).contains(QStringLiteral("缩放")));

        auto badOpacity = samplePreferences();
        badOpacity.presets[0].opacity = 0;
        QVERIFY(watermarkPreferencesError(badOpacity).contains(QStringLiteral("透明度")));

        auto missingDefault = samplePreferences();
        missingDefault.defaultPresetId.clear();
        QVERIFY(watermarkPreferencesError(missingDefault).contains(QStringLiteral("默认")));

        auto duplicateNames = samplePreferences();
        duplicateNames.assets[1].name = duplicateNames.assets[0].name;
        QVERIFY(watermarkPreferencesError(duplicateNames).contains(QStringLiteral("名称重复")));

        auto duplicateIds = samplePreferences();
        duplicateIds.assets[1].id = duplicateIds.assets[0].id;
        QVERIFY(watermarkPreferencesError(duplicateIds).contains(QStringLiteral("ID 重复")));

        auto tooMany = samplePreferences();
        for (int index = 0; index < watermarkMaximumPresets; ++index)
        {
            WatermarkPreset preset = tooMany.presets[0];
            preset.id = QStringLiteral("extra-%1").arg(index);
            preset.name = QStringLiteral("额外 %1").arg(index);
            tooMany.presets.append(preset);
        }
        QVERIFY(watermarkPreferencesError(tooMany).contains(QStringLiteral("最多 10 套")));

        WatermarkPreferences parsed;
        QString error;
        QVERIFY(!watermarkPreferencesFromJson(watermarkPreferencesToJson(missingDefault), parsed,
                                              &error));
        QVERIFY(!error.isEmpty());
    }
    void layoutsAndLookups()
    {
        using namespace waibusnap;
        QCOMPARE(watermarkLayouts().size(), size_t(7));
        for (auto layout : watermarkLayouts())
        {
            WatermarkLayout parsed = WatermarkLayout::Center;
            QVERIFY(watermarkLayoutFromKey(watermarkLayoutKey(layout), parsed));
            QCOMPARE(parsed, layout);
            QVERIFY(!watermarkLayoutName(layout).isEmpty());
        }
        auto preferences = samplePreferences();
        QVERIFY(findWatermarkAsset(preferences, QStringLiteral("asset-text")));
        QVERIFY(!findWatermarkAsset(preferences, QStringLiteral("missing")));
        QVERIFY(findWatermarkPreset(preferences, QStringLiteral("preset-default")));
        QCOMPARE(defaultWatermarkPreset(preferences)->name, QStringLiteral("默认署名"));
        QVERIFY(defaultWatermarkPreset(WatermarkPreferences{}) == nullptr);
    }
    void initialCenters()
    {
        using namespace waibusnap;
        const QSizeF size(40, 20);
        const QSize output(200, 100);
        const qreal epsilon = 1e-6;
        const QPointF topLeft = watermarkInitialCenter(WatermarkLayout::TopLeft, size, output);
        QVERIFY(std::abs(topLeft.x() - 36.0 / 200.0) < epsilon);
        QVERIFY(std::abs(topLeft.y() - 26.0 / 100.0) < epsilon);
        const QPointF bottomRight =
            watermarkInitialCenter(WatermarkLayout::BottomRight, size, output);
        QVERIFY(std::abs(bottomRight.x() - 164.0 / 200.0) < epsilon);
        QVERIFY(std::abs(bottomRight.y() - 74.0 / 100.0) < epsilon);
        QCOMPARE(watermarkInitialCenter(WatermarkLayout::Center, size, output), QPointF(0.5, 0.5));
        QCOMPARE(watermarkInitialCenter(WatermarkLayout::DiagonalForward, size, output),
                 QPointF(0.5, 0.5));
        QCOMPARE(watermarkInitialCenter(WatermarkLayout::BottomRight, QSizeF(400, 400), output),
                 QPointF(0.5, 0.5));
    }
    void imageWatermarkBottomRightAndTopmost()
    {
        using namespace waibusnap;
        const QImage assetImage = solidImage(QSize(40, 20), QColor(220, 20, 20));
        WatermarkAsset asset;
        asset.id = QStringLiteral("image");
        asset.name = QStringLiteral("红色块");
        asset.kind = WatermarkKind::Image;
        asset.imageFileName = QStringLiteral("image.png");
        const QImage base = solidImage(QSize(200, 100), QColor(240, 240, 240));
        WatermarkInstance instance;
        instance.layout = WatermarkLayout::BottomRight;
        instance.opacity = 100;
        instance.scale = 100;
        instance.normalizedCenter =
            watermarkInitialCenter(instance.layout, QSizeF(assetImage.size()), base.size());
        const QImage output = composeWatermark(base, asset, assetImage, instance);
        QCOMPARE(output.size(), base.size());
        const QPoint topLeft(4, 4);
        const QPoint center(164, 74);
        QCOMPARE(output.pixelColor(topLeft), QColor(240, 240, 240));
        QCOMPARE(output.pixelColor(center), QColor(220, 20, 20));
        QVERIFY(changedPixels(base, output) > 0);
        instance.visible = false;
        const QImage hidden = composeWatermark(base, asset, assetImage, instance);
        QCOMPARE(changedPixels(base, hidden), 0);
    }
    void textWatermarkAndScale()
    {
        using namespace waibusnap;
        WatermarkAsset asset = textAsset();
        const QImage base = solidImage(QSize(320, 200), QColor(240, 240, 240));
        const QSizeF baseSize = watermarkBaseSize(asset, {});
        QVERIFY(baseSize.width() > 0);
        QVERIFY(baseSize.height() > 0);
        WatermarkInstance instance;
        instance.layout = WatermarkLayout::Center;
        instance.opacity = 100;
        instance.scale = 100;
        instance.normalizedCenter = {0.5, 0.5};
        const QImage output = composeWatermark(base, asset, {}, instance);
        QVERIFY(changedPixels(base, output) > 0);
        instance.scale = 200;
        const QImage larger = composeWatermark(base, asset, {}, instance);
        QVERIFY(changedPixels(base, larger) > changedPixels(base, output));
        instance.visible = false;
        QCOMPARE(changedPixels(base, composeWatermark(base, asset, {}, instance)), 0);
    }
    void diagonalTileCoverage()
    {
        using namespace waibusnap;
        const QImage assetImage = solidImage(QSize(24, 12), QColor(20, 120, 220));
        WatermarkAsset asset;
        asset.id = QStringLiteral("tile");
        asset.name = QStringLiteral("平铺");
        asset.kind = WatermarkKind::Image;
        asset.imageFileName = QStringLiteral("tile.png");
        const QImage base = solidImage(QSize(400, 300), QColor(250, 250, 250));
        WatermarkInstance instance;
        instance.layout = WatermarkLayout::DiagonalForward;
        instance.opacity = 100;
        instance.scale = 100;
        const QImage output = composeWatermark(base, asset, assetImage, instance);
        QVERIFY(changedPixels(base, output) > 1000);
        instance.layout = WatermarkLayout::DiagonalReverse;
        const QImage reverse = composeWatermark(base, asset, assetImage, instance);
        QVERIFY(changedPixels(base, reverse) > 1000);
        QVERIFY(changedPixels(output, reverse) > 0);
    }
};
QTEST_MAIN(WatermarkTest)
#include "watermark_test.moc"
