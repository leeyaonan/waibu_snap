#pragma once
#include "core/aspect_ratio.h"
#include "interfaces/autostart.h"
#include "output/image_output.h"
#include <QDialog>
#include <QKeySequence>
#include <functional>
class QComboBox;
class QDoubleSpinBox;
class QScrollArea;
class QCheckBox;
class QLineEdit;
class QKeySequenceEdit;
class QLabel;
namespace waibusnap
{
struct SaveSettingsActions
{
    SavePreferences preferences;
    std::function<QString(const SavePreferences&)> savePreferences;
    std::function<QString(QWidget*, const QString&)> chooseDirectory;
    CaptureRatioSettings ratios{defaultCaptureRatios(), {}, AspectRatio{1, 1}};
    std::function<QString(const CaptureRatioSettings&)> saveRatios;
};
class SettingsDialog final : public QDialog
{
    Q_OBJECT
  public:
    using SaveHotkey = std::function<QString(const QKeySequence&)>;
    explicit SettingsDialog(const QKeySequence& current, SaveHotkey save,
                            const QString& notice = {}, QWidget* parent = nullptr,
                            SaveSettingsActions saveSettings = {},
                            AutostartActions autostartActions = {});

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void save();
    void rebuildRatioList();
    void refreshQuickRatios();
    CaptureRatioSettings collectRatios() const;
    struct RatioRow
    {
        AspectRatio ratio;
        bool enabled = true;
    };
    QVector<RatioRow> ratioRows_;
    std::optional<AspectRatio> quickRatio_;
    QScrollArea* ratioScroll_ = nullptr;
    QComboBox* quickRatioCombo_ = nullptr;
    QLabel* ratioEmptyNotice_ = nullptr;
    QDoubleSpinBox* ratioWidthInput_ = nullptr;
    QDoubleSpinBox* ratioHeightInput_ = nullptr;
    QKeySequenceEdit* editor_ = nullptr;
    QLabel* status_ = nullptr;
    SaveHotkey saveHotkey_;
    QKeySequence originalSequence_;
    SaveSettingsActions saveSettings_;
    QLineEdit* directory_ = nullptr;
    QComboBox* format_ = nullptr;
    AutostartActions autostartActions_;
    QCheckBox* autostartCheck_ = nullptr;
};
}
