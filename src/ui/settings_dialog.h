#pragma once
#include "app/app_settings.h"
#include "core/aspect_ratio.h"
#include "core/shape_mask.h"
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
class QSpinBox;
namespace waibusnap
{
struct SaveSettingsActions
{
    SavePreferences preferences;
    std::function<QString(const SavePreferences&)> savePreferences;
    std::function<QString(QWidget*, const QString&)> chooseDirectory;
    CaptureRatioSettings ratios{defaultCaptureRatios(), {}, AspectRatio{1, 1}};
    std::function<QString(const CaptureRatioSettings&)> saveRatios;
    QVector<ShapeTemplateEntry> shapeTemplates;
    QString shapeTemplateDir;
    std::function<QString(const QVector<ShapeTemplateEntry>&)> saveShapeTemplates;
    std::function<ShapeTemplateImport(const QStringList&)> importShapeTemplates;
    std::function<QStringList(QWidget*)> chooseShapeTemplatePaths;
    bool stickerCloseReminder = true;
    std::function<QString(bool)> saveStickerCloseReminder;
};
struct HistorySettingsActions
{
    HistoryPreferences preferences;
    std::array<QKeySequence, 3> hotkeys = {QKeySequence(Qt::Key_F7), QKeySequence(Qt::Key_F8),
                                           QKeySequence(Qt::Key_F9)};
    GroupUsage usage;
    QString usageNotice;
    std::function<QString(const HistoryPreferences&)> savePreferences;
    std::function<QString(HotkeyAction, const QKeySequence&)> saveHotkey;
    std::function<QString(QWidget*, const QString&)> chooseDirectory;
};
class SettingsDialog final : public QDialog
{
    Q_OBJECT
  public:
    using SaveHotkey = std::function<QString(const QKeySequence&)>;
    explicit SettingsDialog(const QKeySequence& current, SaveHotkey save,
                            const QString& notice = {}, QWidget* parent = nullptr,
                            SaveSettingsActions saveSettings = {},
                            AutostartActions autostartActions = {},
                            HistorySettingsActions historySettings = {});

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void save();
    void buildHistoryPage(QWidget* page);
    HistoryPreferences collectHistoryPreferences() const;
    void rebuildRatioList();
    void rebuildShapeTemplateList();
    void refreshQuickRatios();
    CaptureRatioSettings collectRatios() const;
    struct RatioRow
    {
        AspectRatio ratio;
        bool enabled = true;
    };
    QVector<ShapeTemplateEntry> shapeTemplates_;
    QScrollArea* shapeTemplateScroll_ = nullptr;
    QLabel* shapeTemplateEmptyNotice_ = nullptr;
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
    QCheckBox* stickerCloseReminderCheck_ = nullptr;
    HistorySettingsActions historySettings_;
    QLineEdit* historyDirectory_ = nullptr;
    std::array<QKeySequenceEdit*, 3> historyEditors_{};
    std::array<QCheckBox*, 3> historyRules_{};
    QSpinBox* historyDays_ = nullptr;
    QSpinBox* historyCount_ = nullptr;
    QDoubleSpinBox* historyMiB_ = nullptr;
    double originalHistoryMiB_ = 100;
};
}
