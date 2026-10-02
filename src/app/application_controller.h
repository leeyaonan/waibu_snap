#pragma once
#include "interfaces/capture_provider.h"
#include "interfaces/global_hotkey.h"
#include "interfaces/presentation_observer.h"
#include "session/session_metrics.h"
#include "ui/selection_overlay.h"
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>
namespace waibusnap
{
struct RunOptions
{
    QString metricsFile;
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
    explicit ApplicationController(QApplication& application, RunOptions options);
    void start();
    void trigger(const QString& source);
    void quit();
    QMenu* menu() { return &menu_; }
    bool active() const { return active_; }

  private:
    void captureCompleted(quint64 token, CaptureResult result);
    void finish(QRect pixels, int outcome);
    void fail(int outcome, const QString& explanation);
    void startSmokeTest();
    QApplication& application_;
    RunOptions options_;
    QMenu menu_;
    QSystemTrayIcon tray_;
    std::unique_ptr<GlobalHotkey> hotkey_;
    std::unique_ptr<DisplayTopology> displays_;
    std::unique_ptr<CaptureProvider> capture_;
    std::unique_ptr<PresentationObservation> observation_;
    QPointer<SelectionOverlay> overlay_;
    SessionMetrics metrics_;
    QTimer captureTimeout_;
    int sequence_ = 0;
    quint64 token_ = 0;
    bool active_ = false;
    bool quitting_ = false;
};
}
