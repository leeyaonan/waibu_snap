#include "app/application_controller.h"
#include "app/hotkey_rules.h"
#include "app/quick_gunner_save_queue.h"
#include "core/sticker_geometry.h"
#include "core/window_snapping.h"
#include "interfaces/overlay_window_behavior.h"
#include "interfaces/tray_icon.h"
#include "output/watermark_renderer.h"
#include "session/monotonic_clock.h"
#include "ui/group_settings_dialog.h"
#include "ui/quick_gunner_flash_window.h"
#include "ui/settings_dialog.h"
#include <QAction>
#include <QClipboard>
#include <QCursor>
#include <QDebug>
#include <QFileDialog>
#include <QFontMetrics>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
namespace waibusnap
{
ApplicationController::ApplicationController(
    QApplication& application, RunOptions options, StickerActions stickerActions,
    ClipboardActions clipboardActions, AutostartActions autostartActions,
    HistoryWindowActions historyActions, std::shared_ptr<FileManager> fileManager,
    std::unique_ptr<GlobalHotkey> hotkey, RecordingDependencies recordingDependencies)
    : application_(application), options_(std::move(options)),
      hotkey_(hotkey ? std::move(hotkey) : createGlobalHotkey()), settings_(options_.settingsFile),
      history_(settings_, options_.historyRoot), watermarks_(settings_),
      hotkeySettings_(settings_, *hotkey_, [this] { trigger(QStringLiteral("hotkey")); }),
      quickGunnerHotkey_(std::make_unique<QuickGunnerHotkey>(settings_, *hotkey_,
                                                             [this] { quickGunnerShutter(); })),
      displays_(createDisplayTopology()), capture_(createCaptureProvider()),
      windows_(createWindowEnumerator()),
      stickers_(nullptr,
                [this, &stickerActions]
                {
                    stickerActions.loadSavePreferences = [this]
                    { return settings_.loadSavePreferences(); };
                    stickerActions.loadCloseReminder = [this]
                    { return settings_.loadStickerCloseReminder(); };
                    stickerActions.saveCloseReminder = [this](bool enabled)
                    { return settings_.saveStickerCloseReminder(enabled); };
                    stickerActions.loadDoubleClickClose = [this]
                    { return settings_.loadStickerDoubleClickClose(); };
                    stickerActions.saveDoubleClickClose = [this](bool enabled)
                    { return settings_.saveStickerDoubleClickClose(enabled); };
                    return std::move(stickerActions);
                }()),
      clipboardActions_(std::move(clipboardActions)),
      autostartActions_(std::move(autostartActions)), historyActions_(std::move(historyActions)),
      fileManager_(fileManager ? std::move(fileManager) : createFileManager())
{
    const std::array<std::function<void()>, 3> handlers = {
        [this] { navigateHistory(-1); }, [this] { navigateHistory(1); }, [this] { openHistory(); }};
    for (size_t i = 0; i < historyHotkeys_.size(); ++i)
        historyHotkeys_[i] = std::make_unique<HotkeySettings>(settings_, *hotkey_, handlers[i],
                                                              hotkeyActions[i + 1]);
    connect(&history_, &HistoryManager::cleanupFinished, this,
            [this]
            {
                const auto result = history_.lastCleanup();
                // 构造时托盘尚未显示，启动报告在 start() 发布。
                if (!options_.testMode && !options_.smokeTest && tray_.isVisible() &&
                    (result.deletedCount || result.protectedOverLimit))
                    tray_.showMessage(QStringLiteral("截图历史自动清理"), result.message(),
                                      result.error.isEmpty() ? QSystemTrayIcon::Information
                                                             : QSystemTrayIcon::Warning);
            });
    const auto historyPreferences = settings_.loadHistoryPreferences();
    historyEnabled_ = options_.historyEnabledOverride.value_or(historyPreferences.enabled ||
                                                               !options_.historyRoot.isEmpty());
    if (historyEnabled_)
    {
        const auto initialized = history_.initialize();
        if (!initialized.success)
            qWarning().noquote() << "截图历史初始化失败：" << initialized.error;
    }
    QString watermarkNotice;
    watermarks_.initialize(&watermarkNotice);
    if (!watermarkNotice.isEmpty())
        qWarning().noquote() << "水印设置：" << watermarkNotice;
    QuickGunnerDependencies quickGunnerDependencies;
    quickGunnerDependencies.watermark = [this](const QImage& image)
    { return quickGunnerWatermark(image); };
    quickGunner_ = std::make_unique<QuickGunnerController>(settings_, *displays_, *capture_,
                                                           RegionPickerFactory{},
                                                           std::move(quickGunnerDependencies));
    connectQuickGunner();
    if (!historyActions_.copyImage)
        historyActions_.copyImage = copyImageToClipboard;
    if (!historyActions_.pinImage)
        historyActions_.pinImage = [this](const QImage& image) { return pinHistoryImage(image); };
    if (!historyActions_.reveal)
        historyActions_.reveal = [this](const QString& path)
        {
            const auto result = fileManager_->revealInFolder(path);
            return HistoryStatus{result.success, result.error};
        };
    if (!historyActions_.trash)
        historyActions_.trash = [this](const QString& path)
        {
            const auto result = fileManager_->moveToTrash(path);
            return HistoryStatus{result.success, result.error};
        };
    quickGunnerSource_ = std::make_unique<QuickGunnerResourceSource>(
        [this] { return quickGunner_->preferences().directory; }, settings_.quickGunnerStarsPath(),
        historyActions_);
    recordingMediaProbe_ = createRecordingMediaProbe();
    recordingVideoSource_ = std::make_unique<RecordingResourceSource>(
        RecordingOutput::Video,
        [this] { return settings_.loadRecordingPreferences().videoDirectory; },
        settings_.recordingVideoStarsPath(), settings_.recordingVideoMetaPath(),
        recordingMediaProbe_, historyActions_);
    recordingGifSource_ = std::make_unique<RecordingResourceSource>(
        RecordingOutput::Gif, [this] { return settings_.loadRecordingPreferences().gifDirectory; },
        settings_.recordingGifStarsPath(), settings_.recordingGifMetaPath(), recordingMediaProbe_,
        historyActions_);
    if (!autostartActions_.query || !autostartActions_.setEnabled)
    {
        const std::shared_ptr<Autostart> autostart = createAutostart();
        if (!autostartActions_.query)
            autostartActions_.query = [autostart] { return autostart->query(); };
        if (!autostartActions_.setEnabled)
            autostartActions_.setEnabled = [autostart](bool enabled)
            { return autostart->setEnabled(enabled); };
    }
    if (!clipboardActions_.loadImage)
        clipboardActions_.loadImage = [] { return QGuiApplication::clipboard()->image(); };
    scrollFactory_ = [](RegionCaptureRequest request, QRect bar, QObject* parent)
    { return new ScrollCaptureSession(std::move(request), bar, parent); };
    overlayFactory_ =
        [](CaptureFrame frame, OverlayActions actions, QVector<QRect> windows, QString error)
    {
        return new SelectionOverlay(std::move(frame), std::move(actions), std::move(windows),
                                    std::move(error));
    };
    for (size_t i = 0; i < recordingHotkeys_.size(); ++i)
        recordingHotkeys_[i] = std::make_unique<HotkeySettings>(
            settings_, *hotkey_,
            i == 0 ? std::function<void()>([this] { toggleRecordingStartStop(); })
                   : std::function<void()>([this] { toggleRecordingPause(); }),
            hotkeyActions[i + 5]);
    if (!recordingDependencies.cursorDisplay)
        recordingDependencies.cursorDisplay = [this] { return displays_->cursorDisplay(); };
    if (!recordingDependencies.stillMatches)
        recordingDependencies.stillMatches = [this](const DisplayTarget& target)
        { return displays_->stillMatches(target); };
    if (!recordingDependencies.freezeFrame)
        recordingDependencies.freezeFrame =
            [this](const DisplayTarget& target, std::function<void(CaptureResult)> done)
        { capture_->capture(target, std::move(done)); };
    if (!recordingDependencies.toastPresenter)
        recordingDependencies.toastPresenter = [this](const RecordingEvent& event, bool success)
        {
            recordingToast_ =
                std::make_unique<RecordingToast>(event, success,
                                                 [this](const QString& path)
                                                 {
                                                     const auto result =
                                                         fileManager_->revealInFolder(path);
                                                     if (!result.success)
                                                         showRecordingNotice(result.error);
                                                 });
            recordingToast_->present();
        };
    recording_ = std::make_unique<RecordingController>(std::move(recordingDependencies));
    application_.setQuitOnLastWindowClosed(false);
    connect(&application_, &QCoreApplication::aboutToQuit, this, &ApplicationController::cleanup);
    captureTimeout_.setSingleShot(true);
    connect(&captureTimeout_, &QTimer::timeout, this,
            [this]
            {
                if (active_ && !scrollFlowActive_)
                    fail(6, QStringLiteral("截图会话超时，请重新尝试。"));
            });
    sessionWatchdog_.setInterval(5000);
    connect(&sessionWatchdog_, &QTimer::timeout, this,
            &ApplicationController::checkSessionWatchdog);
    menu_.addAction(QStringLiteral("截图"), this, [this] { trigger(QStringLiteral("tray")); })
        ->setObjectName(QStringLiteral("screenshotAction"));
    scrollAction_ = menu_.addAction(QStringLiteral("截长图"), this,
                                    [this] { trigger(QStringLiteral("tray-scroll")); });
    scrollAction_->setObjectName(QStringLiteral("scrollCaptureAction"));
    scrollAction_->setToolTip(regionCaptureSupported()
                                  ? QStringLiteral("框选区域后点击长图开始滚动采集")
                                  : QStringLiteral("截长图暂不支持当前平台（Windows 后补）。"));
    scrollAction_->setEnabled(regionCaptureSupported());
    quickGunnerMenu_ = menu_.addMenu(QStringLiteral("快枪手"));
    quickGunnerMenu_->setObjectName(QStringLiteral("quickGunnerMenu"));
    quickGunnerMenu_->setToolTipsVisible(true);
    quickGunnerMenu_->menuAction()->setObjectName(QStringLiteral("quickGunnerMenuAction"));
    quickGunnerModeAction_ = quickGunnerMenu_->addAction(QStringLiteral("快枪手模式"));
    quickGunnerModeAction_->setObjectName(QStringLiteral("quickGunnerModeAction"));
    quickGunnerModeAction_->setCheckable(true);
    connect(quickGunnerModeAction_, &QAction::toggled, this,
            &ApplicationController::setQuickGunnerMode);
    quickGunnerOpenAction_ =
        quickGunnerMenu_->addAction(QStringLiteral("打开快枪手"), this, [this]
                                    { openResourceBrowser(QStringLiteral("quick-gunner")); });
    quickGunnerOpenAction_->setObjectName(QStringLiteral("quickGunnerOpenAction"));
    quickGunnerFrameAction_ =
        quickGunnerMenu_->addAction(QStringLiteral("调整拍摄区域…"), this,
                                    [this]
                                    {
                                        if (quickGunnerActionsEnabled())
                                            showQuickGunnerNotice(quickGunner_->beginFraming());
                                    });
    quickGunnerFrameAction_->setObjectName(QStringLiteral("quickGunnerFrameAction"));
    quickGunnerDirectoryAction_ = quickGunnerMenu_->addAction(
        QStringLiteral("选择快枪手目录…"), this, [this] { chooseQuickGunnerDirectory(); });
    quickGunnerDirectoryAction_->setObjectName(QStringLiteral("quickGunnerDirectoryAction"));
    quickGunnerStatusAction_ = quickGunnerMenu_->addAction(QString());
    quickGunnerStatusAction_->setObjectName(QStringLiteral("quickGunnerStatusAction"));
    quickGunnerStatusAction_->setEnabled(false);
    connect(quickGunnerMenu_, &QMenu::aboutToShow, this,
            &ApplicationController::refreshQuickGunnerActions);
    refreshQuickGunnerActions();
    recordingMenu_ = menu_.addMenu(QStringLiteral("录制"));
    recordingMenu_->setObjectName(QStringLiteral("recordingMenu"));
    recordingMenu_->menuAction()->setObjectName(QStringLiteral("recordingMenuAction"));
    recordingMenu_->setToolTipsVisible(true);
    menu_.setToolTipsVisible(true);
    recordingStatusAction_ = recordingMenu_->addAction(QString());
    recordingStatusAction_->setObjectName(QStringLiteral("recordingStatusAction"));
    recordingStatusAction_->setEnabled(false);
    recordingStartAction_ = recordingMenu_->addAction(
        QStringLiteral("开始录制…"), this, &ApplicationController::startRecordingSelection);
    recordingStartAction_->setObjectName(QStringLiteral("recordingStartAction"));
    recordingPauseAction_ = recordingMenu_->addAction(QStringLiteral("暂停"), this,
                                                      &ApplicationController::toggleRecordingPause);
    recordingPauseAction_->setObjectName(QStringLiteral("recordingPauseAction"));
    recordingStopAction_ = recordingMenu_->addAction(QStringLiteral("停止并保存"), recording_.get(),
                                                     &RecordingController::stopAndSave);
    recordingStopAction_->setObjectName(QStringLiteral("recordingStopAction"));
    recordingDiscardAction_ = recordingMenu_->addAction(QStringLiteral("丢弃…"), this,
                                                        &ApplicationController::discardRecording);
    recordingDiscardAction_->setObjectName(QStringLiteral("recordingDiscardAction"));
    recordingCancelAction_ = recordingMenu_->addAction(QStringLiteral("取消录制"), recording_.get(),
                                                       &RecordingController::cancel);
    recordingCancelAction_->setObjectName(QStringLiteral("recordingCancelAction"));
    recordingMenu_->addSeparator();
    recordingOpenVideoAction_ =
        recordingMenu_->addAction(QStringLiteral("打开录制视频"), this, [this]
                                  { openResourceBrowser(QStringLiteral("recording-video")); });
    recordingOpenVideoAction_->setObjectName(QStringLiteral("recordingOpenVideoAction"));
    recordingOpenGifAction_ =
        recordingMenu_->addAction(QStringLiteral("打开录制 GIF"), this,
                                  [this] { openResourceBrowser(QStringLiteral("recording-gif")); });
    recordingOpenGifAction_->setObjectName(QStringLiteral("recordingOpenGifAction"));
    connect(recording_.get(), &RecordingController::finished, this,
            [this]
            {
                recordingVideoSource_->directoryChanged();
                recordingGifSource_->directoryChanged();
            });
    connect(recording_.get(), &RecordingController::stateChanged, this,
            [this]
            {
                refreshRecordingActions();
                refreshStickerActions();
            });
    connect(recording_.get(), &RecordingController::progressChanged, this,
            &ApplicationController::refreshRecordingActions);
    connect(recording_.get(), &RecordingController::noticeChanged, this,
            &ApplicationController::showRecordingNotice);
    connect(recording_.get(), &RecordingController::quitReady, this,
            [this] { QTimer::singleShot(0, this, &ApplicationController::quit); });
    connect(recordingMenu_, &QMenu::aboutToShow, this,
            &ApplicationController::refreshRecordingActions);
    clipboardStickerAction_ = menu_.addAction(QStringLiteral("剪贴板贴图"), this,
                                              &ApplicationController::pinClipboardImage);
    clipboardStickerAction_->setObjectName(QStringLiteral("clipboardStickerAction"));
    settingsAction_ =
        menu_.addAction(QStringLiteral("设置…"), this, &ApplicationController::openSettings);
    settingsAction_->setObjectName(QStringLiteral("settingsAction"));
    watermarkMenu_ = menu_.addMenu(QStringLiteral("水印"));
    watermarkMenu_->setObjectName(QStringLiteral("watermarkMenu"));
    watermarkMenu_->menuAction()->setObjectName(QStringLiteral("watermarkMenuAction"));
    watermarkAutoAction_ = watermarkMenu_->addAction(QStringLiteral("常驻水印"));
    watermarkAutoAction_->setObjectName(QStringLiteral("watermarkAutoAction"));
    watermarkAutoAction_->setCheckable(true);
    connect(watermarkAutoAction_, &QAction::triggered, this,
            [this](bool enabled)
            {
                const QString error = watermarks_.setAutoEnabled(enabled);
                if (!error.isEmpty() && !options_.testMode && !options_.smokeTest)
                    tray_.showMessage(QStringLiteral("水印"), error, QSystemTrayIcon::Warning);
                rebuildWatermarkMenu();
            });
    watermarkPresetMenu_ = watermarkMenu_->addMenu(QStringLiteral("预设"));
    watermarkPresetMenu_->setObjectName(QStringLiteral("watermarkPresetMenu"));
    watermarkPresetMenu_->menuAction()->setObjectName(QStringLiteral("watermarkPresetMenuAction"));
    const auto refreshWatermarkMenu = [this]
    { QTimer::singleShot(0, this, &ApplicationController::rebuildWatermarkMenu); };
    connect(&watermarks_, &WatermarkService::preferencesChanged, this, refreshWatermarkMenu);
    connect(&watermarks_, &WatermarkService::currentPresetChanged, this, refreshWatermarkMenu);
    connect(&watermarks_, &WatermarkService::autoChanged, this, refreshWatermarkMenu);
    rebuildWatermarkMenu();
    historyMenu_ = menu_.addMenu(QStringLiteral("历史"));
    historyMenu_->setObjectName(QStringLiteral("historyMenu"));
    historyMenu_->menuAction()->setObjectName(QStringLiteral("historyMenuAction"));
    historyPreviousAction_ =
        historyMenu_->addAction(QStringLiteral("上一张"), this, [this] { navigateHistory(-1); });
    historyPreviousAction_->setObjectName(QStringLiteral("historyPreviousAction"));
    historyNextAction_ =
        historyMenu_->addAction(QStringLiteral("下一张"), this, [this] { navigateHistory(1); });
    historyNextAction_->setObjectName(QStringLiteral("historyNextAction"));
    openHistoryAction_ = historyMenu_->addAction(QStringLiteral("打开历史…"), this,
                                                 &ApplicationController::openHistory);
    openHistoryAction_->setObjectName(QStringLiteral("openHistoryAction"));
    auto* cleanupStatus = historyMenu_->addAction(QString());
    cleanupStatus->setObjectName(QStringLiteral("historyCleanupStatus"));
    cleanupStatus->setEnabled(false);
    const auto updateCleanupStatus = [this, cleanupStatus]
    {
        const auto message = history_.lastCleanup().message();
        cleanupStatus->setText(message);
        cleanupStatus->setToolTip(message);
        cleanupStatus->setVisible(!message.isEmpty());
    };
    connect(&history_, &HistoryManager::cleanupFinished, this, updateCleanupStatus);
    updateCleanupStatus();
    currentGroupMenu_ = menu_.addMenu(QStringLiteral("当前分组"));
    currentGroupMenu_->setObjectName(QStringLiteral("currentGroupMenu"));
    currentGroupMenu_->menuAction()->setObjectName(QStringLiteral("currentGroupAction"));
    connect(&history_, &HistoryManager::groupsChanged, this,
            &ApplicationController::rebuildCurrentGroupMenu);
    connect(&history_, &HistoryManager::currentGroupChanged, this,
            &ApplicationController::updateCurrentGroupMenu);
    historyDisabledAction_ = new QAction(QStringLiteral("历史"), &menu_);
    historyDisabledAction_->setObjectName(QStringLiteral("historyDisabledAction"));
    historyDisabledAction_->setEnabled(false);
    historyDisabledAction_->setToolTip(QStringLiteral("历史功能未启用；请在设置 → 历史中开启"));
    currentGroupDisabledAction_ = new QAction(QStringLiteral("当前分组"), &menu_);
    currentGroupDisabledAction_->setObjectName(QStringLiteral("currentGroupDisabledAction"));
    currentGroupDisabledAction_->setEnabled(false);
    currentGroupDisabledAction_->setToolTip(
        QStringLiteral("历史功能未启用；请在设置 → 历史中开启"));
    rebuildCurrentGroupMenu();
    hideAllStickersAction_ = menu_.addAction(QStringLiteral("隐藏全部贴图"), this,
                                             [this]
                                             {
                                                 stickers_.hideAll();
                                                 refreshStickerActions();
                                             });
    hideAllStickersAction_->setObjectName(QStringLiteral("hideAllStickersAction"));
    restoreAllStickersAction_ = menu_.addAction(QStringLiteral("恢复全部贴图"), this,
                                                [this]
                                                {
                                                    stickers_.restoreAll();
                                                    refreshStickerActions();
                                                });
    restoreAllStickersAction_->setObjectName(QStringLiteral("restoreAllStickersAction"));
    menu_.addAction(QStringLiteral("退出"), this, &ApplicationController::quit)
        ->setObjectName(QStringLiteral("quitAction"));
    connect(&menu_, &QMenu::aboutToShow, this,
            [this]
            {
                if (historyEnabled_ && !active_ && !settingsOpen_ && !resolvingQuit_ && !quitting_)
                    history_.refresh();
                refreshStickerActions();
            });
    refreshStickerActions();
    refreshRecordingActions();
    tray_.setContextMenu(&menu_);
    // 热插拔或屏幕参数变更只响应通知，立即取消当前会话。
    connect(&application_, &QGuiApplication::screenRemoved, this,
            [this](QScreen*) { layoutChanged(); });
    const auto watchScreen = [this](QScreen* screen)
    {
        connect(screen, &QScreen::geometryChanged, this, [this] { layoutChanged(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { layoutChanged(); });
    };
    for (QScreen* screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(&application_, &QGuiApplication::screenAdded, this,
            [this, watchScreen](QScreen* screen)
            {
                layoutChanged();
                watchScreen(screen);
            });
}
ApplicationController::~ApplicationController() { cleanup(); }
bool ApplicationController::quickGunnerActionsEnabled() const
{
    return regionCaptureSupported() && !recordingActive() && !active_ && !settingsOpen_ &&
           !quitting_ && !resolvingQuit_ &&
           quickGunner_->state() != QuickGunnerController::State::Framing;
}
void ApplicationController::connectQuickGunner()
{
    connect(quickGunner_.get(), &QuickGunnerController::noticeChanged, this,
            &ApplicationController::showQuickGunnerNotice);
    connect(quickGunner_.get(), &QuickGunnerController::shotFailed, this,
            &ApplicationController::showQuickGunnerNotice);
    connect(quickGunner_.get(), &QuickGunnerController::shotsDropped, this,
            [this](int count) {
                showQuickGunnerNotice(
                    QStringLiteral("快枪手已丢弃 %1 张尚未处理的图片。").arg(count));
            });
    connect(quickGunner_.get(), &QuickGunnerController::preferencesChanged, this,
            [this]
            {
                refreshQuickGunnerActions();
                quickGunnerSource_->directoryChanged();
                if (historyEnabled_ && historyWindow_)
                    historyWindow_->refresh();
            });
    connect(quickGunner_.get(), &QuickGunnerController::stateChanged, this,
            [this](QuickGunnerController::State state)
            {
                if (state == QuickGunnerController::State::Active)
                {
                    const auto error = quickGunnerHotkey_->activate();
                    if (!error.isEmpty())
                    {
                        quickGunnerModeRequested_ = false;
                        quickGunnerActivationPending_ = false;
                        quickGunner_->deactivate();
                        showQuickGunnerNotice(QStringLiteral("快枪手快门无法注册：") + error);
                    }
                    else
                        quickGunnerActivationPending_ = false;
                }
                else
                {
                    quickGunnerHotkey_->deactivate();
                    if (state == QuickGunnerController::State::Idle &&
                        quickGunnerActivationPending_)
                    {
                        quickGunnerModeRequested_ = false;
                        quickGunnerActivationPending_ = false;
                    }
                }
                refreshQuickGunnerActions();
            });
}
void ApplicationController::showQuickGunnerNotice(const QString& notice)
{
    if (notice.isEmpty())
        return;
    qWarning().noquote() << "快枪手：" + notice;
    if (!options_.testMode && !options_.smokeTest)
        tray_.showMessage(QStringLiteral("快枪手"), notice, QSystemTrayIcon::Warning);
}
QStringList ApplicationController::reservedQuickGunnerDirectories() const
{
    QStringList reserved;
    const auto root = history_.rootDirectory();
    reserved << (root.isEmpty() ? (options_.historyRoot.isEmpty()
                                       ? settings_.loadHistoryPreferences().rootDirectory
                                       : options_.historyRoot)
                                : root);
    for (const auto& group : history_.groups())
        if (!group.absolutePath.isEmpty())
            reserved << group.absolutePath;
    const auto recording = settings_.loadRecordingPreferences();
    reserved << recording.videoDirectory << recording.gifDirectory;
    return reserved;
}
bool ApplicationController::chooseQuickGunnerDirectory()
{
    const auto interactive = [this]
    {
        return !active_ && !settingsOpen_ && !quitting_ && !resolvingQuit_ &&
               quickGunner_->state() != QuickGunnerController::State::Framing &&
               !QApplication::activeModalWidget();
    };
    if (!interactive())
        return false;
    const auto directory = QFileDialog::getExistingDirectory(
        nullptr, QStringLiteral("选择快枪手目录"), quickGunner_->preferences().directory);
    if (directory.isEmpty() || !interactive())
        return false;
    const auto error = quickGunner_->setDirectory(directory, reservedQuickGunnerDirectories());
    showQuickGunnerNotice(error);
    return error.isEmpty();
}
void ApplicationController::setQuickGunnerMode(bool enabled)
{
    if (!enabled)
    {
        quickGunnerModeRequested_ = false;
        quickGunnerActivationPending_ = false;
        quickGunner_->deactivate();
    }
    else if (quickGunnerActionsEnabled())
    {
        quickGunnerModeRequested_ = true;
        if (quickGunner_->preferences().directory.isEmpty() && !chooseQuickGunnerDirectory())
            quickGunnerModeRequested_ = false;
        if (quickGunnerModeRequested_)
        {
            QString error;
            if (!quickGunner_->preferences().region)
            {
                quickGunnerActivationPending_ = true;
                error = quickGunner_->beginFraming(nullptr, true);
            }
            else
                error = quickGunner_->activate(reservedQuickGunnerDirectories());
            if (!error.isEmpty())
            {
                quickGunnerModeRequested_ = false;
                quickGunnerActivationPending_ = false;
                showQuickGunnerNotice(error);
            }
        }
    }
    refreshQuickGunnerActions();
}
void ApplicationController::quickGunnerShutter()
{
    if (recordingActive())
    {
        showRecordingNotice(QStringLiteral("录制中，已暂停截图 / 快枪手入口"));
        return;
    }
    if (!quickGunner_->active() || active_ || settingsOpen_ || quitting_ || resolvingQuit_)
        return;
    showQuickGunnerNotice(quickGunner_->shutter());
}
void ApplicationController::refreshQuickGunnerActions()
{
    if (!quickGunnerStatusAction_)
        return;
    const QSignalBlocker blocker(quickGunnerModeAction_);
    quickGunnerModeAction_->setChecked(quickGunnerModeRequested_);
    for (auto* action : {quickGunnerModeAction_, quickGunnerFrameAction_})
    {
        action->setEnabled(quickGunnerActionsEnabled());
        action->setToolTip(regionCaptureSupported() ? QString()
                                                    : QStringLiteral("Windows 原生采集后补"));
    }
    const bool browserEnabled = !active_ && !settingsOpen_ && !quitting_ && !resolvingQuit_;
    quickGunnerOpenAction_->setEnabled(browserEnabled);
    quickGunnerDirectoryAction_->setEnabled(!recordingActive() && browserEnabled &&
                                            quickGunner_->state() !=
                                                QuickGunnerController::State::Framing);
    const auto& preferences = quickGunner_->preferences();
    QString status =
        quickGunner_->active() ? QStringLiteral("快枪手：开") : QStringLiteral("快枪手：关");
    status += QStringLiteral(" · ") + (preferences.region
                                           ? QStringLiteral("机位 %1×%2 px")
                                                 .arg(preferences.region->pixelRect.width())
                                                 .arg(preferences.region->pixelRect.height())
                                           : QStringLiteral("机位未设置"));
    status +=
        QStringLiteral(" · ") +
        (preferences.directory.isEmpty() ? QStringLiteral("目录未设置") : preferences.directory);
    if (!regionCaptureSupported())
        status += QStringLiteral(" · Windows 原生采集后补");
    quickGunnerStatusAction_->setText(
        QFontMetrics(menu_.font()).elidedText(status, Qt::ElideMiddle, 440));
    quickGunnerStatusAction_->setToolTip(status);
}
QImage ApplicationController::quickGunnerWatermark(const QImage& image)
{
    if (!watermarks_.autoEnabled())
        return image;
    const auto* preset = watermarks_.currentPreset();
    const auto* asset = preset ? watermarks_.asset(preset->assetId) : nullptr;
    if (!preset || !asset)
        return image;
    QString error;
    const auto assetImage = watermarks_.assetImage(asset->id, &error);
    if (asset->kind == WatermarkKind::Image && assetImage.isNull())
    {
        if (!quickGunnerWatermarkWarned_)
        {
            quickGunnerWatermarkWarned_ = true;
            if (!options_.testMode && !options_.smokeTest)
                tray_.showMessage(
                    QStringLiteral("快枪手水印"),
                    QStringLiteral("水印资产无法读取，本次及后续受影响图片将保存原图。%1")
                        .arg(error),
                    QSystemTrayIcon::Warning);
        }
        return image;
    }
    WatermarkInstance instance;
    instance.presetId = preset->id;
    instance.layout = preset->layout;
    instance.opacity = preset->opacity;
    instance.scale = preset->scale;
    instance.normalizedCenter =
        watermarkInitialCenter(preset->layout, watermarkBaseSize(*asset, assetImage), image.size());
    return composeWatermark(image, *asset, assetImage, instance);
}
void ApplicationController::setHistoryMenusInstalled(bool installed)
{
    if (installed == historyMenusInstalled_)
        return;
    // 原生托盘对 disabled 子菜单的绘制差异较大；关闭历史时直接换成两个禁用的
    // 普通动作，保证“历史”“当前分组”在菜单里明确呈现灰态。
    if (!hideAllStickersAction_)
        return;
    if (installed)
    {
        menu_.removeAction(historyDisabledAction_);
        menu_.removeAction(currentGroupDisabledAction_);
        menu_.insertAction(hideAllStickersAction_, historyMenu_->menuAction());
        menu_.insertAction(hideAllStickersAction_, currentGroupMenu_->menuAction());
    }
    else
    {
        menu_.removeAction(historyMenu_->menuAction());
        menu_.removeAction(currentGroupMenu_->menuAction());
        menu_.insertAction(hideAllStickersAction_, historyDisabledAction_);
        menu_.insertAction(hideAllStickersAction_, currentGroupDisabledAction_);
    }
    historyMenusInstalled_ = installed;
}
void ApplicationController::refreshStickerActions()
{
    refreshQuickGunnerActions();
    if (recording_)
        refreshRecordingActions();
    setHistoryMenusInstalled(historyEnabled_);

    const bool interactive = !active_ && !settingsOpen_ && !resolvingQuit_ && !quitting_;
    const bool historyAvailable = interactive && historyEnabled_ && history_.ready();
    historyMenu_->setEnabled(historyAvailable);
    historyMenu_->menuAction()->setEnabled(historyAvailable);
    openHistoryAction_->setEnabled(historyAvailable);
    bool hasItems = false;
    if (historyEnabled_)
        for (const auto& group : history_.snapshot().groups)
            hasItems |= !group.items.isEmpty();
    historyPreviousAction_->setEnabled(historyAvailable && hasItems);
    historyNextAction_->setEnabled(historyAvailable && hasItems);
    currentGroupMenu_->setEnabled(historyAvailable);
    currentGroupMenu_->menuAction()->setEnabled(historyAvailable);
    if (!historyEnabled_)
    {
        // 某些原生托盘实现不会只因为父菜单 disabled 就呈现灰态；关闭时把所有子项
        // 一并禁用，确保“历史”和“当前分组”在视觉与键盘上都不可进入。
        for (auto* action : historyMenu_->actions())
            if (!action->isSeparator())
                action->setEnabled(false);
        for (auto* action : currentGroupMenu_->actions())
            if (!action->isSeparator())
                action->setEnabled(false);
    }
    if (historyWindow_)
        historyWindow_->setGroupActionsEnabled(interactive);
    clipboardStickerAction_->setEnabled(!recordingActive() && !active_ && !resolvingQuit_ &&
                                        !quitting_);
    bool visible = false, hidden = false;
    for (const auto& window : stickers_.windows())
        if (window)
        {
            visible |= window->isVisible();
            hidden |= !window->isVisible();
        }
    hideAllStickersAction_->setEnabled(visible);
    restoreAllStickersAction_->setEnabled(hidden);
}
void ApplicationController::rebuildWatermarkMenu()
{
    if (!watermarkMenu_ || !watermarkAutoAction_ || !watermarkPresetMenu_)
        return;
    const auto& preferences = watermarks_.preferences();
    const bool hasPresets = !preferences.presets.isEmpty();
    {
        const QSignalBlocker blocker(watermarkAutoAction_);
        watermarkAutoAction_->setChecked(watermarks_.autoEnabled());
    }
    watermarkAutoAction_->setEnabled(hasPresets);
    watermarkPresetMenu_->clear();
    watermarkPresetMenu_->setEnabled(hasPresets);
    if (!hasPresets)
    {
        auto* placeholder =
            watermarkPresetMenu_->addAction(QStringLiteral("请先在设置 → 水印中创建预设"));
        placeholder->setEnabled(false);
        watermarkMenu_->setToolTip(QStringLiteral("请先在设置 → 水印中创建预设"));
        return;
    }
    for (const auto& preset : preferences.presets)
    {
        auto* action = watermarkPresetMenu_->addAction(preset.name);
        action->setObjectName(QStringLiteral("watermarkPresetAction-%1").arg(preset.id));
        action->setCheckable(true);
        action->setChecked(preset.id == watermarks_.currentPresetId());
        connect(action, &QAction::triggered, this,
                [this, id = preset.id]
                {
                    const QString error = watermarks_.setCurrentPreset(id);
                    if (!error.isEmpty() && !options_.testMode && !options_.smokeTest)
                        tray_.showMessage(QStringLiteral("水印"), error, QSystemTrayIcon::Warning);
                    QTimer::singleShot(0, this, &ApplicationController::rebuildWatermarkMenu);
                });
    }
    watermarkMenu_->setToolTip(
        QStringLiteral("当前预设：%1")
            .arg(watermarks_.currentPreset() ? watermarks_.currentPreset()->name : QString()));
}
void ApplicationController::rebuildCurrentGroupMenu()
{
    currentGroupMenu_->clear();
    auto groups = history_.groups();
    std::stable_sort(groups.begin(), groups.end(),
                     [](const auto& a, const auto& b) { return a.isDefault && !b.isDefault; });
    for (const auto& group : groups)
    {
        auto* action = currentGroupMenu_->addAction(
            group.name, this,
            [this, id = group.id]
            {
                if (active_ || settingsOpen_ || resolvingQuit_ || quitting_)
                    return;
                const auto result = history_.setCurrentGroup(id);
                if (!result.success)
                {
                    qWarning().noquote() << result.error;
                    if (!options_.testMode)
                        tray_.showMessage(QStringLiteral("当前分组切换失败"), result.error,
                                          QSystemTrayIcon::Warning);
                }
                updateCurrentGroupMenu();
            });
        action->setObjectName(QStringLiteral("currentGroupItem_%1").arg(group.id));
        action->setData(group.id);
        action->setCheckable(true);
    }
    currentGroupMenu_->addSeparator();
    auto* create = currentGroupMenu_->addAction(QStringLiteral("新建分组…"), this,
                                                [this]
                                                {
                                                    if (active_ || settingsOpen_ ||
                                                        resolvingQuit_ || quitting_)
                                                        return;
                                                    settingsOpen_ = true;
                                                    refreshStickerActions();
                                                    createHistoryGroupDialog(history_, nullptr);
                                                    settingsOpen_ = false;
                                                    refreshStickerActions();
                                                });
    create->setObjectName(QStringLiteral("newGroupAction"));
    updateCurrentGroupMenu();
}
void ApplicationController::updateCurrentGroupMenu()
{
    if (!historyEnabled_)
    {
        currentGroupMenu_->setTitle(QStringLiteral("当前分组：历史未启用"));
        return;
    }
    const auto group = history_.currentGroup();
    currentGroupMenu_->setTitle(
        QStringLiteral("当前分组：%1").arg(group ? group->name : QStringLiteral("不可用")));
    for (auto* action : currentGroupMenu_->actions())
        action->setChecked(action->data().toString() == history_.currentGroupId());
}
void ApplicationController::pinClipboardImage()
{
    if (recordingActive() || active_ || resolvingQuit_ || quitting_)
        return;
    QImage image = clipboardActions_.loadImage();
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    image.setDevicePixelRatio(1);
    const QSize size =
        screen ? stickerWindowSize(image.size(), 1, screen->devicePixelRatio()) : QSize();
    if (image.isNull() || size.isEmpty() ||
        !stickers_
             .create(image, clipboardStickerPosition(available, size, clipboardStickerSequence_))
             .success)
    {
        if (!clipboardToast_)
            clipboardToast_ = std::make_unique<ClipboardStickerToast>();
        clipboardToast_->showMessage(
            QStringLiteral("剪贴板中没有可用图片，请先复制图片后再试；剪贴板内容未被修改。"),
            cursor, available);
    }
    else
    {
        ++clipboardStickerSequence_;
        if (clipboardToast_)
            clipboardToast_->close();
    }
    refreshStickerActions();
}
void ApplicationController::start()
{
    quickGunnerHotkey_->load();
    if (options_.smokeTest)
    {
        startSmokeTest();
        return;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        qCritical("系统托盘不可用，应用无法驻留。请在正常桌面环境启动。");
        QTimer::singleShot(0, &application_, [this] { application_.exit(2); });
        return;
    }
    const auto startup = hotkeySettings_.initialize();
    qInfo().noquote() << startup.report;
    for (auto& historyHotkey : historyHotkeys_)
        qInfo().noquote() << historyHotkey->initialize().report;
    tray_.setToolTip(hotkeySettings_.tooltip());
    tray_.show();
    if (recordingCaptureSupported())
        for (auto& key : recordingHotkeys_)
        {
            const auto initialized = key->initialize();
            qInfo().noquote() << initialized.report;
            if (!initialized.warning.isEmpty())
                showRecordingNotice(initialized.warning +
                                    QStringLiteral("\n请在设置 → 通用改键。"));
        }
    const auto cleanup = history_.lastCleanup();
    if (!options_.testMode && (cleanup.deletedCount || cleanup.protectedOverLimit))
        tray_.showMessage(QStringLiteral("截图历史自动清理"), cleanup.message(),
                          QSystemTrayIcon::Information);
    if (!startup.warning.isEmpty() && !options_.testMode)
        tray_.showMessage(hotkeySettings_.enabled() ? QStringLiteral("截图键已回退")
                                                    : QStringLiteral("快捷键未启用"),
                          startup.warning, QSystemTrayIcon::Warning);
    if (options_.testMode)
    {
        // 仅受控模式有这些定时器；空闲采样必须走普通启动。
        QTimer::singleShot(options_.stableMs, this,
                           [this] { trigger(QStringLiteral("injected")); });
    }
}
HotkeySettings& ApplicationController::hotkeySettings(HotkeyAction action)
{
    switch (action)
    {
    case HotkeyAction::Screenshot:
        return hotkeySettings_;
    case HotkeyAction::HistoryPrevious:
    case HotkeyAction::HistoryNext:
    case HotkeyAction::HistoryOpen:
        return *historyHotkeys_[size_t(action) - 1];
    case HotkeyAction::QuickGunnerShutter:
        break;
    case HotkeyAction::RecordingStartStop:
    case HotkeyAction::RecordingPauseResume:
        return *recordingHotkeys_[size_t(action) - 5];
    }
    qFatal("此动作不使用常驻热键设置。");
}
QKeySequence ApplicationController::sequenceForAction(HotkeyAction action)
{
    return action == HotkeyAction::QuickGunnerShutter ? quickGunnerHotkey_->currentSequence()
                                                      : hotkeySettings(action).currentSequence();
}
QString ApplicationController::changeHotkey(const QKeySequence& sequence, HotkeyAction action)
{
    for (const auto other : hotkeyActions)
        if (other != action && sequenceForAction(other) == sequence)
            return QStringLiteral("%1与%2快捷键重复，原绑定保留。")
                .arg(hotkeyActionName(action), hotkeyActionName(other));
    const bool shutter = action == HotkeyAction::QuickGunnerShutter;
    const QString error = shutter ? quickGunnerHotkey_->change(sequence)
                                  : hotkeySettings(action).changeHotkey(sequence);
    const QString explanation =
        shutter ? quickGunnerHotkey_->explanation() : hotkeySettings(action).explanation();
    qInfo().noquote() << (error.isEmpty() ? explanation : error);
    tray_.setToolTip(hotkeySettings_.tooltip());
    return error;
}
void ApplicationController::openSettings()
{
    if (recordingActive() || active_ || quitting_ || resolvingQuit_ || settingsOpen_)
        return;
    settingsOpen_ = true;
    refreshStickerActions();
    QString shapeNotice;
    const auto templates = settings_.loadShapeTemplates(&shapeNotice);
    HistorySettingsActions historySettings;
    historySettings.preferences = settings_.loadHistoryPreferences();
    historySettings.enabled = historyEnabled_;
    if (historyEnabled_ && history_.ready())
        historySettings.preferences.rootDirectory = history_.rootDirectory();
    for (size_t i = 0; i < historyHotkeys_.size(); ++i)
        historySettings.hotkeys[i] = historyHotkeys_[i]->currentSequence();
    if (historyEnabled_ && history_.ready())
    {
        historySettings.usage =
            history_.totalUsage(historyWindow_ ? historyWindow_->selectedItemId() : QString());
        historySettings.groups = history_.groups();
        historySettings.currentGroupId = history_.currentGroupId();
    }
    if (historyEnabled_ && history_.ready())
    {
        historySettings.createGroup = [this](QWidget* parent)
        { createHistoryGroupDialog(history_, parent); };
        historySettings.renameGroup = [this](QWidget* parent, const QString& id)
        { renameHistoryGroupDialog(history_, id, parent); };
        historySettings.changeGroupDirectory = [this](QWidget* parent, const QString& id)
        { changeHistoryGroupDirectoryDialog(history_, id, parent); };
        historySettings.openGroupSettings = [this](QWidget* parent, const QString& id)
        {
            GroupSettingsDialog dialog(
                history_, id, historyActions_.trash,
                historyWindow_ ? historyWindow_->selectedItemId() : QString(), parent);
            dialog.exec();
        };
        historySettings.setCurrentGroup = [this](QWidget* parent, const QString& id)
        {
            const auto result = history_.setCurrentGroup(id);
            if (!result.success && parent)
                QMessageBox::warning(parent, QStringLiteral("设为当前分组失败"), result.error);
        };
        historySettings.deleteGroup = [this](QWidget* parent, const QString& id)
        { confirmHistoryGroupDeletion(history_, id, historyActions_.trash, parent); };
        historySettings.usageNotice = history_.snapshot().error;
        if (!history_.lastCleanup().message().isEmpty())
            historySettings.usageNotice += QStringLiteral("\n") + history_.lastCleanup().message();
    }
    else
    {
        historySettings.usageNotice =
            QStringLiteral("截图历史默认关闭。开启前不会创建或读取历史目录；已有历史文件仍保留，"
                           "关闭后也不会自动删除。");
    }
    for (const auto& key : historyHotkeys_)
        if (!key->enabled() && !key->explanation().isEmpty() &&
            !historySettings.usageNotice.contains(key->explanation()))
            historySettings.usageNotice += QStringLiteral("\n") + key->explanation();
    historySettings.saveHotkey = [this](HotkeyAction action, const QKeySequence& sequence)
    { return changeHotkey(sequence, action); };
    WatermarkSettingsActions watermarkSettings;
    watermarkSettings.preferences = watermarks_.preferences();
    watermarkSettings.historyEnabled = historyEnabled_;
    watermarkSettings.chooseImage = [](QWidget* parent, const QString& current)
    {
        return QFileDialog::getOpenFileName(parent, QStringLiteral("选择水印图片"), current,
                                            QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp)"));
    };
    watermarkSettings.importImage =
        [this](const QString& sourcePath, QString* managed, QString* error)
    { return watermarks_.importImage(sourcePath, managed, error); };
    watermarkSettings.loadManagedImage = [this](const QString& fileName, QString* error)
    { return watermarks_.loadManagedImage(fileName, error); };
    watermarkSettings.removeAssetImage = [this](const QString& fileName)
    { return watermarks_.removeAssetImage(fileName); };
    watermarkSettings.savePreferences = [this](const WatermarkPreferences& preferences)
    { return watermarks_.savePreferences(preferences); };
    historySettings.savePreferences = [this](const HistoryPreferences& preferences)
    {
        if (!preferences.enabled)
        {
            const auto error = settings_.saveHistoryPreferences(preferences);
            if (!error.isEmpty())
                return error;
            historyEnabled_ = false;
            if (historyWindow_)
                historyWindow_->setHistoryEnabled(false);
            // 关闭后同步清空 / 置灰当前分组菜单，不能等下一次菜单重建。
            rebuildCurrentGroupMenu();
            refreshStickerActions();
            return QString();
        }
        const bool enabling = !historyEnabled_;
        const auto oldRoot = history_.rootDirectory();
        const auto result = history_.savePreferences(preferences);
        if (result.success)
        {
            if (enabling)
            {
                // 启用后从默认分组开始，避免复用关闭前的自定义当前分组。
                for (const auto& group : history_.groups())
                    if (group.isDefault)
                    {
                        const auto selected = history_.setCurrentGroup(group.id);
                        if (!selected.success)
                            qWarning().noquote()
                                << "历史开启后选择默认分组失败：" << selected.error;
                        break;
                    }
            }
            historyEnabled_ = true;
            // 开启时 groupsChanged 在 historyEnabled_ 仍为 false 时已经发出；这里必须
            // 重新建组并刷新标题，才能立即显示默认分组的当前状态。
            rebuildCurrentGroupMenu();
            if (historyWindow_)
            {
                historyWindow_->setHistoryEnabled(true);
                historyWindow_->refresh();
            }
            if (!oldRoot.isEmpty() && oldRoot != history_.rootDirectory() && historyWindow_ &&
                historyWindow_->selectedSourceId() == QStringLiteral("history"))
                historyWindow_->navigate(0, true);
            refreshStickerActions();
        }
        return result.error;
    };
    RecordingSettingsActions recordingSettings;
    recordingSettings.preferences = settings_.loadRecordingPreferences(nullptr, false);
    recordingSettings.reservedDirectories =
        [this](const QString& root, QString* quick, QString* error)
    {
        *quick = settings_.loadQuickGunnerPreferences().directory;
        const auto effectiveRoot = root.isEmpty() ? HistoryStore::defaultRootDirectory() : root;
        return HistoryStore(effectiveRoot).registeredDirectories(error);
    };
    recordingSettings.savePreferences = [this](const RecordingPreferences& preferences)
    {
        const auto error = settings_.saveRecordingPreferences(preferences);
        if (error.isEmpty())
        {
            recordingVideoSource_->directoryChanged();
            recordingGifSource_->directoryChanged();
            if (historyEnabled_ && history_.ready())
                history_.refresh();
        }
        return error;
    };
    SettingsDialog dialog(
        hotkeySettings_.currentSequence(),
        [this](const QKeySequence& sequence) { return changeHotkey(sequence); },
        (hotkeySettings_.enabled() ? QString() : hotkeySettings_.explanation()) + shapeNotice,
        nullptr,
        {settings_.loadSavePreferences(),
         [this](const SavePreferences& preferences)
         { return settings_.saveSavePreferences(preferences); },
         {},
         settings_.loadCaptureRatios(),
         [this](const CaptureRatioSettings& ratios) { return settings_.saveCaptureRatios(ratios); },
         templates,
         settings_.shapeTemplateDir(),
         [this](const QVector<ShapeTemplateEntry>& entries)
         { return settings_.saveShapeTemplates(entries); },
         [this](const QStringList& paths) { return settings_.importShapeTemplates(paths); },
         {},
         settings_.loadStickerCloseReminder(),
         [this](bool enabled) { return settings_.saveStickerCloseReminder(enabled); },
         settings_.loadStickerDoubleClickClose(),
         [this](bool enabled) { return settings_.saveStickerDoubleClickClose(enabled); },
         quickGunnerHotkey_->currentSequence(),
         [this](const QKeySequence& sequence)
         { return changeHotkey(sequence, HotkeyAction::QuickGunnerShutter); },
         {recordingHotkeys_[0]->currentSequence(), recordingHotkeys_[1]->currentSequence()},
         [this](HotkeyAction action, const QKeySequence& sequence)
         { return changeHotkey(sequence, action); }},
        autostartActions_, std::move(historySettings), std::move(watermarkSettings),
        std::move(recordingSettings));
    connect(&history_, &HistoryManager::groupsChanged, &dialog,
            [this, &dialog]
            {
                dialog.updateHistoryUsage(history_.totalUsage(), history_.lastCleanup().message());
                dialog.updateHistoryGroups(history_.groups(), history_.currentGroupId());
            });
    connect(&history_, &HistoryManager::currentGroupChanged, &dialog, [this, &dialog]
            { dialog.updateHistoryGroups(history_.groups(), history_.currentGroupId()); });
    dialog.exec();
    settingsOpen_ = false;
    refreshStickerActions();
}
ImageOutputResult ApplicationController::pinHistoryImage(const QImage& image)
{
    if (active_ || settingsOpen_ || quitting_ || resolvingQuit_)
        return {false, QStringLiteral("截图、设置或退出期间无法钉住历史图片。")};
    QScreen* screen = historyWindow_ ? historyWindow_->screen() : nullptr;
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return {false, QStringLiteral("未找到可用显示器，请重试。")};
    const QRect available = screen->availableGeometry();
    const qreal dpr = screen->devicePixelRatio();
    const qreal scale = historyStickerScale(image.size(), available, dpr);
    const auto size = stickerWindowSize(image.size(), scale, dpr);
    const auto result = stickers_.create(
        image, clipboardStickerPosition(available, size, historyStickerSequence_), true, scale);
    if (result.success)
        ++historyStickerSequence_;
    refreshStickerActions();
    return result;
}
void ApplicationController::navigateHistory(int step)
{
    if (active_ || settingsOpen_ || quitting_ || resolvingQuit_)
        return;
    if (!historyEnabled_ || !history_.ready())
    {
        openHistory();
        return;
    }
    const bool opening = !historyWindow_ || !historyWindow_->isVisible();
    openHistory();
    if (historyWindow_)
        historyWindow_->navigate(step, opening);
    refreshStickerActions();
}
void ApplicationController::openHistory() { openResourceBrowser(QStringLiteral("history")); }
void ApplicationController::openResourceBrowser(const QString& sourceId)
{
    if (active_ || settingsOpen_ || quitting_ || resolvingQuit_)
        return;
    if (!historyWindow_)
    {
        historyWindow_ = new HistoryWindow(history_, settings_, historyActions_,
                                           QVector<ResourceSource*>{quickGunnerSource_.get(),
                                                                    recordingVideoSource_.get(),
                                                                    recordingGifSource_.get()},
                                           historyEnabled_);
        connect(historyWindow_, &HistoryWindow::groupDialogOpenChanged, this,
                [this](bool open)
                {
                    settingsOpen_ = open;
                    refreshStickerActions();
                });
        connect(historyWindow_, &HistoryWindow::directoryRequested, this,
                [this](const QString& id)
                {
                    if (id == quickGunnerSource_->id())
                        chooseQuickGunnerDirectory();
                });
    }
    if (!sourceId.isEmpty())
        historyWindow_->selectSource(sourceId);
    historyWindow_->refresh();
    historyWindow_->show();
    historyWindow_->raise();
    historyWindow_->activateWindow();
    refreshStickerActions();
}
void ApplicationController::trigger(const QString& source)
{
    if (recordingActive())
    {
        showRecordingNotice(QStringLiteral("录制中，已暂停截图 / 快枪手入口"));
        return;
    }
    if (quickGunner_ && (quickGunner_->capturing() || quickGunner_->pendingCaptureCount() > 0))
    {
        showQuickGunnerNotice(QStringLiteral("快枪手正在拍摄，请稍后再按普通截图键。"));
        return;
    }
    if (quickGunner_ && quickGunner_->state() == QuickGunnerController::State::Framing)
    {
        if (!options_.testMode && !options_.smokeTest)
            tray_.showMessage(QStringLiteral("快枪手"), QStringLiteral("请先完成或取消机位框选"),
                              QSystemTrayIcon::Information);
        return;
    }
    const qint64 t0 = monotonicNs();
    if (active_ || quitting_ || resolvingQuit_ || settingsOpen_)
    {
        qWarning().noquote()
            << "截图触发被忽略：active=" << active_ << " quitting=" << quitting_
            << " resolvingQuit=" << resolvingQuit_ << " settingsOpen=" << settingsOpen_
            << visibilityQuery_(overlay_ ? overlay_->windowHandle() : nullptr).diagnostic();
        return;
    }
    if (!retryDisplay_)
        scrollRestoreFocus_ = captureForegroundRestorer();
    if (quickGunner_ && quickGunner_->flashWindow())
        quickGunner_->flashWindow()->hide();
    historySession_ =
        historyEnabled_ && history_.ready() ? history_.beginSession() : HistorySession{};
    active_ = true;
    qInfo() << "截图会话开始：source=" << source;
    if (clipboardToast_)
        clipboardToast_->close();
    refreshStickerActions();
    settingsAction_->setEnabled(false);
    const quint64 token = ++token_;
    metrics_ = {};
    metrics_.sequence = ++sequence_;
    metrics_.trigger = source;
    metrics_.t0 = t0;
    sessionWatchdog_.start();
    const auto display =
        retryDisplay_ ? DisplayResult{*retryDisplay_, {}} : displays_->cursorDisplay();
    if (retryDisplay_ && !displays_->stillMatches(*retryDisplay_))
    {
        retryDisplay_.reset();
        retryRegion_ = {};
        fail(4, QStringLiteral("显示器布局已变化，请重新框选长图区域。"));
        return;
    }
    if (!display.error.isEmpty())
    {
        fail(5, display.error);
        return;
    }
    if (!capture_->hasPermission())
    {
        // 脚本不弹授权窗、不将权限时间混入性能。首次授权由用户从 F1 / 托盘执行。
        if (options_.testMode || !capture_->requestPermission())
        {
            fail(3, capture_->permissionExplanation());
            return;
        }
    }
    metrics_.captureStart = monotonicNs();
    QPointer<ApplicationController> self(this);
    capture_->capture(display.target,
                      [self, token](CaptureResult result) mutable
                      {
                          if (self)
                              self->captureCompleted(token, std::move(result));
                      });
    // 仅会话存活时存在超时保护；空闲不轮询。
    captureTimeout_.start(15000);
}
void ApplicationController::captureCompleted(quint64 token, CaptureResult result)
{
    if (!active_ || token != token_)
        return;
    metrics_.captureComplete = monotonicNs();
    if (result.error != CaptureError::None)
    {
        fail(7, result.explanation);
        return;
    }
    if (!displays_->stillMatches(result.frame.display))
    {
        fail(4, QStringLiteral("显示器布局已变化，本次截图已取消，请重试。"));
        return;
    }
    qInfo() << "截图捕获完成，耗时 " << (metrics_.captureComplete - metrics_.t0) / 1000000 << " ms";
    metrics_.frameWidth = result.frame.pixels.width();
    metrics_.frameHeight = result.frame.pixels.height();
    const auto listed = windows_->visibleWindows(result.frame.display);
    const auto localWindows =
        localWindowRects(listed.windows, result.frame.display.logicalGeometry);
    OverlayActions actions;
    actions.watermarks = &watermarks_;
    const auto session = historySession_;
    actions.historySessionId = session.id;
    actions.historyGroupId = session.groupId;
    actions.recordHistory = [this, session](const QImage& image, const QString&) -> HistoryStatus
    {
        if (!historyEnabled_ || session.id.isEmpty())
            return {true, {}};
        const auto result = history_.record(session, image, HistorySource::Overlay);
        return {result.success, result.error};
    };
    actions.recordHistoryPair = [this, session](const QImage& image,
                                                const std::optional<QImage>& original,
                                                const QString&) -> HistoryStatus
    {
        if (!historyEnabled_ || session.id.isEmpty())
            return {true, {}};
        const auto result = watermarks_.retainOriginal() && original
                                ? history_.record(session, image, HistorySource::Overlay, *original)
                                : history_.record(session, image, HistorySource::Overlay);
        return {result.success, result.error};
    };
    actions.ratios = settings_.loadCaptureRatios();
    actions.shapeTemplates = settings_.loadShapeTemplates(&actions.shapeTemplateNotice);
    actions.shapeTemplateDir = settings_.shapeTemplateDir();
    actions.loadSavePreferences = [this] { return settings_.loadSavePreferences(); };
    actions.pinImage = [this](const QImage& image)
    {
        if (quitting_ || !overlay_)
            return ImageOutputResult{false, QStringLiteral("截图会话已关闭。")};
        return stickers_.create(image, overlay_->selectionGlobalPosition(),
                                overlay_->isSelectionSaved());
    };
    const DisplayTarget scrollDisplay = result.frame.display;
    overlay_ =
        overlayFactory_(std::move(result.frame), std::move(actions), localWindows, listed.error);
    bindScrollOverlay(scrollDisplay);
    overlay_->winId();
    metrics_.windowCreated = monotonicNs();
    connect(overlay_, &SelectionOverlay::finished, this,
            [this](QRect pixels, int outcome) { finish(pixels, outcome); });
    connect(overlay_, &SelectionOverlay::firstPaintCompleted, this,
            [this, token](qint64 timestamp)
            {
                qInfo() << "覆盖层首帧完成";
                metrics_.paintComplete = timestamp;
                // 排到 backing store flush 之后，再观察目标屏刷新；不能用 paintEvent 当可见终点。
                QTimer::singleShot(0, this,
                                   [this, token]
                                   {
                                       if (!active_ || token != token_ || !overlay_ ||
                                           scrollFlowActive_)
                                           return;
                                       watchPresentation(token);
                                   });
            });
    overlay_->show();
    overlay_->raise();
    overlay_->activateWindow();
    overlay_->setFocus(Qt::ActiveWindowFocusReason);
    watchOverlayWindow(overlay_, true);
    if (!retryRegion_.isEmpty())
    {
        const QRect region = retryRegion_;
        retryRegion_ = {};
        retryDisplay_.reset();
        overlay_->enterScrollReady(region);
    }
}

void ApplicationController::bindScrollOverlay(DisplayTarget display)
{
    connect(overlay_, &SelectionOverlay::scrollReadyChanged, this,
            [this](bool ready)
            {
                scrollFlowActive_ = ready;
                if (ready)
                {
                    captureTimeout_.stop();
                    observation_.reset();
                    qInfo().nospace() << "截长图进入准备态：区域 " << overlay_->selection().width()
                                      << "×" << overlay_->selection().height();
                }
            });
    connect(overlay_, &SelectionOverlay::scrollCaptureRequested, this,
            [this, display](QRect pixels) { beginScrollCapture({display, pixels, {}}); });
}
void ApplicationController::beginScrollCapture(RegionCaptureRequest request)
{
    if (!active_ || !scrollFlowActive_ || scrollSession_ || !overlay_)
        return;
    captureTimeout_.stop();
    observation_.reset();
    overlay_->hide();
    if (scrollRestoreFocus_)
        scrollRestoreFocus_();
    const qreal dpr = request.display.devicePixelRatio;
    const QRect region(
        request.display.logicalGeometry.topLeft() +
            QPoint(qRound(request.pixelRect.x() / dpr), qRound(request.pixelRect.y() / dpr)),
        QSize(qRound(request.pixelRect.width() / dpr), qRound(request.pixelRect.height() / dpr)));
    const QRect available = request.display.availableLogicalGeometry.isEmpty()
                                ? request.display.logicalGeometry
                                : request.display.availableLogicalGeometry;
    scrollBorder_ = new ScrollBorderWindow(region);
    scrollBar_ = new ScrollControlBar(region, available);
    scrollBorder_->show();
    scrollBar_->show();
    // 窗口显示并获得原生编号之后再建立 SCK 排除列表。
    for (QWidget* window :
         {static_cast<QWidget*>(scrollBorder_), static_cast<QWidget*>(scrollBar_)})
    {
        window->winId();
        raiseOverlayAboveSystemChrome(window->windowHandle());
        watchOverlayWindow(window, false);
        const quint64 id = regionCaptureWindowId(window->windowHandle());
        if (id)
            request.excludedWindowIds.append(id);
    }
    scrollSession_ = scrollFactory_(request, scrollBar_->geometry(), this);
    if (!scrollSession_)
    {
        finish({}, scrollCancelledOutcome);
        return;
    }
    const auto session = scrollSession_;
    connect(scrollBar_, &ScrollControlBar::finishRequested, session,
            &ScrollCaptureSession::complete);
    connect(scrollBar_, &ScrollControlBar::cancelRequested, this,
            [this] { finish({}, scrollCancelledOutcome); });
    connect(scrollBar_, &ScrollControlBar::automaticRequested, session,
            &ScrollCaptureSession::setAutomatic);
    connect(scrollBar_, &ScrollControlBar::speedRequested, session,
            &ScrollCaptureSession::cycleSpeed);
    connect(scrollBar_, &ScrollControlBar::directionRequested, session,
            &ScrollCaptureSession::cycleDirection);
    connect(scrollBar_, &ScrollControlBar::placementChanged, session,
            &ScrollCaptureSession::setControlBar);
    connect(session, &ScrollCaptureSession::noticeChanged, scrollBar_,
            &ScrollControlBar::setNotice);
    connect(session, &ScrollCaptureSession::statisticsChanged, scrollBar_,
            [this](ScrollStitchStats stats)
            {
                if (scrollBar_ && scrollSession_)
                    scrollBar_->setStatistics(stats, scrollSession_->lengthLimit());
            });
    connect(session, &ScrollCaptureSession::automaticChanged, scrollBar_,
            &ScrollControlBar::setAutomatic);
    connect(session, &ScrollCaptureSession::optionsChanged, scrollBar_,
            &ScrollControlBar::setOptions);
    connect(session, &ScrollCaptureSession::permissionMissing, this,
            [this, session]
            {
                if (!scrollBar_ || !scrollBorder_)
                    return;
                const auto bar = scrollBar_;
                const auto border = scrollBorder_;
                bar->hide();
                border->hide();
                const bool open = bar->explainAccessibilityPermission();
                if (scrollSession_ != session || !session || !active_)
                    return;
                if (open)
                    session->openAccessibilitySettings();
                if (bar)
                    bar->show();
                if (border)
                    border->show();
            });
    connect(session, &ScrollCaptureSession::completed, this,
            [this, request]
            {
                retryDisplay_ = request.display;
                retryRegion_ = request.pixelRect;
                finishScrollCapture();
            });
    scrollStartedNs_ = monotonicNs();
    session->start();
}
void ApplicationController::finishScrollCapture()
{
    if (!scrollSession_ || !active_)
        return;
    QImage image = scrollSession_->takeResult();
    const auto session = historySession_;
    QImage recorded = image;
    bool recordedHasWatermark = false;
    if (!image.isNull() && watermarks_.autoEnabled())
    {
        const auto* preset = watermarks_.currentPreset();
        const auto* asset = preset ? watermarks_.asset(preset->assetId) : nullptr;
        if (preset && asset)
        {
            QString error;
            const QImage assetImage = watermarks_.assetImage(asset->id, &error);
            if (asset->kind == WatermarkKind::Text || !assetImage.isNull())
            {
                WatermarkInstance instance;
                instance.presetId = preset->id;
                instance.layout = preset->layout;
                instance.opacity = preset->opacity;
                instance.scale = preset->scale;
                instance.normalizedCenter = watermarkInitialCenter(
                    preset->layout, watermarkBaseSize(*asset, assetImage), image.size());
                recorded = composeWatermark(image, *asset, assetImage, instance);
                recordedHasWatermark = true;
            }
        }
    }
    if (!image.isNull() && historyEnabled_ && !session.id.isEmpty())
    {
        if (watermarks_.retainOriginal() && recordedHasWatermark)
            history_.record(session, recorded, HistorySource::Scroll, image);
        else
            history_.record(session, recorded, HistorySource::Scroll);
    }
    const auto stats = scrollSession_->stats();
    if (image.isNull() && stats.limitReached)
    {
        // 预算连首帧也容不下，先关闭高层级采集窗，再明确说明，避免静默消失。
        clearScrollCapture(scrollCancelledOutcome);
        fail(scrollCancelledOutcome,
             QStringLiteral("选区过大或可用内存不足，无法创建长图；请缩小选区后重试。"));
        return;
    }
    const auto display = retryDisplay_;
    const QRect region = retryRegion_;
    const auto restoreFocus = scrollRestoreFocus_;
    finish(image.isNull() ? QRect() : QRect(QPoint(), image.size()),
           image.isNull() ? scrollCancelledOutcome : scrollCompletedOutcome);
    if (image.isNull() || quitting_ || !display)
        return;
    ScrollResultActions actions;
    actions.watermarks = &watermarks_;
    actions.watermarkEnabled = watermarks_.autoEnabled();
    actions.historyUpdated = [this, session](const QImage& image)
    {
        if (historyEnabled_ && !session.id.isEmpty())
            history_.record(session, image, HistorySource::Scroll);
    };
    actions.historyUpdatedPair =
        [this, session](const QImage& image, const std::optional<QImage>& original)
    {
        if (!historyEnabled_ || session.id.isEmpty())
            return;
        if (watermarks_.retainOriginal() && original)
            history_.record(session, image, HistorySource::Scroll, *original);
        else
            history_.record(session, image, HistorySource::Scroll);
    };
    actions.loadSavePreferences = [this] { return settings_.loadSavePreferences(); };
    actions.pinImage = [this](const QImage& original)
    {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (!screen)
            return ImageOutputResult{false, QStringLiteral("未找到可用显示器，请重试。")};
        const QRect available = screen->availableGeometry();
        const QSize maximum(qRound(available.width() * 0.85 * screen->devicePixelRatio()),
                            qRound(available.height() * 0.85 * screen->devicePixelRatio()));
        const QImage scaled = scaledToFit(original, maximum);
        const auto result = stickers_.create(
            scaled, clipboardStickerPosition(
                        available, stickerWindowSize(scaled.size(), 1, screen->devicePixelRatio()),
                        scrollStickerSequence_));
        if (result.success)
            ++scrollStickerSequence_;
        refreshStickerActions();
        return result;
    };
    auto* result = new ScrollResultWindow(std::move(image), stats, std::move(actions));
    scrollResults_.append(result);
    connect(result, &QObject::destroyed, this,
            [this]
            {
                scrollResults_.erase(std::remove_if(scrollResults_.begin(), scrollResults_.end(),
                                                    [](const auto& window)
                                                    { return window.isNull(); }),
                                     scrollResults_.end());
            });
    connect(result, &ScrollResultWindow::retryRequested, this,
            [this, result, display, region, restoreFocus]
            {
                if (active_ || quitting_ || resolvingQuit_ || settingsOpen_)
                    return;
                retryDisplay_ = display;
                retryRegion_ = region;
                scrollRestoreFocus_ = restoreFocus;
                result->closeWithoutConfirmation();
                trigger(QStringLiteral("scroll-retry"));
            });
    const QRect available = display->availableLogicalGeometry.isEmpty()
                                ? display->logicalGeometry
                                : display->availableLogicalGeometry;
    result->resize(result->size().boundedTo(available.size()));
    result->move(available.center() - QPoint(result->width() / 2, result->height() / 2));
    result->show();
}
void ApplicationController::clearScrollCapture(int outcome)
{
    if (scrollFlowActive_)
    {
        const ScrollStitchStats stats =
            scrollSession_ ? scrollSession_->stats() : ScrollStitchStats{};
        qInfo().nospace() << "截长图结束：帧 " << stats.frames << "，接受 " << stats.accepted
                          << "，跳过 " << stats.skipped << "，警告 " << stats.warnings << "，长度 "
                          << stats.lengthPx << " px，终态 " << outcome << "，用时 "
                          << (scrollStartedNs_ ? (monotonicNs() - scrollStartedNs_) / 1000000 : 0)
                          << " ms";
    }
    scrollFlowActive_ = false;
    scrollStartedNs_ = 0;
    if (scrollSession_)
    {
        scrollSession_->stop();
        scrollSession_->disconnect();
        scrollSession_->deleteLater();
        scrollSession_ = nullptr;
    }
    for (QWidget* window :
         {static_cast<QWidget*>(scrollBorder_), static_cast<QWidget*>(scrollBar_)})
        if (window)
        {
            window->hide();
            window->disconnect();
            window->deleteLater();
        }
    scrollBorder_ = nullptr;
    scrollBar_ = nullptr;
}
void ApplicationController::layoutChanged()
{
    if (recording_)
        recording_->topologyChanged();
    if (!active_)
        return;
    if (scrollSession_)
        scrollSession_->interrupt(QStringLiteral("显示器布局已变化，采集已中断，已保留当前结果。"));
    else
        finish({}, scrollFlowActive_ ? scrollCancelledOutcome : 4);
}
bool ApplicationController::resolveScrollResultsForQuit()
{
    for (const auto& window : scrollResults_)
        if (window && !window->hasOutput())
        {
            QMessageBox dialog(QMessageBox::Question, QStringLiteral("退出 WaibuSnap"),
                               QStringLiteral("有未保存的长图，退出将丢弃。"),
                               QMessageBox::NoButton, nullptr);
            auto* discard =
                dialog.addButton(QStringLiteral("放弃并退出"), QMessageBox::DestructiveRole);
            auto* cancel = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
            dialog.setDefaultButton(cancel);
            dialog.exec();
            return dialog.clickedButton() == discard;
        }
    return true;
}
void ApplicationController::finish(QRect pixels, int outcome)
{
    if (!active_)
        return;
    ++token_;
    windowObservations_.clear();
    invisibleSinceNs_ = 0;
    visibilityRecoveryAttempts_ = 0;
    clearScrollCapture(outcome);
    retryDisplay_.reset();
    retryRegion_ = {};
    scrollRestoreFocus_ = {};
    captureTimeout_.stop();
    sessionWatchdog_.stop();
    observation_.reset();
    metrics_.width = pixels.width();
    metrics_.height = pixels.height();
    metrics_.outcome = outcome;
    if (overlay_)
    {
        overlay_->disconnect(this);
        overlay_->hide();
        // 延迟销毁避开正在执行的窗口事件；销毁后不留冻结像素或隐藏窗口。
        overlay_->deleteLater();
        overlay_ = nullptr;
    }
    const bool written = appendMetrics(options_.metricsFile, metrics_);
    active_ = false;
    qInfo() << "截图会话结束：outcome=" << outcome << "，用时 "
            << (monotonicNs() - metrics_.t0) / 1000000 << " ms";
    settingsAction_->setEnabled(!quitting_);
    if (clipboardToast_)
        clipboardToast_->close();
    refreshStickerActions();
    if (!written)
        qCritical("无法写入会话测量日志。");
    if (options_.testMode && !quitting_)
    {
        if (!written || !metrics_.interactive)
            QTimer::singleShot(0, &application_, [this] { application_.exit(3); });
        else if (sequence_ >= options_.testCount)
            QTimer::singleShot(0, this, &ApplicationController::quit);
        else
            QTimer::singleShot(options_.intervalMs, this,
                               [this] { trigger(QStringLiteral("injected")); });
    }
}
void ApplicationController::fail(int outcome, const QString& explanation)
{
    if (options_.testMode)
    {
        qWarning().noquote() << explanation;
        finish({}, outcome);
        return;
    }
    qWarning().noquote() << explanation;
    // 2026-10-13 防御：层级 25 的全屏覆盖层会遮住独立模态窗口，先隐藏再提示。
    if (overlay_)
        overlay_->hide();
    // 对话框期间仍持有会话锁，避免连续 F1 堆叠提示。
    QMessageBox::information(nullptr, QStringLiteral("截图未完成"), explanation);
    finish({}, outcome);
}
bool ApplicationController::visibilityRecoveryBlocked() const
{
    return QApplication::activeModalWidget() || resolvingQuit_ || quitting_ ||
           (overlay_ && overlay_->systemPanelOpen());
}
void ApplicationController::watchOverlayWindow(QWidget* window, bool activate)
{
    const auto token = token_;
    const QPointer<QWidget> target(window);
    auto observation = observeOverlayWindow(window->windowHandle(), this,
                                            [this, token, target, activate](const QString& reason)
                                            {
                                                if (active_ && token == token_ && target)
                                                    recoverOverlayWindow(target, activate, reason);
                                            });
    if (observation)
        windowObservations_.push_back(std::move(observation));
}
void ApplicationController::recoverOverlayWindow(QWidget* window, bool activate,
                                                 const QString& reason)
{
    if (!window || !window->isVisible() || visibilityRecoveryBlocked())
        return;
    const auto before = visibilityQuery_(window->windowHandle());
    const bool ordered = visibilityRecovery_(window->windowHandle(), activate && !before.onScreen);
    ++visibilityRecoveryAttempts_;
    qInfo().noquote() << "覆盖层在屏自愈：reason=" << reason
                      << " attempt=" << visibilityRecoveryAttempts_ << " ordered=" << ordered
                      << " before:" << before.diagnostic()
                      << " after:" << visibilityQuery_(window->windowHandle()).diagnostic();
    // 即刻结果仅供诊断；是否持续恢复必须在下一次看门狗 / 呈现采样重新读取。
}
void ApplicationController::watchPresentation(quint64 token)
{
    if (!active_ || token != token_ || !overlay_ || scrollFlowActive_ || metrics_.interactive)
        return;
    observation_ =
        observePresentation(overlay_->windowHandle(), this, [this, token](PresentationSample sample)
                            { presentationCompleted(token, sample); });
}
void ApplicationController::presentationCompleted(quint64 token, PresentationSample sample)
{
    if (!active_ || token != token_ || !overlay_ || scrollFlowActive_ || metrics_.interactive)
        return;
    const auto visibility = visibilityQuery_(overlay_->windowHandle());
    if (!visibility.available || !visibility.onScreen || visibilityRecoveryBlocked())
    {
        qInfo().noquote() << "覆盖层尚未在屏就绪（保留超时保护）：" << visibility.diagnostic();
        if (!visibilityRecoveryBlocked() && !invisibleSinceNs_)
        {
            invisibleSinceNs_ = monotonicNs();
            recoverOverlayWindow(overlay_, true, QStringLiteral("就绪检查"));
        }
        // 恢复后重新观察刷新，不把旧采样当成新窗口的呈现证据。
        QTimer::singleShot(250, this, [this, token] { watchPresentation(token); });
        return;
    }
    if (!sample.available || !overlay_->isVisible() || !overlay_->isActiveWindow())
    {
        fail(8, QStringLiteral("未观察到可交互覆盖窗口，本次不计入有效性能样本。"));
        return;
    }
    invisibleSinceNs_ = 0;
    captureTimeout_.stop();
    qInfo().noquote() << "覆盖层可交互就绪（停止超时保护）" << visibility.diagnostic();
    metrics_.visibleProxy = sample.observedAtNs;
    metrics_.interactive = monotonicNs();
    metrics_.refreshIntervalMs = sample.refreshIntervalMs;
    if (options_.testMode)
        QTimer::singleShot(options_.holdMs, this,
                           [this, token]
                           {
                               if (active_ && token == token_)
                                   finish({}, 2);
                           });
}
void ApplicationController::checkSessionWatchdog()
{
    if (!active_ || scrollSession_)
        return;
    if (visibilityRecoveryBlocked())
    {
        invisibleSinceNs_ = 0;
        return;
    }
    const auto visibility = visibilityQuery_(overlay_ ? overlay_->windowHandle() : nullptr);
    const qint64 now = monotonicNs();
    const qint64 age = now - metrics_.t0;
    if (overlay_ && overlay_->isVisible())
    {
        if (visibility.available && visibility.onScreen)
        {
            if (invisibleSinceNs_)
                qInfo().noquote() << "截图会话看门狗确认在屏恢复：" << visibility.diagnostic();
            invisibleSinceNs_ = 0;
            return;
        }
        if (!invisibleSinceNs_)
            invisibleSinceNs_ = now;
        if (now - invisibleSinceNs_ < 20'000'000'000LL)
        {
            qWarning().noquote() << "截图会话看门狗发现真实在屏异常：" << visibility.diagnostic()
                                 << " 连续异常ms=" << (now - invisibleSinceNs_) / 1000000;
            recoverOverlayWindow(overlay_, true, QStringLiteral("会话看门狗"));
            return;
        }
    }
    else if (age < 20'000'000'000LL)
        return;
    qWarning().nospace().noquote()
        << "截图会话异常残留，已自动复位：active=" << active_
        << " overlay=" << (overlay_ != nullptr)
        << " overlayVisible=" << (overlay_ ? overlay_->isVisible() : false) << " "
        << visibility.diagnostic() << " observation=" << (observation_ != nullptr)
        << " captureTimeoutActive=" << captureTimeout_.isActive()
        << " settingsOpen=" << settingsOpen_ << " resolvingQuit=" << resolvingQuit_
        << " quitting=" << quitting_ << " 会话年龄ms=" << age / 1000000
        << " 连续异常ms=" << (invisibleSinceNs_ ? (now - invisibleSinceNs_) / 1000000 : 0)
        << " 自愈次数=" << visibilityRecoveryAttempts_;
    finish({}, 12);
}
void ApplicationController::cleanup()
{
    quitting_ = true;
    refreshStickerActions();
    clipboardToast_.reset();
    recordingToast_.reset();
    if (recording_)
        recording_->discard();
    if (historyWindow_)
    {
        auto* closing = historyWindow_.data();
        historyWindow_.clear();
        const bool nested = closing->dismissGroupDialogs();
        closing->close();
        // 先让对话框 / 菜单的嵌套循环和触发它们的控件信号退栈。
        if (nested)
            closing->deleteLater();
        else
            delete closing;
    }
    const QPointer<SelectionOverlay> closingOverlay = overlay_;
    if (active_)
        finish({}, scrollFlowActive_ ? scrollCancelledOutcome : 9);
    clearScrollCapture();
    const auto results = scrollResults_;
    scrollResults_.clear();
    for (const auto& result : results)
        if (result)
            delete result.data();
    // 所有退出入口共用清理；正在面板嵌套事件循环中的窗口也会被销毁。
    stickers_.closeAll();
    if (closingOverlay)
        delete closingOverlay.data();
    setQuickGunnerMode(false);
    quickGunnerHotkey_->deactivate();
    hotkey_->unregister();
    tray_.hide();
}
void ApplicationController::quit()
{
    if (quitting_ || resolvingQuit_)
        return;
    if (recordingActive())
    {
        RecordingController::QuitChoice choice = RecordingController::QuitChoice::Cancel;
        QVector<QPointer<QWidget>> hidden;
        bool saveFailed = false;
        {
            QScopedValueRollback<bool> resolving(resolvingQuit_, true);
            for (auto* window : QApplication::topLevelWidgets())
                if (window->isVisible() &&
                    (window->objectName() == QStringLiteral("recordingPicker") ||
                     window->objectName() == QStringLiteral("recordingCountdown")))
                {
                    hidden.append(window);
                    window->hide();
                }
            QMessageBox box(QMessageBox::Question, QStringLiteral("退出录制"),
                            QStringLiteral("录制尚未结束，请选择退出方式。"),
                            QMessageBox::NoButton);
            box.setObjectName(QStringLiteral("recordingQuitDialog"));
            connect(recording_.get(), &RecordingController::failed, &box,
                    [&saveFailed] { saveFailed = true; });
            auto* save = box.addButton(QStringLiteral("停止并保存后退出"), QMessageBox::AcceptRole);
            save->setObjectName(QStringLiteral("recordingQuitSave"));
            auto* discard =
                box.addButton(QStringLiteral("丢弃后退出"), QMessageBox::DestructiveRole);
            discard->setObjectName(QStringLiteral("recordingQuitDiscard"));
            auto* cancel = box.addButton(QStringLiteral("取消退出"), QMessageBox::RejectRole);
            cancel->setObjectName(QStringLiteral("recordingQuitCancel"));
            box.setDefaultButton(cancel);
            box.setEscapeButton(cancel);
            box.exec();
            if (box.clickedButton() == save)
                choice = RecordingController::QuitChoice::Save;
            else if (box.clickedButton() == discard)
                choice = RecordingController::QuitChoice::Discard;
        }
        if (choice == RecordingController::QuitChoice::Cancel)
        {
            for (auto window : hidden)
                if (window && recordingActive())
                {
                    window->show();
                    reassertOverlayWindow(window->windowHandle(), true);
                    window->setFocus();
                }
            return;
        }
        if (saveFailed && choice == RecordingController::QuitChoice::Save)
        {
            showRecordingNotice(QStringLiteral("录制保存失败，已取消退出。"));
            return;
        }
        recording_->quitRequested(choice);
        return;
    }
    const QPointer<SelectionOverlay> pausedOverlay = overlay_;
    const quint64 pausedToken = token_;
    const bool restoreOverlay = pausedOverlay && pausedOverlay->isVisible();
    const QPointer<ScrollBorderWindow> pausedBorder = scrollBorder_;
    const QPointer<ScrollControlBar> pausedBar = scrollBar_;
    const bool restoreBorder = pausedBorder && pausedBorder->isVisible();
    const bool restoreBar = pausedBar && pausedBar->isVisible();
    bool backlogCancelled = false;
    bool resolved = false;
    {
        QScopedValueRollback<bool> resolving(resolvingQuit_, true);
        refreshStickerActions();
        if (clipboardToast_)
            clipboardToast_->close();
        // 2026-10-13 同类防御：退出汇总与逐张保存的独立模态也可能被覆盖层遮住。
        if (restoreOverlay)
            pausedOverlay->hide();
        if (scrollBorder_)
            scrollBorder_->hide();
        if (scrollBar_)
            scrollBar_->hide();
        const int pending = quickGunner_->pendingCaptureCount() + quickGunner_->pendingSaveCount() +
                            int(quickGunner_->capturing());
        if (pending > 0)
        {
            QMessageBox box(
                QMessageBox::Warning, QStringLiteral("退出快枪手"),
                QStringLiteral("快枪手还有 %1 张图片待保存或重试，退出会丢失。").arg(pending),
                QMessageBox::NoButton);
            auto* cancel = box.addButton(QStringLiteral("取消退出"), QMessageBox::RejectRole);
            cancel->setObjectName(QStringLiteral("quickGunnerQuitCancel"));
            auto* discard = box.addButton(QStringLiteral("仍然退出"), QMessageBox::DestructiveRole);
            discard->setObjectName(QStringLiteral("quickGunnerQuitDiscard"));
            box.setDefaultButton(cancel);
            box.setEscapeButton(cancel);
            box.exec();
            backlogCancelled = box.clickedButton() != discard;
        }
        if (!backlogCancelled)
        {
            if (scrollFlowActive_)
                finish({}, scrollCancelledOutcome);
            resolved = resolveScrollResultsForQuit() && stickers_.resolveUnsavedForQuit();
        }
    }
    refreshStickerActions();
    if (!resolved)
    {
        if (backlogCancelled && active_ && token_ == pausedToken)
        {
            if (restoreBorder && scrollBorder_ == pausedBorder)
                pausedBorder->show();
            if (restoreBar && scrollBar_ == pausedBar)
                pausedBar->show();
        }
        // 嵌套事件循环可能已取消或销毁会话，不能复活旧覆盖层。
        if (restoreOverlay && active_ && token_ == pausedToken && overlay_ == pausedOverlay)
        {
            pausedOverlay->show();
            pausedOverlay->raise();
            pausedOverlay->activateWindow();
            pausedOverlay->setFocus(Qt::ActiveWindowFocusReason);
        }
        return;
    }
    cleanup();
    application_.quit();
}
void ApplicationController::startSmokeTest()
{
    // 无头环境无法显示系统托盘；仍构造真实菜单并验证窗口关闭不会退出事件循环。
    QTimer::singleShot(
        0, this,
        [this]
        {
            const auto action = [this](const char* name)
            { return menu_.findChild<QAction*>(QLatin1String(name)); };
            const auto* clipboard = action("clipboardStickerAction");
            const auto* settings = action("settingsAction");
            const auto* history = action("openHistoryAction");
            const auto* hide = action("hideAllStickersAction");
            const auto* restore = action("restoreAllStickersAction");
            if (tray_.icon().isNull() || tray_.icon().availableSizes().isEmpty() ||
                application_.quitOnLastWindowClosed() || !action("screenshotAction") ||
                !action("scrollCaptureAction") || !action("quitAction") || !clipboard ||
                clipboard->text() != QStringLiteral("剪贴板贴图") || !clipboard->isEnabled() ||
                !settings || settings->text() != QStringLiteral("设置…") || !history ||
                !history->isEnabled() || history->text() != QStringLiteral("打开历史…") || !hide ||
                !restore || hide->isEnabled() || restore->isEnabled() ||
                !QApplication::topLevelWidgets().contains(&menu_))
            {
                application_.exit(20);
                return;
            }
            auto* transient = new QWidget;
            transient->setAttribute(Qt::WA_DeleteOnClose);
            transient->show();
            QTimer::singleShot(
                50, this,
                [this, transient]
                {
                    if (!transient->isVisible())
                    {
                        application_.exit(21);
                        return;
                    }
                    transient->close();
                    QTimer::singleShot(
                        50, this,
                        [this]
                        {
                            const auto windows = QApplication::topLevelWidgets();
                            if (!windows.contains(&menu_) ||
                                std::any_of(windows.cbegin(), windows.cend(),
                                            [this](QWidget* window)
                                            {
                                                return window != &menu_ &&
                                                       window != currentGroupMenu_ &&
                                                       window != historyMenu_ &&
                                                       window != watermarkMenu_ &&
                                                       window != watermarkPresetMenu_ &&
                                                       window != quickGunnerMenu_ &&
                                                       window != recordingMenu_;
                                            }))
                            {
                                application_.exit(22);
                                return;
                            }
                            QTimer::singleShot(0, this,
                                               [this]
                                               {
                                                   auto* dialog = qobject_cast<SettingsDialog*>(
                                                       QApplication::activeModalWidget());
                                                   if (!dialog)
                                                   {
                                                       qCritical("冒烟验证未找到设置对话框。");
                                                       application_.exit(20);
                                                       return;
                                                   }
                                                   trigger(QStringLiteral("hotkey"));
                                                   if (active_)
                                                   {
                                                       qCritical("设置期间热键未被忽略。");
                                                       application_.exit(20);
                                                       return;
                                                   }
                                                   dialog->reject();
                                               });
                            settingsAction_->trigger();
                            if (settingsOpen_ || !settingsAction_->isEnabled())
                            {
                                qCritical("设置关闭后入口状态未恢复。");
                                application_.exit(20);
                                return;
                            }
                            QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
                            image.fill(Qt::red);
                            if (!stickers_.create(image, {20, 20}, true).success ||
                                !stickers_.create(image, {80, 80}, true).success)
                            {
                                application_.exit(23);
                                return;
                            }
                            const auto pinned = stickers_.windows();
                            qInfo("托盘生命周期已验证（tray-lifecycle-verified）");
                            menu_.findChild<QAction*>(QStringLiteral("quitAction"))->trigger();
                            if (stickers_.count() != 0 || pinned.at(0) || pinned.at(1))
                                application_.exit(24);
                            else
                                qInfo("退出贴图清理已验证（sticker-cleanup-verified）");
                        });
                });
        });
}
}

namespace waibusnap
{
bool ApplicationController::recordingActive() const { return recording_ && recording_->active(); }
void ApplicationController::showRecordingNotice(const QString& notice)
{
    if (!notice.isEmpty() && !options_.testMode && !options_.smokeTest)
        tray_.showMessage(QStringLiteral("录制"), notice, QSystemTrayIcon::Warning);
}
void ApplicationController::startRecordingSelection()
{
    if (recordingActive() || quitting_ || resolvingQuit_)
        return;
    if (active_ || settingsOpen_ || scrollFlowActive_ ||
        quickGunner_->state() == QuickGunnerController::State::Framing ||
        quickGunner_->capturing() || quickGunner_->pendingCaptureCount())
    {
        showRecordingNotice(QStringLiteral("请先结束当前截图、长图或快枪手会话。"));
        return;
    }
    if (recordingToast_)
        recordingToast_->hide();
    if (quickGunner_->flashWindow())
        quickGunner_->flashWindow()->hide();
    recording_->startSelection(settings_.loadRecordingPreferences());
}
void ApplicationController::toggleRecordingStartStop()
{
    if (settingsOpen_ || resolvingQuit_ || quitting_)
        return;
    if (!recordingActive())
        startRecordingSelection();
    else if (recording_->state() == RecordingController::State::Recording ||
             recording_->state() == RecordingController::State::Paused)
        recording_->stopAndSave();
}
void ApplicationController::toggleRecordingPause()
{
    if (resolvingQuit_ || quitting_)
        return;
    if (recording_->state() == RecordingController::State::Recording)
        recording_->pause();
    else if (recording_->state() == RecordingController::State::Paused)
        recording_->resume();
}
void ApplicationController::discardRecording()
{
    if (recording_->state() != RecordingController::State::Recording &&
        recording_->state() != RecordingController::State::Paused)
        return;
    QScopedValueRollback<bool> resolving(resolvingQuit_, true);
    QMessageBox box(QMessageBox::Warning, QStringLiteral("丢弃录制"),
                    QStringLiteral("丢弃这段录制？临时文件会被删除，无法恢复。"),
                    QMessageBox::NoButton);
    auto* cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    auto* discard = box.addButton(QStringLiteral("丢弃"), QMessageBox::DestructiveRole);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() == discard)
        recording_->discard();
}
void ApplicationController::refreshRecordingActions()
{
    if (!recordingMenu_)
        return;
    using State = RecordingController::State;
    const auto state = recording_->state();
    const bool idle = state == State::Idle;
    const bool running = state == State::Recording || state == State::Paused;
    const bool saving = state == State::Finalizing;
    const auto text = recording_->statusText();
    recordingStatusAction_->setText(
        QFontMetrics(menu_.font()).elidedText(text, Qt::ElideMiddle, 440));
    recordingStatusAction_->setToolTip(text);
    recordingStartAction_->setVisible(!running);
    const bool supported = recording_->captureSupported();
    recordingMenu_->setEnabled(supported);
    recordingStartAction_->setEnabled(idle && supported && !active_ && !settingsOpen_ &&
                                      !resolvingQuit_ && !quitting_);
    recordingStartAction_->setToolTip(supported
                                          ? QStringLiteral("选择区域、窗口或全屏录制视频")
                                          : QStringLiteral("录制将在 Windows 原生采集补齐后提供"));
    recordingPauseAction_->setVisible(running || saving);
    recordingPauseAction_->setEnabled(running);
    recordingPauseAction_->setText(state == State::Paused ? QStringLiteral("继续")
                                                          : QStringLiteral("暂停"));
    for (auto* action : {recordingStopAction_, recordingDiscardAction_})
    {
        action->setVisible(running || saving);
        action->setEnabled(running);
    }
    const bool browse = !active_ && !settingsOpen_ && !resolvingQuit_ && !quitting_;
    recordingOpenVideoAction_->setEnabled(browse);
    recordingOpenGifAction_->setEnabled(browse);
    recordingCancelAction_->setVisible(!idle && !running && !saving);
    recordingCancelAction_->setEnabled(!idle && !running && !saving);
    if (!supported)
    {
        const auto notice = QStringLiteral("录制将在 Windows 原生采集补齐后提供");
        recordingMenu_->menuAction()->setToolTip(notice);
        for (auto* action : recordingMenu_->actions())
        {
            action->setEnabled(false);
            action->setToolTip(notice);
        }
    }
    TrayIconState icon = TrayIconState::Idle;
    if (state == State::Paused)
        icon = TrayIconState::Paused;
    else if (state == State::Recording || saving)
        icon = TrayIconState::Recording;
    // 仅状态变化才替换原生图标，计时刷新不重复创建位图。
    if (tray_.property("recordingIconState").toInt() != int(icon) || tray_.icon().isNull())
    {
        tray_.setIcon(createTrayIcon(icon));
        tray_.setProperty("recordingIconState", int(icon));
    }
    const bool inputs = idle && !active_ && !settingsOpen_ && !resolvingQuit_ && !quitting_;
    if (auto* screenshot = menu_.findChild<QAction*>(QStringLiteral("screenshotAction")))
        screenshot->setEnabled(inputs);
    scrollAction_->setEnabled(inputs && regionCaptureSupported());
    if (settingsAction_)
        settingsAction_->setEnabled(inputs);
}
}
