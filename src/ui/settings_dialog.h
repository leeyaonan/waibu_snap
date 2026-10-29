#pragma once
#include "app/app_settings.h"
#include "core/aspect_ratio.h"
#include "core/shape_mask.h"
#include "interfaces/autostart.h"
#include "output/image_output.h"
#include "ui/recording_settings_page.h"
#include "ui/watermark_settings_page.h"
#include <QDialog>
#include <QKeySequence>
#include <functional>
#include <optional>
class QComboBox;
class QDoubleSpinBox;
class QScrollArea;
class QCheckBox;
class QLineEdit;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeWidget;
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
    // nullopt = 从未配置；关闭设置页时不应把未配置误写成 false。
    std::optional<bool> stickerDoubleClickClose;
    std::function<QString(bool)> saveStickerDoubleClickClose;
    QKeySequence quickGunnerHotkey = QKeySequence(Qt::Key_F2);
    std::function<QString(const QKeySequence&)> saveQuickGunnerHotkey;
    std::array<QKeySequence, 2> recordingHotkeys = {QKeySequence(Qt::Key_F3),
                                                    QKeySequence(Qt::Key_F4)};
    std::function<QString(HotkeyAction, const QKeySequence&)> saveRecordingHotkey;
};
struct HistorySettingsActions
{
    HistoryPreferences preferences;
    bool enabled = false;
    std::array<QKeySequence, 3> hotkeys = {QKeySequence(Qt::Key_F7), QKeySequence(Qt::Key_F8),
                                           QKeySequence(Qt::Key_F9)};
    GroupUsage usage;
    QString usageNotice;
    QVector<HistoryGroup> groups;
    QString currentGroupId;
    std::function<QString(const HistoryPreferences&)> savePreferences;
    std::function<QString(HotkeyAction, const QKeySequence&)> saveHotkey;
    std::function<QString(QWidget*, const QString&)> chooseDirectory;
    std::function<void(QWidget*)> createGroup;
    std::function<void(QWidget*, const QString&)> renameGroup;
    std::function<void(QWidget*, const QString&)> changeGroupDirectory;
    std::function<void(QWidget*, const QString&)> openGroupSettings;
    std::function<void(QWidget*, const QString&)> setCurrentGroup;
    std::function<void(QWidget*, const QString&)> deleteGroup;
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
                            HistorySettingsActions historySettings = {},
                            WatermarkSettingsActions watermarkSettings = {},
                            RecordingSettingsActions recordingSettings = {});

    void updateHistoryUsage(GroupUsage usage, const QString& notice = {});
    void updateHistoryGroups(const QVector<HistoryGroup>& groups, const QString& currentGroupId);

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void save();
    bool confirmHistoryEnable();
    void buildHistoryPage(QWidget* page);
    void buildGroupsPage(QWidget* page);
    void rebuildGroupList();
    QString selectedGroupId() const;
    void updateGroupButtons();
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
    QKeySequenceEdit* quickGunnerEditor_ = nullptr;
    std::array<QKeySequenceEdit*, 2> recordingEditors_{};
    QLabel* historyUsage_ = nullptr;
    QLabel* status_ = nullptr;
    SaveHotkey saveHotkey_;
    QKeySequence originalSequence_;
    SaveSettingsActions saveSettings_;
    QLineEdit* directory_ = nullptr;
    QComboBox* format_ = nullptr;
    AutostartActions autostartActions_;
    QCheckBox* autostartCheck_ = nullptr;
    QCheckBox* stickerCloseReminderCheck_ = nullptr;
    QCheckBox* stickerDoubleClickCloseCheck_ = nullptr;
    HistorySettingsActions historySettings_;
    WatermarkSettingsActions watermarkSettings_;
    WatermarkSettingsPage* watermarkPage_ = nullptr;
    RecordingSettingsActions recordingSettings_;
    RecordingSettingsPage* recordingPage_ = nullptr;
    QLineEdit* historyDirectory_ = nullptr;
    QCheckBox* historyEnabledCheck_ = nullptr;
    QPushButton* historyGroupNew_ = nullptr;
    std::array<QKeySequenceEdit*, 3> historyEditors_{};
    std::array<QCheckBox*, 3> historyRules_{};
    QSpinBox* historyDays_ = nullptr;
    QSpinBox* historyCount_ = nullptr;
    QDoubleSpinBox* historyMiB_ = nullptr;
    double originalHistoryMiB_ = 100;
    QVector<HistoryGroup> historyGroups_;
    QString historyCurrentGroupId_;
    QTreeWidget* historyGroupList_ = nullptr;
    QLabel* historyGroupNotice_ = nullptr;
    QPushButton* historyGroupRename_ = nullptr;
    QPushButton* historyGroupDirectory_ = nullptr;
    QPushButton* historyGroupSettings_ = nullptr;
    QPushButton* historyGroupCurrent_ = nullptr;
    QPushButton* historyGroupDelete_ = nullptr;
};
}
