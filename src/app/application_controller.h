#pragma once
#include "app/app_settings.h"
#include "app/history_manager.h"
#include "app/scroll_capture.h"
#include "app/sticker_manager.h"
#include "interfaces/autostart.h"
#include "interfaces/capture_provider.h"
#include "interfaces/file_manager.h"
#include "interfaces/global_hotkey.h"
#include "interfaces/overlay_window_behavior.h"
#include "interfaces/presentation_observer.h"
#include "interfaces/window_enumerator.h"
#include "interfaces/window_visibility.h"
#include "session/session_metrics.h"
#include "ui/clipboard_sticker_toast.h"
#include "ui/history_window.h"
#include "ui/scroll_capture_windows.h"
#include "ui/scroll_result_window.h"
#include "ui/selection_overlay.h"
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>
#include <optional>
#include <vector>
namespace waibusnap
{
struct ClipboardActions
{
    std::function<QImage()> loadImage;
};
struct RunOptions
{
    QString metricsFile;
    QString settingsFile;
    QString historyRoot;
    // 仅受控测试使用；不写回用户历史开关。
    std::optional<bool> historyEnabledOverride;
    bool testMode = false;
    bool smokeTest = false;
    int testCount = 1;
    int stableMs = 10000;
    int intervalMs = 1000;
    int holdMs = 250;
};
class ApplicationController final : public QObject
{
    Q_OBJECT
  public:
    explicit ApplicationController(QApplication& application, RunOptions options,
                                   StickerActions stickerActions = {},
                                   ClipboardActions clipboardActions = {},
                                   AutostartActions autostartActions = {},
                                   HistoryWindowActions historyActions = {},
                                   std::shared_ptr<FileManager> fileManager = {},
                                   std::unique_ptr<GlobalHotkey> hotkey = {});
    ~ApplicationController() override;
    void start();
    void trigger(const QString& source);
    void quit();
    void openHistory();
    void navigateHistory(int step);
    QMenu* menu() { return &menu_; }
    bool active() const { return active_; }
    StickerManager& stickers() { return stickers_; }

  private:
    // 冒烟仅通过友元注入会话状态，不开放生产运行参数。
    friend struct ApplicationControllerTestAccess;
    void bindScrollOverlay(DisplayTarget display);
    void beginScrollCapture(RegionCaptureRequest request);
    void finishScrollCapture();
    void clearScrollCapture(int outcome = scrollCancelledOutcome);
    bool resolveScrollResultsForQuit();
    void layoutChanged();
    void cleanup();
    void captureCompleted(quint64 token, CaptureResult result);
    void finish(QRect pixels, int outcome);
    void fail(int outcome, const QString& explanation);
    void checkSessionWatchdog();
    void watchPresentation(quint64 token);
    void presentationCompleted(quint64 token, PresentationSample sample);
    void watchOverlayWindow(QWidget* window, bool activate);
    void recoverOverlayWindow(QWidget* window, bool activate, const QString& reason);
    bool visibilityRecoveryBlocked() const;
    void startSmokeTest();
    void openSettings();
    void refreshStickerActions();
    void setHistoryMenusInstalled(bool installed);
    void rebuildCurrentGroupMenu();
    void updateCurrentGroupMenu();
    void pinClipboardImage();
    ImageOutputResult pinHistoryImage(const QImage& image);
    QString changeHotkey(const QKeySequence& sequence,
                         HotkeyAction action = HotkeyAction::Screenshot);
    HotkeySettings& hotkeySettings(HotkeyAction action);
    QApplication& application_;
    RunOptions options_;
    QAction* scrollAction_ = nullptr;
    QAction* settingsAction_ = nullptr;
    QAction* openHistoryAction_ = nullptr;
    QAction* historyPreviousAction_ = nullptr;
    QAction* historyNextAction_ = nullptr;
    QMenu* historyMenu_ = nullptr;
    QAction* historyDisabledAction_ = nullptr;
    QAction* currentGroupDisabledAction_ = nullptr;
    bool historyMenusInstalled_ = true;
    QAction* clipboardStickerAction_ = nullptr;
    QAction* hideAllStickersAction_ = nullptr;
    QAction* restoreAllStickersAction_ = nullptr;
    QMenu menu_;
    QMenu* currentGroupMenu_ = nullptr;
    QSystemTrayIcon tray_;
    std::unique_ptr<GlobalHotkey> hotkey_;
    AppSettings settings_;
    HistoryManager history_;
    bool historyEnabled_ = false;
    HistorySession historySession_;
    HotkeySettings hotkeySettings_;
    std::array<std::unique_ptr<HotkeySettings>, 3> historyHotkeys_;
    std::unique_ptr<DisplayTopology> displays_;
    std::unique_ptr<CaptureProvider> capture_;
    std::unique_ptr<WindowEnumerator> windows_;
    std::unique_ptr<PresentationObservation> observation_;
    StickerManager stickers_;
    ClipboardActions clipboardActions_;
    AutostartActions autostartActions_;
    HistoryWindowActions historyActions_;
    std::shared_ptr<FileManager> fileManager_;
    quint64 historyStickerSequence_ = 0;
    std::unique_ptr<ClipboardStickerToast> clipboardToast_;
    quint64 clipboardStickerSequence_ = 0;
    QPointer<SelectionOverlay> overlay_;
    QPointer<HistoryWindow> historyWindow_;
    QPointer<ScrollCaptureSession> scrollSession_;
    QPointer<ScrollBorderWindow> scrollBorder_;
    QPointer<ScrollControlBar> scrollBar_;
    QVector<QPointer<ScrollResultWindow>> scrollResults_;
    std::function<ScrollCaptureSession*(RegionCaptureRequest, QRect, QObject*)> scrollFactory_;
    std::function<SelectionOverlay*(CaptureFrame, OverlayActions, QVector<QRect>, QString)>
        overlayFactory_;
    std::function<void()> scrollRestoreFocus_;
    std::optional<DisplayTarget> retryDisplay_;
    QRect retryRegion_;
    bool scrollFlowActive_ = false;
    qint64 scrollStartedNs_ = 0;
    quint64 scrollStickerSequence_ = 0;
    std::function<WindowVisibility(QWindow*)> visibilityQuery_ = queryWindowVisibility;
    std::function<bool(QWindow*, bool)> visibilityRecovery_ = reassertOverlayWindow;
    std::vector<std::unique_ptr<OverlayWindowObservation>> windowObservations_;
    qint64 invisibleSinceNs_ = 0;
    int visibilityRecoveryAttempts_ = 0;
    SessionMetrics metrics_;
    QTimer captureTimeout_;
    QTimer sessionWatchdog_;
    int sequence_ = 0;
    quint64 token_ = 0;
    bool active_ = false;
    bool quitting_ = false;
    bool resolvingQuit_ = false;
    bool settingsOpen_ = false;
};
}
