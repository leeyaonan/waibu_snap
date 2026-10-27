#pragma once
#include "app/app_settings.h"
#include "interfaces/capture_provider.h"
#include "interfaces/region_capture.h"
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <deque>
class QWidget;
namespace waibusnap
{
class QuickGunnerRegionPicker;
class QuickGunnerSaveQueue;
class QuickGunnerFlashWindow;
struct QuickGunnerDependencies
{
    std::function<std::unique_ptr<RegionCaptureOnce>()> captureFactory;
    std::function<QImage(const QImage&)> watermark;
    std::unique_ptr<QuickGunnerSaveQueue> saveQueue;
};
using RegionPickerFactory = std::function<QuickGunnerRegionPicker*(CaptureFrame, QWidget*)>;
class QuickGunnerController final : public QObject
{
    Q_OBJECT
  public:
    enum class State
    {
        Idle,
        Framing,
        Active
    };
    Q_ENUM(State)
    QuickGunnerController(AppSettings settings, DisplayTopology& displays, CaptureProvider& capture,
                          RegionPickerFactory pickerFactory = {}, QObject* parent = nullptr);
    QuickGunnerController(AppSettings settings, DisplayTopology& displays, CaptureProvider& capture,
                          RegionPickerFactory pickerFactory, QuickGunnerDependencies dependencies,
                          QObject* parent = nullptr);
    ~QuickGunnerController() override;
    QString shutter();
    bool capturing() const { return capturing_; }
    int pendingCaptureCount() const { return int(pending_.size()); }
    int pendingSaveCount() const;
    QuickGunnerFlashWindow* flashWindow() const { return flash_.get(); }
    bool active() const { return state_ == State::Active; }
    State state() const { return state_; }
    const QuickGunnerPreferences& preferences() const { return preferences_; }
    QString setDirectory(const QString& directory, const QStringList& reservedDirectories);
    QString beginFraming(QWidget* parent = nullptr, bool activateAfterSelection = false);
    QString activate(const QStringList& reservedDirectories);
    void deactivate();
  signals:
    void shotCaptured(QRect logicalRegion);
    void shotFailed(QString error);
    void shotsDropped(int count);
    void stateChanged(waibusnap::QuickGunnerController::State state);
    void preferencesChanged();
    void noticeChanged(QString notice);

  private:
    void pumpCapture();
    void setState(State state);
    void finishFraming(const QString& notice = {});
    AppSettings settings_;
    DisplayTopology& displays_;
    CaptureProvider& capture_;
    RegionPickerFactory pickerFactory_;
    QuickGunnerPreferences preferences_;
    State state_ = State::Idle;
    bool resumeActive_ = false;
    bool activateAfter_ = false;
    bool framingAccepted_ = false;
    quint64 token_ = 0;
    QPointer<QuickGunnerRegionPicker> picker_;
    std::function<std::unique_ptr<RegionCaptureOnce>()> captureFactory_;
    std::function<QImage(const QImage&)> watermark_;
    std::unique_ptr<QuickGunnerSaveQueue> saveQueue_;
    std::unique_ptr<QuickGunnerFlashWindow> flash_;
    std::unique_ptr<RegionCaptureOnce> once_;
    std::deque<RegionCaptureRequest> pending_;
    bool capturing_ = false;
    quint64 captureToken_ = 0;
};
}
