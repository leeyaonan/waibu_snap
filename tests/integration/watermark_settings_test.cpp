#include "app/app_settings.h"
#include "app/application_controller.h"
#include "app/hotkey_rules.h"
#include "app/watermark_service.h"
#include "ui/settings_dialog.h"
#include "ui/watermark_settings_page.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
namespace
{
using namespace waibusnap;
WatermarkPreferences samplePreferences()
{
    WatermarkPreferences preferences;
    WatermarkAsset asset;
    asset.id = QStringLiteral("text");
    asset.name = QStringLiteral("文字水印");
    asset.kind = WatermarkKind::Text;
    asset.text = QStringLiteral("WaibuSnap");
    asset.fontPixelSize = 24;
    preferences.assets = {asset};
    WatermarkPreset preset;
    preset.id = QStringLiteral("preset");
    preset.name = QStringLiteral("右下署名");
    preset.assetId = asset.id;
    preset.layout = WatermarkLayout::BottomRight;
    preset.opacity = 35;
    preset.scale = 100;
    preferences.presets = {preset};
    preferences.defaultPresetId = preset.id;
    preferences.retainOriginal = true;
    return preferences;
}
AutostartActions startup()
{
    return {[] { return AutostartState{}; }, [](bool) { return QString(); }};
}
}
class WatermarkSettingsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void pageAndSaveRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings, directory.filePath(QStringLiteral("watermark-assets")));
        QVERIFY(service.initialize());
        QVERIFY(service.savePreferences(samplePreferences()).isEmpty());

        WatermarkSettingsActions watermark;
        watermark.preferences = service.preferences();
        watermark.historyEnabled = true;
        watermark.loadManagedImage = [&service](const QString& name, QString* error)
        { return service.loadManagedImage(name, error); };
        watermark.removeAssetImage = [&service](const QString& name)
        { return service.removeAssetImage(name); };
        watermark.savePreferences = [&service](const WatermarkPreferences& preferences)
        { return service.savePreferences(preferences); };
        HistorySettingsActions history;
        history.enabled = true;
        history.preferences.enabled = true;
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), history, watermark);
        auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->widget(7)->objectName(), QStringLiteral("settingsWatermarkPage"));
        tabs->setCurrentIndex(7);
        auto* assets = dialog.findChild<QListWidget*>(QStringLiteral("watermarkAssetsList"));
        auto* presets = dialog.findChild<QListWidget*>(QStringLiteral("watermarkPresetsList"));
        QVERIFY(assets);
        QVERIFY(presets);
        QCOMPARE(assets->count(), 1);
        QCOMPARE(presets->count(), 1);
        auto* autoCheck = dialog.findChild<QCheckBox*>(QStringLiteral("watermarkAutoCheck"));
        auto* retain = dialog.findChild<QCheckBox*>(QStringLiteral("watermarkRetainOriginalCheck"));
        QVERIFY(autoCheck);
        QVERIFY(retain);
        QVERIFY(autoCheck->isEnabled());
        QVERIFY(retain->isEnabled());
        QVERIFY(retain->isChecked());
        autoCheck->setChecked(true);
        dialog.findChild<QPushButton*>(QStringLiteral("saveSettingsButton"))->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(service.preferences().autoEnabled);
        QVERIFY(service.preferences().retainOriginal);
        QCOMPARE(service.preferences().presets.size(), 1);
    }
    void addAssetAndPresetThroughDialogs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings, directory.filePath(QStringLiteral("watermark-assets")));
        QVERIFY(service.initialize());
        QVERIFY(service.savePreferences(samplePreferences()).isEmpty());
        WatermarkSettingsActions watermark;
        watermark.preferences = service.preferences();
        watermark.historyEnabled = true;
        watermark.loadManagedImage = [&service](const QString& name, QString* error)
        { return service.loadManagedImage(name, error); };
        watermark.savePreferences = [&service](const WatermarkPreferences& preferences)
        { return service.savePreferences(preferences); };
        WatermarkSettingsPage page(watermark);
        page.show();
        QApplication::processEvents();
        auto* assets = page.findChild<QListWidget*>(QStringLiteral("watermarkAssetsList"));
        auto* presets = page.findChild<QListWidget*>(QStringLiteral("watermarkPresetsList"));
        QVERIFY(assets && presets);
        QCOMPARE(assets->count(), 1);
        QCOMPARE(presets->count(), 1);
        bool filled = false;
        QTimer::singleShot(
            0,
            [&filled]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("watermarkAssetName"));
                auto* text =
                    dialog->findChild<QPlainTextEdit*>(QStringLiteral("watermarkAssetText"));
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!name || !text || !buttons)
                    return;
                name->setText(QStringLiteral("第二套文字"));
                text->setPlainText(QStringLiteral("Second"));
                filled = true;
                buttons->button(QDialogButtonBox::Ok)->click();
            });
        page.findChild<QPushButton*>(QStringLiteral("watermarkAddTextButton"))->click();
        QVERIFY(filled);
        QCOMPARE(assets->count(), 2);
        filled = false;
        QTimer::singleShot(
            0,
            [&filled]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("watermarkPresetName"));
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!name || !buttons)
                    return;
                name->setText(QStringLiteral("第二套预设"));
                filled = true;
                buttons->button(QDialogButtonBox::Ok)->click();
            });
        page.findChild<QPushButton*>(QStringLiteral("watermarkAddPresetButton"))->click();
        QVERIFY(filled);
        QCOMPARE(presets->count(), 2);
        QCOMPARE(page.collect().presets.size(), 2);
        QCOMPARE(page.collect().defaultPresetId, QStringLiteral("preset"));
    }
    void emptyStateAddAssetEnablesPreset()
    {
        WatermarkSettingsActions actions;
        actions.historyEnabled = true;
        WatermarkSettingsPage page(actions);
        page.show();
        QApplication::processEvents();
        auto* addPreset = page.findChild<QPushButton*>(QStringLiteral("watermarkAddPresetButton"));
        auto* assets = page.findChild<QListWidget*>(QStringLiteral("watermarkAssetsList"));
        auto* presets = page.findChild<QListWidget*>(QStringLiteral("watermarkPresetsList"));
        auto* preview = page.findChild<QLabel*>(QStringLiteral("watermarkAssetPreview"));
        QVERIFY(addPreset && assets && presets && preview);
        QVERIFY(!addPreset->isEnabled());
        bool filled = false;
        QTimer::singleShot(
            0,
            [&filled]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("watermarkAssetName"));
                auto* text =
                    dialog->findChild<QPlainTextEdit*>(QStringLiteral("watermarkAssetText"));
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!name || !text || !buttons)
                    return;
                name->setText(QStringLiteral("首个资产"));
                text->setPlainText(QStringLiteral("WaibuSnap"));
                filled = true;
                buttons->button(QDialogButtonBox::Ok)->click();
            });
        page.findChild<QPushButton*>(QStringLiteral("watermarkAddTextButton"))->click();
        QVERIFY(filled);
        QCOMPARE(assets->count(), 1);
        QVERIFY(addPreset->isEnabled());
        QVERIFY(!preview->pixmap().isNull());
        filled = false;
        QTimer::singleShot(
            0,
            [&filled]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("watermarkPresetName"));
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!name || !buttons)
                    return;
                name->setText(QStringLiteral("首个预设"));
                filled = true;
                buttons->button(QDialogButtonBox::Ok)->click();
            });
        addPreset->click();
        QVERIFY(filled);
        QCOMPARE(presets->count(), 1);
        QVERIFY(page.findChild<QCheckBox*>(QStringLiteral("watermarkAutoCheck"))->isEnabled());
    }
    void controllerTrayPresetAndAuto()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath = directory.filePath(QStringLiteral("settings.ini"));
        AppSettings settings(settingsPath);
        WatermarkService seed(settings, directory.filePath(QStringLiteral("watermark-assets")));
        QVERIFY(seed.initialize());
        auto preferences = samplePreferences();
        WatermarkPreset second;
        second.id = QStringLiteral("preset-2");
        second.name = QStringLiteral("第二预设");
        second.assetId = QStringLiteral("text");
        second.layout = WatermarkLayout::TopLeft;
        second.opacity = 50;
        second.scale = 80;
        preferences.presets.append(second);
        QVERIFY(seed.savePreferences(preferences).isEmpty());

        RunOptions options;
        options.testMode = true;
        options.settingsFile = settingsPath;
        options.historyRoot = directory.filePath(QStringLiteral("history"));
        RecordingPreferences recording;
        recording.videoDirectory = directory.filePath(QStringLiteral("video"));
        recording.gifDirectory = directory.filePath(QStringLiteral("gif"));
        QVERIFY(settings.saveRecordingPreferences(recording).isEmpty());
        ApplicationController controller(*qApp, options);
        auto* autoAction =
            controller.menu()->findChild<QAction*>(QStringLiteral("watermarkAutoAction"));
        QVERIFY(autoAction);
        QVERIFY(autoAction->isEnabled());
        QVERIFY(!autoAction->isChecked());
        autoAction->trigger();
        QCoreApplication::processEvents();
        QVERIFY(AppSettings(settingsPath).loadWatermarkPreferences().autoEnabled);

        auto* presetMenu =
            controller.menu()->findChild<QMenu*>(QStringLiteral("watermarkPresetMenu"));
        QVERIFY(presetMenu);
        const auto actions = presetMenu->actions();
        QCOMPARE(actions.size(), 2);
        QAction* secondAction = nullptr;
        for (auto* action : actions)
            if (action->text() == QStringLiteral("第二预设"))
                secondAction = action;
        QVERIFY(secondAction);
        QVERIFY(!secondAction->isChecked());
        secondAction->trigger();
        QCoreApplication::processEvents();
        presetMenu = controller.menu()->findChild<QMenu*>(QStringLiteral("watermarkPresetMenu"));
        QVERIFY(presetMenu);
        for (auto* action : presetMenu->actions())
            if (action->text() == QStringLiteral("第二预设"))
                QVERIFY(action->isChecked());
    }
    void historyOffDisablesRetain()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
        WatermarkService service(settings, directory.filePath(QStringLiteral("watermark-assets")));
        QVERIFY(service.initialize());
        QVERIFY(service.savePreferences(samplePreferences()).isEmpty());
        WatermarkSettingsActions watermark;
        watermark.preferences = service.preferences();
        watermark.historyEnabled = false;
        watermark.savePreferences = [&service](const WatermarkPreferences& preferences)
        { return service.savePreferences(preferences); };
        SettingsDialog dialog(
            defaultScreenshotHotkey(), [](const auto&) { return QString(); }, {}, nullptr, {},
            startup(), {}, watermark);
        auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
        tabs->setCurrentIndex(7);
        auto* retain = dialog.findChild<QCheckBox*>(QStringLiteral("watermarkRetainOriginalCheck"));
        QVERIFY(retain);
        QVERIFY(!retain->isEnabled());
        QVERIFY(!retain->isChecked());
    }
};
QTEST_MAIN(WatermarkSettingsTest)
#include "watermark_settings_test.moc"
