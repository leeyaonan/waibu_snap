#pragma once
#include "app/app_settings.h"
#include "interfaces/capture_provider.h"
#include <QObject>
#include <QPointer>
#include <QStringList>
class QWidget;
namespace waibusnap
{
class QuickGunnerRegionPicker;
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
        // B2 再引入 Capturing / Saving 及其异步生命周期。
    };
    Q_ENUM(State)
    QuickGunnerController(AppSettings settings, DisplayTopology& displays, CaptureProvider& capture,
                          RegionPickerFactory pickerFactory = {}, QObject* parent = nullptr);
    ~QuickGunnerController() override;
    bool active() const { return state_ == State::Active; }
    State state() const { return state_; }
    const QuickGunnerPreferences& preferences() const { return preferences_; }
    QString setDirectory(const QString& directory, const QStringList& reservedDirectories);
    QString beginFraming(QWidget* parent = nullptr);
    QString activate(const QStringList& reservedDirectories);
    void deactivate();
  signals:
    void stateChanged(waibusnap::QuickGunnerController::State state);
    void preferencesChanged();
    void noticeChanged(QString notice);

  private:
    void setState(State state);
    void finishFraming(const QString& notice = {});
    AppSettings settings_;
    DisplayTopology& displays_;
    CaptureProvider& capture_;
    RegionPickerFactory pickerFactory_;
    QuickGunnerPreferences preferences_;
    State state_ = State::Idle;
    bool resumeActive_ = false;
    quint64 token_ = 0;
    QPointer<QuickGunnerRegionPicker> picker_;
};
}
