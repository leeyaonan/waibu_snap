#include "app/app_settings.h"
#include "app/watermark_service.h"
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
namespace
{
using namespace waibusnap;
WatermarkAsset textAsset(const QString& id, const QString& name)
{
    WatermarkAsset asset;
    asset.id = id;
    asset.name = name;
    asset.kind = WatermarkKind::Text;
    asset.text = QStringLiteral("WaibuSnap");
    asset.fontPixelSize = 24;
    asset.color = Qt::white;
    return asset;
}
WatermarkPreset preset(const QString& id, const QString& name, const QString& assetId,
                       WatermarkLayout layout)
{
    WatermarkPreset result;
    result.id = id;
    result.name = name;
    result.assetId = assetId;
    result.layout = layout;
    result.opacity = 40;
    result.scale = 100;
    return result;
}
WatermarkPreferences onePresetPreferences()
{
    WatermarkPreferences preferences;
    preferences.assets = {textAsset(QStringLiteral("text"), QStringLiteral("文字"))};
    preferences.presets = {preset(QStringLiteral("p1"), QStringLiteral("预设一"),
                                  QStringLiteral("text"), WatermarkLayout::BottomRight)};
    preferences.defaultPresetId = QStringLiteral("p1");
    return preferences;
}
}
class WatermarkServiceTest final : public QObject
{
    Q_OBJECT
  private slots:
    void settingsRoundTripAndCorruption()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
        AppSettings settings(settingsPath);
        QString notice;
        const auto empty = settings.loadWatermarkPreferences(&notice);
        QVERIFY(empty.assets.isEmpty());
        QVERIFY(empty.presets.isEmpty());
        QVERIFY(notice.isEmpty());

        auto preferences = onePresetPreferences();
        preferences.autoEnabled = true;
        preferences.retainOriginal = true;
        QCOMPARE(settings.saveWatermarkPreferences(preferences), QString());
        const auto loaded = settings.loadWatermarkPreferences(&notice);
        QCOMPARE(loaded.assets.size(), 1);
        QCOMPARE(loaded.presets.size(), 1);
        QCOMPARE(loaded.presets[0].layout, WatermarkLayout::BottomRight);
        QCOMPARE(loaded.defaultPresetId, QStringLiteral("p1"));
        QVERIFY(loaded.autoEnabled);
        QVERIFY(loaded.retainOriginal);

        QSettings raw(settingsPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("watermark/preferences"), QStringLiteral("not-json"));
        raw.sync();
        const auto corrupted = settings.loadWatermarkPreferences(&notice);
        QVERIFY(corrupted.assets.isEmpty());
        QVERIFY(!notice.isEmpty());
    }
    void serviceDefaultsCurrentAndAuto()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings,
                                 QDir(directory.path()).filePath(QStringLiteral("assets")));
        QVERIFY(service.initialize());
        QVERIFY(service.currentPreset() == nullptr);
        QVERIFY(!service.setAutoEnabled(true).isEmpty());

        auto preferences = onePresetPreferences();
        preferences.presets.append(preset(QStringLiteral("p2"), QStringLiteral("预设二"),
                                          QStringLiteral("text"), WatermarkLayout::TopLeft));
        QCOMPARE(service.savePreferences(preferences), QString());
        QCOMPARE(service.currentPresetId(), QStringLiteral("p1"));
        QCOMPARE(service.currentPreset()->id, QStringLiteral("p1"));
        QCOMPARE(service.setCurrentPreset(QStringLiteral("p2")), QString());
        QCOMPARE(service.currentPresetId(), QStringLiteral("p2"));
        QVERIFY(!service.setCurrentPreset(QStringLiteral("missing")).isEmpty());

        QCOMPARE(service.setAutoEnabled(true), QString());
        QVERIFY(service.autoEnabled());
        WatermarkService restarted(settings,
                                   QDir(directory.path()).filePath(QStringLiteral("assets")));
        QVERIFY(restarted.initialize());
        QCOMPARE(restarted.currentPresetId(), QStringLiteral("p1"));
        QVERIFY(restarted.autoEnabled());
    }
    void imageImportAndAssetValidation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings,
                                 QDir(directory.path()).filePath(QStringLiteral("assets")));
        QVERIFY(service.initialize());
        const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.png"));
        QImage source(2000, 1000, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(20, 120, 220, 180));
        QVERIFY(source.save(sourcePath, "PNG"));
        QString managed;
        QString error;
        QCOMPARE(service.importImage(sourcePath, &managed, &error), QString());
        QVERIFY(!managed.isEmpty());
        QVERIFY(QFileInfo::exists(QDir(service.assetDirectory()).filePath(managed)));
        QImage imported = QImage(QDir(service.assetDirectory()).filePath(managed));
        QCOMPARE(imported.size(), QSize(1024, 512));

        WatermarkAsset image = textAsset(QStringLiteral("image"), QStringLiteral("图片"));
        image.kind = WatermarkKind::Image;
        image.imageFileName = managed;
        auto preferences = onePresetPreferences();
        preferences.assets.append(image);
        preferences.presets.append(preset(QStringLiteral("p2"), QStringLiteral("图片预设"),
                                          QStringLiteral("image"), WatermarkLayout::Center));
        QCOMPARE(service.savePreferences(preferences), QString());
        QVERIFY(!service.assetImage(QStringLiteral("image"), &error).isNull());
        QVERIFY(error.isEmpty());

        WatermarkAsset missing = image;
        missing.id = QStringLiteral("missing");
        missing.name = QStringLiteral("缺失");
        missing.imageFileName = QStringLiteral("missing.png");
        auto invalid = preferences;
        invalid.assets.append(missing);
        invalid.presets.append(preset(QStringLiteral("p3"), QStringLiteral("缺失预设"),
                                      QStringLiteral("missing"), WatermarkLayout::TopRight));
        QVERIFY(!service.savePreferences(invalid).isEmpty());
        QCOMPARE(service.removeAssetImage(managed), QString());
        QVERIFY(!QFileInfo::exists(QDir(service.assetDirectory()).filePath(managed)));
    }
    void invalidJsonAndLimits()
    {
        using namespace waibusnap;
        auto preferences = onePresetPreferences();
        preferences.defaultPresetId.clear();
        QVERIFY(!watermarkPreferencesError(preferences).isEmpty());
        preferences.defaultPresetId = QStringLiteral("p1");
        preferences.presets.clear();
        preferences.defaultPresetId.clear();
        QCOMPARE(watermarkPreferencesError(preferences), QString());
        for (int index = 0; index < watermarkMaximumPresets; ++index)
        {
            WatermarkPreset next =
                preset(QStringLiteral("p%1").arg(index), QStringLiteral("预设 %1").arg(index),
                       QStringLiteral("text"), WatermarkLayout::Center);
            preferences.presets.append(next);
        }
        preferences.defaultPresetId = preferences.presets.first().id;
        QCOMPARE(preferences.presets.size(), 10);
        QVERIFY(watermarkPreferencesError(preferences).isEmpty());
        preferences.presets.append(preset(QStringLiteral("p10"), QStringLiteral("超出"),
                                          QStringLiteral("text"), WatermarkLayout::Center));
        QVERIFY(watermarkPreferencesError(preferences).contains(QStringLiteral("最多")));
    }
};
QTEST_MAIN(WatermarkServiceTest)
#include "watermark_service_test.moc"
