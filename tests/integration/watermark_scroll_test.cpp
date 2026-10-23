#include "app/app_settings.h"
#include "app/watermark_service.h"
#include "ui/scroll_result_window.h"
#include <QDir>
#include <QImage>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
namespace
{
using namespace waibusnap;
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
    result.autoEnabled = true;
    return result;
}
QString savedFile(const QString& directory)
{
    const auto files =
        QDir(directory).entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Time);
    return files.isEmpty() ? QString() : QDir(directory).filePath(files.first());
}
}
class WatermarkScrollTest final : public QObject
{
    Q_OBJECT
  private slots:
    void longImageAutoWatermarkCanBeToggled()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString outputDirectory = directory.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(outputDirectory));
        AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings, directory.filePath(QStringLiteral("assets")));
        QVERIFY(service.initialize());
        const QString sourcePath = directory.filePath(QStringLiteral("blue.png"));
        QImage source(40, 20, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(20, 80, 240));
        QVERIFY(source.save(sourcePath, "PNG"));
        QString managed;
        QVERIFY(service.importImage(sourcePath, &managed).isEmpty());
        QVERIFY(service.savePreferences(preferences(managed)).isEmpty());

        ScrollResultActions actions;
        actions.watermarks = &service;
        actions.watermarkEnabled = true;
        actions.loadSavePreferences = [&]
        { return SavePreferences{outputDirectory, ImageFormat::Png}; };
        QImage base(320, 200, QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::white);
        ScrollResultWindow window(base, {}, actions);
        window.show();
        QTest::qWait(10);
        auto* toggle =
            window.findChild<QPushButton*>(QStringLiteral("scrollResultWatermarkButton"));
        QVERIFY(toggle);
        QVERIFY(toggle->isVisible());
        QVERIFY(toggle->isChecked());
        QVERIFY(window.findChild<QPushButton*>(QStringLiteral("scrollResultSaveButton")));
        window.findChild<QPushButton*>(QStringLiteral("scrollResultSaveButton"))->click();
        QCoreApplication::processEvents();
        const QString watermarked = savedFile(outputDirectory);
        QVERIFY(!watermarked.isEmpty());
        QVERIFY(bluePixels(QImage(watermarked)) > 100);

        QFile::remove(watermarked);
        toggle->click();
        QVERIFY(!toggle->isChecked());
        window.findChild<QPushButton*>(QStringLiteral("scrollResultSaveButton"))->click();
        QCoreApplication::processEvents();
        const QString plain = savedFile(outputDirectory);
        QVERIFY(!plain.isEmpty());
        QCOMPARE(bluePixels(QImage(plain)), 0);
        window.closeWithoutConfirmation();
    }
};
QTEST_MAIN(WatermarkScrollTest)
#include "watermark_scroll_test.moc"
