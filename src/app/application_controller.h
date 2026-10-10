#pragma once
#include "app/app_settings.h"
#include "app/sticker_manager.h"
#include "interfaces/autostart.h"
#include "interfaces/capture_provider.h"
#include "interfaces/global_hotkey.h"
#include "interfaces/presentation_observer.h"
#include "interfaces/window_enumerator.h"
#include "session/session_metrics.h"
#include "ui/clipboard_sticker_toast.h"
#include "ui/selection_overlay.h"
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>
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
                                   AutostartActions autostartActions = {});
    ~ApplicationController() override;
    void start();
    void trigger(const QString& source);
    void quit();
    QMenu* menu() { return &menu_; }
    bool active() const { return active_; }
    StickerManager& stickers() { return stickers_; }

  private:
    void cleanup();
    void captureCompleted(quint64 token, CaptureResult result);
    void finish(QRect pixels, int outcome);
    void fail(int outcome, const QString& explanation);
    void startSmokeTest();
    void openSettings();
    void refreshStickerActions();
    void pinClipboardImage();
    QString changeHotkey(const QKeySequence& sequence);
    QApplication& application_;
    RunOptions options_;
    QAction* settingsAction_ = nullptr;
    QAction* clipboardStickerAction_ = nullptr;
    QAction* hideAllStickersAction_ = nullptr;
    QAction* restoreAllStickersAction_ = nullptr;
    QMenu menu_;
    QSystemTrayIcon tray_;
    std::unique_ptr<GlobalHotkey> hotkey_;
    AppSettings settings_;
    HotkeySettings hotkeySettings_;
    std::unique_ptr<DisplayTopology> displays_;
    std::unique_ptr<CaptureProvider> capture_;
    std::unique_ptr<WindowEnumerator> windows_;
    std::unique_ptr<PresentationObservation> observation_;
    StickerManager stickers_;
    ClipboardActions clipboardActions_;
    AutostartActions autostartActions_;
    std::unique_ptr<ClipboardStickerToast> clipboardToast_;
    quint64 clipboardStickerSequence_ = 0;
    QPointer<SelectionOverlay> overlay_;
    SessionMetrics metrics_;
    QTimer captureTimeout_;
    int sequence_ = 0;
    quint64 token_ = 0;
    bool active_ = false;
    bool quitting_ = false;
    bool resolvingQuit_ = false;
    bool settingsOpen_ = false;
};
}
