#pragma once
#include "core/aspect_ratio.h"
#include "core/history_types.h"
#include "core/shape_mask.h"
#include "core/watermark.h"
#include "interfaces/global_hotkey.h"
#include "output/image_output.h"
#include <QKeySequence>
#include <QString>
#include <optional>
namespace waibusnap
{
struct LoadedHotkey
{
    QKeySequence sequence;
    QString explanation;
};
struct HistoryPreferences
{
    // 空路径表示使用 Pictures/WaibuSnap；读取时解析为默认绝对路径。
    QString rootDirectory;
    QString currentGroupId;
    CleanupPolicy defaultCleanup;
    // 隐私开关：历史截图保存默认关闭；只有显式开启后才初始化目录 / 扫描 / 入库。
    bool enabled = false;
};
struct HistoryWindowState
{
    QByteArray geometry;
    QByteArray splitterState;
    QString lastView = QStringLiteral("all");
    QString lastSelection;
    // 空值为各视图默认顺序；其余为 oldest / newest。
    QString sortOrder;
};
class AppSettings
{
  public:
    explicit AppSettings(QString filePath = {});
    HistoryPreferences loadHistoryPreferences(QString* notice = nullptr) const;
    QString saveHistoryPreferences(const HistoryPreferences& preferences) const;
    QString saveHistoryCurrentGroupId(const QString& groupId) const;
    HistoryWindowState loadHistoryWindowState() const;
    QString saveHistoryWindowState(const HistoryWindowState& state) const;
    LoadedHotkey loadHotkey(HotkeyAction action = HotkeyAction::Screenshot) const;
    SavePreferences loadSavePreferences() const;
    bool loadStickerCloseReminder() const;
    QString saveStickerCloseReminder(bool enabled) const;
    // nullopt 表示从未配置过（首次双击提示的触发条件）；false / true 为显式选择。
    std::optional<bool> loadStickerDoubleClickClose() const;
    QString saveStickerDoubleClickClose(bool enabled) const;
    WatermarkPreferences loadWatermarkPreferences(QString* notice = nullptr) const;
    QString saveWatermarkPreferences(const WatermarkPreferences& preferences) const;
    CaptureRatioSettings loadCaptureRatios() const;
    QString shapeTemplateDir() const;
    QVector<ShapeTemplateEntry> loadShapeTemplates(QString* notice = nullptr) const;
    ShapeTemplateImport importShapeTemplates(const QStringList& paths) const;
    QString saveShapeTemplates(const QVector<ShapeTemplateEntry>& entries) const;
    QString removeShapeTemplate(const QString& file) const;
    QString saveCaptureRatios(const CaptureRatioSettings& ratios) const;
    QString saveSavePreferences(const SavePreferences& preferences) const;
    // 注册成功后才调用；失败返回中文原因，不声称重启后已保留。
    QString saveHotkey(const QKeySequence& sequence,
                       HotkeyAction action = HotkeyAction::Screenshot) const;

  private:
    QString filePath_;
    mutable bool stickerCloseReminderWriteFailed_ = false;
    mutable bool stickerDoubleClickCloseWriteFailed_ = false;
    mutable bool historyWriteFailed_ = false;
    mutable bool historyWindowWriteFailed_ = false;
    mutable bool watermarkWriteFailed_ = false;
};
struct HotkeyStartup
{
    QString report;
    QString warning;
};
// 注册与落盘协调独立于窗口和托盘，允许注入热键替身验证失败路径。
class HotkeySettings
{
  public:
    HotkeySettings(AppSettings settings, GlobalHotkey& hotkey, std::function<void()> handler,
                   HotkeyAction action = HotkeyAction::Screenshot);
    HotkeyStartup initialize();
    QString changeHotkey(const QKeySequence& sequence);
    bool enabled() const { return !enabledSequence_.isEmpty(); }
    QKeySequence currentSequence() const;
    QString explanation() const { return explanation_; }
    QString tooltip() const;

  private:
    HotkeyRegistration registerSequence(const QKeySequence& sequence);
    HotkeyAction action_;
    AppSettings settings_;
    GlobalHotkey& hotkey_;
    std::function<void()> handler_;
    QKeySequence preferredSequence_;
    QKeySequence enabledSequence_;
    QString explanation_;
};
}
