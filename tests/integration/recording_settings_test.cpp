#include "app/application_controller.h"
#include "ui/settings_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSemaphore>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest>
#include <atomic>
using namespace waibusnap;
namespace
{
template <typename T> T* child(QObject& root, const char* name)
{
    auto* value = root.findChild<T*>(QLatin1String(name));
    if (!value)
        qFatal("缺少录制设置控件：%s", name);
    return value;
}
QByteArray bytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
struct Fixture
{
    QTemporaryDir temp;
    AppSettings settings{temp.filePath(QStringLiteral("settings.ini"))};
    RecordingSettingsActions actions;
    HistorySettingsActions history;
    QString chosen, quick;
    QStringList groups;
    int writes = 0;
    bool fail = false;
    Fixture()
    {
        actions.supported = true;
        actions.preferences.videoDirectory = temp.filePath(QStringLiteral("video"));
        actions.preferences.gifDirectory = temp.filePath(QStringLiteral("gif"));
        history.preferences.rootDirectory = temp.filePath(QStringLiteral("history"));
        actions.queryMicrophones = []
        {
            return QVector<MicrophoneDevice>{
                {QStringLiteral("mic-1"), QStringLiteral("测试麦克风"), true}};
        };
        actions.chooseDirectory = [this](QWidget*, const QString&) { return chosen; };
        actions.reservedDirectories = [this](const QString&, QString* q, QString*)
        {
            *q = quick;
            return groups;
        };
        actions.savePreferences = [this](const RecordingPreferences& p)
        {
            ++writes;
            return fail ? QStringLiteral("录制设置写入失败，原设置已保留。")
                        : settings.saveRecordingPreferences(p);
        };
    }
    std::unique_ptr<SettingsDialog> dialog(SaveSettingsActions save = {})
    {
        return std::make_unique<SettingsDialog>(
            QKeySequence(Qt::Key_F1), [](const auto&) { return QString(); }, QString(), nullptr,
            save, AutostartActions{[] { return AutostartState{}; }, [](bool) { return QString(); }},
            history, WatermarkSettingsActions{}, actions);
    }
    static RecordingSettingsPage* page(SettingsDialog& d)
    {
        return child<RecordingSettingsPage>(d, "settingsRecordingPage");
    }
    static void show(SettingsDialog& d)
    {
        d.show();
        child<QTabWidget>(d, "settingsTabs")->setCurrentWidget(page(d));
        QCoreApplication::processEvents();
    }
    static void save(SettingsDialog& d) { child<QPushButton>(d, "saveSettingsButton")->click(); }
};
}
class RecordingSettingsTest final : public QObject
{
    Q_OBJECT
  private slots:
    void defaultsAndState()
    {
        Fixture f;
        f.actions.preferences = {};
        auto d = f.dialog();
        Fixture::show(*d);
        auto* tabs = child<QTabWidget>(*d, "settingsTabs");
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->widget(3)->objectName(), QStringLiteral("settingsRecordingPage"));
        QCOMPARE(tabs->tabText(4), QStringLiteral("贴图"));
        QCOMPARE(tabs->tabText(7), QStringLiteral("水印"));
        const auto p = Fixture::page(*d)->collect();
        QCOMPARE(p.defaultOutput, RecordingOutput::Video);
        QCOMPARE(p.resolution, RecordingResolution::Native);
        QCOMPARE(p.videoFps, 30);
        QCOMPARE(p.quality, RecordingQuality::High);
        QCOMPARE(p.gifMaxEdge, 1280);
        QCOMPARE(p.gifFps, 12);
        QCOMPARE(p.gifMaxSeconds, 30);
        QCOMPARE(p.countdownSeconds, 3);
        QVERIFY(p.showCursor && p.systemAudio && !p.microphone);
        QCOMPARE(p.systemVolumePercent, 100);
        QCOMPARE(p.microphoneVolumePercent, 100);
        QVERIFY(p.videoDirectory.isEmpty() && p.gifDirectory.isEmpty());
        QVERIFY(child<QLineEdit>(*d, "recordingGifDirectory")->isReadOnly());
        QCOMPARE(child<QLineEdit>(*d, "recordingGifDirectory")->toolTip(),
                 defaultRecordingGifDirectory());
        if (defaultRecordingGifDirectory().startsWith(QDir::homePath() + QLatin1Char('/')))
            QVERIFY(child<QLineEdit>(*d, "recordingGifDirectory")
                        ->text()
                        .startsWith(QStringLiteral("~/")));
        QVERIFY(!child<QComboBox>(*d, "recordingMicrophoneDevice")->isEnabled());
        child<QCheckBox>(*d, "recordingMicrophone")->setChecked(true);
        QVERIFY(child<QComboBox>(*d, "recordingMicrophoneDevice")->isEnabled());
        child<QRadioButton>(*d, "recordingDefaultGif")->click();
        QVERIFY(!child<QGroupBox>(*d, "recordingAudioGroup")->isEnabled());
        QVERIFY(child<QLabel>(*d, "recordingAudioHelp")
                    ->text()
                    .contains(QStringLiteral("GIF 不含声音；切回视频后生效")));
        child<QRadioButton>(*d, "recordingDefaultVideo")->click();
        QVERIFY(child<QCheckBox>(*d, "recordingMicrophone")->isChecked());
        QVERIFY(child<QComboBox>(*d, "recordingMicrophoneDevice")->isEnabled());
        child<QComboBox>(*d, "recordingCountdown")->setCurrentIndex(0);
        QVERIFY(child<QLabel>(*d, "recordingCountdownHelp")->isVisible());
        QVERIFY(child<QLabel>(*d, "recordingGifEstimate")
                    ->text()
                    .contains(QStringLiteral("估算值，实际以编码结果为准")));
        const auto estimate = child<QLabel>(*d, "recordingGifEstimate")->text();
        child<QComboBox>(*d, "recordingGifFps")->setCurrentIndex(2);
        QVERIFY(estimate != child<QLabel>(*d, "recordingGifEstimate")->text());
        QVERIFY(!child<QLabel>(*d, "recordingGifEstimate")
                     ->text()
                     .contains(QStringLiteral("可直接发送")));
        auto* scroll = Fixture::page(*d)->findChild<QScrollArea*>();
        scroll->ensureWidgetVisible(child<QSpinBox>(*d, "recordingMicrophoneVolume"));
        QCoreApplication::processEvents();
        QVERIFY(!child<QSpinBox>(*d, "recordingMicrophoneVolume")->visibleRegion().isEmpty());
        QCOMPARE(f.writes, 0);
    }
    void roundTripAndDrafts()
    {
        Fixture f;
        auto d = f.dialog();
        Fixture::show(*d);
        child<QRadioButton>(*d, "recordingDefaultGif")->click();
        for (const auto* name :
             {"recordingResolution", "recordingVideoFps", "recordingQuality", "recordingGifMaxEdge",
              "recordingGifFps", "recordingGifMaxSeconds", "recordingCountdown"})
            child<QComboBox>(*d, name)->setCurrentIndex(1);
        child<QCheckBox>(*d, "recordingShowCursor")->setChecked(false);
        child<QCheckBox>(*d, "recordingSystemAudio")->setChecked(false);
        child<QCheckBox>(*d, "recordingMicrophone")->setChecked(true);
        child<QSlider>(*d, "recordingSystemVolumeSlider")->setValue(200);
        child<QSpinBox>(*d, "recordingMicrophoneVolume")->setValue(0);
        QCOMPARE(child<QSlider>(*d, "recordingMicrophoneVolumeSlider")->value(), 0);
        for (int page : {0, 7, 3, 1, 3})
            child<QTabWidget>(*d, "settingsTabs")->setCurrentIndex(page);
        QCOMPARE(f.writes, 0);
        QVERIFY(!QFileInfo::exists(f.temp.filePath(QStringLiteral("settings.ini"))));
        Fixture::save(*d);
        QCOMPARE(d->result(), int(QDialog::Accepted));
        QCOMPARE(f.writes, 1);
        const auto p = f.settings.loadRecordingPreferences();
        QCOMPARE(p.defaultOutput, RecordingOutput::Gif);
        QCOMPARE(p.resolution, RecordingResolution::P1080);
        QCOMPARE(p.videoFps, 60);
        QCOMPARE(p.quality, RecordingQuality::Medium);
        QCOMPARE(p.gifFps, 12);
        QCOMPARE(p.systemVolumePercent, 200);
        QCOMPARE(p.microphoneVolumePercent, 0);
        QVERIFY(!p.showCursor && !p.systemAudio && p.microphone);
        f.actions.preferences = f.settings.loadRecordingPreferences(nullptr, false);
        auto reopened = f.dialog();
        QCOMPARE(Fixture::page(*reopened)->collect().defaultOutput, RecordingOutput::Gif);
        QCOMPARE(Fixture::page(*reopened)->collect().systemVolumePercent, 200);
    }
    void parameterGroupsRoundTrip_data()
    {
        QTest::addColumn<bool>("gif");
        QTest::addColumn<int>("variant");
        QTest::newRow("video-1080p30") << false << 0;
        QTest::newRow("video-720p60") << false << 1;
        QTest::newRow("gif-960-10-15") << true << 0;
        QTest::newRow("gif-1280-15-30") << true << 1;
    }
    void parameterGroupsRoundTrip()
    {
        QFETCH(bool, gif);
        QFETCH(int, variant);
        Fixture f;
        const auto evidence = qEnvironmentVariable("WAIBUSNAP_RECORDING_SETTINGS_EVIDENCE");
        if (!evidence.isEmpty())
        {
            QVERIFY(QDir().mkpath(evidence));
            f.temp.setAutoRemove(false);
            f.actions.queryMicrophones = microphoneDevices;
        }
        auto d = f.dialog();
        Fixture::show(*d);
        child<QRadioButton>(*d, gif ? "recordingDefaultGif" : "recordingDefaultVideo")->click();
        child<QComboBox>(*d, "recordingResolution")->setCurrentIndex(variant == 0 ? 1 : 2);
        child<QComboBox>(*d, "recordingVideoFps")->setCurrentIndex(variant);
        child<QComboBox>(*d, "recordingQuality")->setCurrentIndex(variant == 0 ? 0 : 2);
        child<QComboBox>(*d, "recordingGifMaxEdge")->setCurrentIndex(variant);
        child<QComboBox>(*d, "recordingGifFps")->setCurrentIndex(variant == 0 ? 0 : 2);
        child<QComboBox>(*d, "recordingGifMaxSeconds")->setCurrentIndex(variant);
        child<QComboBox>(*d, "recordingCountdown")->setCurrentIndex(0);
        child<QCheckBox>(*d, "recordingShowCursor")->setChecked(variant == 0);
        child<QCheckBox>(*d, "recordingSystemAudio")->setChecked(false);
        child<QSpinBox>(*d, "recordingSystemVolume")->setValue(variant == 0 ? 0 : 200);
        if (!evidence.isEmpty())
        {
            QTest::qWait(200);
            const auto prefix =
                QDir(evidence).filePath(QString::fromLatin1(QTest::currentDataTag()));
            QVERIFY(d->grab().save(prefix + QStringLiteral("-top.png")));
            auto* scroll = Fixture::page(*d)->findChild<QScrollArea*>();
            scroll->ensureWidgetVisible(child<QLabel>(*d, "recordingAudioHelp"));
            QTest::qWait(50);
            QVERIFY(d->grab().save(prefix + QStringLiteral("-audio.png")));
        }
        Fixture::save(*d);
        QCOMPARE(d->result(), int(QDialog::Accepted));
        const auto saved = f.settings.loadRecordingPreferences();
        QCOMPARE(saved.defaultOutput, gif ? RecordingOutput::Gif : RecordingOutput::Video);
        QCOMPARE(saved.videoFps, variant == 0 ? 30 : 60);
        QCOMPARE(saved.resolution,
                 variant == 0 ? RecordingResolution::P1080 : RecordingResolution::P720);
        QCOMPARE(saved.gifMaxEdge, variant == 0 ? 960 : 1280);
        QCOMPARE(saved.gifFps, variant == 0 ? 10 : 15);
        QCOMPARE(saved.gifMaxSeconds, variant == 0 ? 15 : 30);
        if (!evidence.isEmpty())
            QVERIFY(
                QFile::copy(f.temp.filePath(QStringLiteral("settings.ini")),
                            QDir(evidence).filePath(QString::fromLatin1(QTest::currentDataTag()) +
                                                    QStringLiteral(".ini"))));
    }
    void cancelAndEscape_data()
    {
        QTest::addColumn<bool>("escape");
        QTest::newRow("cancel") << false;
        QTest::newRow("escape") << true;
    }
    void cancelAndEscape()
    {
        QFETCH(bool, escape);
        Fixture f;
        QVERIFY(f.settings.saveRecordingPreferences(f.actions.preferences).isEmpty());
        const auto before = bytes(f.temp.filePath(QStringLiteral("settings.ini")));
        auto d = f.dialog();
        Fixture::show(*d);
        child<QRadioButton>(*d, "recordingDefaultGif")->click();
        if (escape)
            QTest::keyClick(d.get(), Qt::Key_Escape);
        else
            child<QPushButton>(*d, "cancelSettingsButton")->click();
        QCOMPARE(d->result(), int(QDialog::Rejected));
        QCOMPARE(f.writes, 0);
        QCOMPARE(bytes(f.temp.filePath(QStringLiteral("settings.ini"))), before);
    }
    void saveFailureAndRetry()
    {
        Fixture f;
        QVERIFY(f.settings.saveRecordingPreferences(f.actions.preferences).isEmpty());
        const auto before = bytes(f.temp.filePath(QStringLiteral("settings.ini")));
        f.fail = true;
        auto d = f.dialog();
        Fixture::show(*d);
        child<QRadioButton>(*d, "recordingDefaultGif")->click();
        Fixture::save(*d);
        QVERIFY(d->isVisible());
        QVERIFY(child<QLabel>(*d, "settingsStatus")->text().contains(QStringLiteral("失败")));
        QCOMPARE(bytes(f.temp.filePath(QStringLiteral("settings.ini"))), before);
        f.fail = false;
        Fixture::save(*d);
        QCOMPARE(d->result(), int(QDialog::Accepted));
        QCOMPARE(f.settings.loadRecordingPreferences().defaultOutput, RecordingOutput::Gif);
    }
    void earlierSaveFailureDoesNotCommitRecording()
    {
        Fixture f;
        SaveSettingsActions save;
        save.savePreferences = [](const SavePreferences&) { return QStringLiteral("保存页失败"); };
        auto d = f.dialog(save);
        Fixture::show(*d);
        child<QRadioButton>(*d, "recordingDefaultGif")->click();
        Fixture::save(*d);
        QVERIFY(d->isVisible());
        QCOMPARE(f.writes, 0);
    }
    void directories_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<bool>("accepted");
        for (const auto* name :
             {"history-child", "group-child", "prefix-sibling", "create-missing"})
            QTest::newRow(name) << QString::fromLatin1(name) << true;
        for (const auto* name :
             {"history-equal", "history-parent", "group-equal", "group-parent", "quick-equal",
              "quick-child", "quick-parent", "outputs-equal", "outputs-child", "outputs-parent",
              "relative", "nul", "file-parent", "not-writable"})
            QTest::newRow(name) << QString::fromLatin1(name) << false;
    }
    void directories()
    {
        QFETCH(QString, scenario);
        QFETCH(bool, accepted);
        Fixture f;
        const auto base = f.temp.path();
        const auto path = [&](const char* name) { return f.temp.filePath(QLatin1String(name)); };
        f.groups = {path("group")};
        f.quick = path("quick");
        QString video = path("video");
        if (scenario == QStringLiteral("history-child"))
            video = path("history/GIF/deep");
        if (scenario == QStringLiteral("group-child"))
            video = path("group/media");
        if (scenario == QStringLiteral("prefix-sibling"))
            video = path("group-sibling");
        if (scenario == QStringLiteral("create-missing"))
            video = path("missing/deep/video");
        if (scenario == QStringLiteral("history-equal"))
            video = path("history");
        if (scenario == QStringLiteral("history-parent"))
            video = base;
        if (scenario == QStringLiteral("group-equal"))
            video = path("group");
        if (scenario == QStringLiteral("group-parent"))
        {
            f.groups = {path("parent/group")};
            video = path("parent");
        }
        if (scenario == QStringLiteral("quick-equal"))
            video = path("quick");
        if (scenario == QStringLiteral("quick-child"))
            video = path("quick/video");
        if (scenario == QStringLiteral("quick-parent"))
        {
            f.quick = path("video/quick");
        }
        if (scenario == QStringLiteral("outputs-equal"))
            video = path("gif");
        if (scenario == QStringLiteral("outputs-child"))
            video = path("gif/video");
        if (scenario == QStringLiteral("outputs-parent"))
            f.actions.preferences.gifDirectory = path("video/gif");
        if (scenario == QStringLiteral("relative"))
            video = QStringLiteral("relative/path");
        if (scenario == QStringLiteral("nul"))
            video += QChar::Null;
        if (scenario == QStringLiteral("file-parent"))
        {
            QFile file(path("file"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            video = path("file/video");
        }
        if (scenario == QStringLiteral("not-writable"))
        {
            QVERIFY(QDir().mkpath(video));
            QVERIFY(QFile::setPermissions(video, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
            if (QFileInfo(video).isWritable())
            {
                QFile::setPermissions(video, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                 QFileDevice::ExeOwner);
                QSKIP("当前平台权限模型不提供只读目录；其余创建失败用例仍执行。");
            }
        }
        f.actions.preferences.videoDirectory = video;
        auto d = f.dialog();
        Fixture::show(*d);
        Fixture::save(*d);
        if (scenario == QStringLiteral("not-writable"))
            QVERIFY(QFile::setPermissions(video, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                     QFileDevice::ExeOwner));
        QCOMPARE(f.writes, accepted ? 1 : 0);
        if (accepted)
        {
            QCOMPARE(d->result(), int(QDialog::Accepted));
            QVERIFY(QFileInfo(video).isDir());
        }
        else
        {
            QVERIFY(d->isVisible());
            auto* error = child<QLabel>(*d, "recordingDirectoryError");
            QVERIFY(error->isVisible() && !error->text().isEmpty());
            QVERIFY(error->styleSheet().contains(QStringLiteral("#d63c36")));
            QVERIFY(!QFileInfo::exists(path("settings.ini")));
        }
    }
    void directoryChooseRestoreAndDefaultSentinel()
    {
        Fixture f;
        auto d = f.dialog();
        Fixture::show(*d);
        f.chosen = f.temp.filePath(QStringLiteral("新 GIF"));
        child<QPushButton>(*d, "recordingChooseGifDirectory")->click();
        QCOMPARE(Fixture::page(*d)->collect().gifDirectory, f.chosen);
        f.chosen.clear();
        child<QPushButton>(*d, "recordingChooseGifDirectory")->click();
        QCOMPARE(Fixture::page(*d)->collect().gifDirectory,
                 f.temp.filePath(QStringLiteral("新 GIF")));
        child<QPushButton>(*d, "recordingRestoreGifDirectory")->click();
        child<QPushButton>(*d, "recordingRestoreVideoDirectory")->click();
        const auto draft = Fixture::page(*d)->collect();
        QVERIFY(draft.videoDirectory.isEmpty() && draft.gifDirectory.isEmpty());
        QVERIFY(f.settings.saveRecordingPreferences(draft).isEmpty());
        QSettings ini(f.temp.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        QVERIFY(ini.value(QStringLiteral("recording/videoDirectory")).toString().isEmpty());
        QVERIFY(ini.value(QStringLiteral("recording/gifDirectory")).toString().isEmpty());
        QVERIFY(f.settings.loadRecordingPreferences(nullptr, false).gifDirectory.isEmpty());
        QCOMPARE(f.settings.loadRecordingPreferences().gifDirectory,
                 defaultRecordingGifDirectory());
    }
    void microphoneRefreshIsAsyncAndKeepsMissingSelection()
    {
        Fixture f;
        f.actions.preferences.microphone = true;
        f.actions.preferences.microphoneDeviceId = QStringLiteral("missing");
        const auto onGui = std::make_shared<std::atomic_bool>(true);
        f.actions.queryMicrophones = [onGui]
        {
            *onGui = QThread::currentThread() == qApp->thread();
            return QVector<MicrophoneDevice>{
                {QStringLiteral("mic-1"), QStringLiteral("麦克风"), true}};
        };
        auto d = f.dialog();
        Fixture::show(*d);
        auto* devices = child<QComboBox>(*d, "recordingMicrophoneDevice");
        QTRY_VERIFY(devices->findData(QStringLiteral("mic-1")) >= 0);
        QVERIFY(!onGui->load());
        QCOMPARE(devices->currentData().toString(), QStringLiteral("missing"));
        QVERIFY(devices->currentText().contains(QStringLiteral("不可用")));
        devices->setCurrentIndex(devices->findData(QStringLiteral("mic-1")));
        child<QPushButton>(*d, "recordingRefreshMicrophones")->click();
        QTRY_VERIFY(child<QPushButton>(*d, "recordingRefreshMicrophones")->isEnabled());
        QCOMPARE(Fixture::page(*d)->collect().microphoneDeviceId, QStringLiteral("mic-1"));
        QCOMPARE(f.writes, 0);
    }
    void unsupportedPageIsVisibleAndNeverWrites()
    {
        Fixture f;
        f.actions.supported = false;
        auto d = f.dialog();
        Fixture::show(*d);
        QVERIFY(child<QLabel>(*d, "recordingUnsupportedNotice")->isVisible());
        for (auto* widget : Fixture::page(*d)->findChildren<QWidget*>())
            if (qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractButton*>(widget) ||
                qobject_cast<QSpinBox*>(widget) || qobject_cast<QLineEdit*>(widget) ||
                qobject_cast<QSlider*>(widget))
                QVERIFY2(!widget->isEnabled(), qPrintable(widget->objectName()));
        Fixture::save(*d);
        QCOMPARE(f.writes, 0);
        QCOMPARE(d->result(), int(QDialog::Accepted));
        QCOMPARE(RecordingSettingsActions{}.supported, recordingCaptureSupported());
    }
    void closingDuringDeviceRefreshIsSafe()
    {
        Fixture f;
        const auto gate = std::make_shared<QSemaphore>();
        const auto entered = std::make_shared<std::atomic_bool>(false);
        f.actions.queryMicrophones = [gate, entered]
        {
            *entered = true;
            gate->acquire();
            return QVector<MicrophoneDevice>{};
        };
        auto d = f.dialog();
        Fixture::show(*d);
        QTRY_VERIFY(entered->load());
        // 设备枚举仍在后台等待时，窗口可取消并析构；迟到结果只能被丢弃。
        child<QPushButton>(*d, "cancelSettingsButton")->click();
        d.reset();
        gate->release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        QCOMPARE(f.writes, 0);
    }
    void cleanupTestCase() { QThreadPool::globalInstance()->waitForDone(); }
};
QTEST_MAIN(RecordingSettingsTest)
#include "recording_settings_test.moc"
