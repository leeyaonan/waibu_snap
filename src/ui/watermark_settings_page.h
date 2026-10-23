#pragma once
#include "core/watermark.h"
#include <QSet>
#include <QWidget>
#include <functional>
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
namespace waibusnap
{
struct WatermarkSettingsActions
{
    WatermarkPreferences preferences;
    bool historyEnabled = false;
    std::function<QString(const QString& sourcePath, QString* managedFileName, QString* error)>
        importImage;
    std::function<QImage(const QString& managedFileName, QString* error)> loadManagedImage;
    std::function<QString(const QString& managedFileName)> removeAssetImage;
    std::function<QString(QWidget*, const QString& current)> chooseImage;
    std::function<QString(const WatermarkPreferences&)> savePreferences;
};
// 设置页的水印草稿。图片导入先写受管文件；取消或未引用时由本页清理。
class WatermarkSettingsPage final : public QWidget
{
  public:
    explicit WatermarkSettingsPage(WatermarkSettingsActions actions, QWidget* parent = nullptr);
    ~WatermarkSettingsPage() override;
    WatermarkPreferences collect() const;
    QString validationError(bool historyEnabled) const;
    void setHistoryEnabled(bool enabled);
    // 保存成功后提交旧文件清理；返回清理错误。
    QString commitPendingAssets();
    void discardPendingAssets();

  private:
    void rebuildAssets(const QString& selected = {});
    void rebuildPresets(const QString& selected = {});
    void rebuild();
    QString selectedAssetId() const;
    QString selectedPresetId() const;
    WatermarkAsset* assetById(const QString& id);
    WatermarkPreset* presetById(const QString& id);
    int presetReferenceCount(const QString& assetId) const;
    void addAsset(WatermarkKind kind);
    void editAsset();
    void deleteAsset();
    void addPreset();
    void editPreset();
    void duplicatePreset();
    void deletePreset();
    void setDefaultPreset();
    void updatePreviews();
    QImage assetImage(const WatermarkAsset& asset, QString* error = nullptr) const;
    void showNotice(const QString& text);
    WatermarkSettingsActions actions_;
    WatermarkPreferences draft_;
    QSet<QString> pendingNewImages_;
    QSet<QString> pendingOldImages_;
    QListWidget* assets_ = nullptr;
    QListWidget* presets_ = nullptr;
    QLabel* assetPreview_ = nullptr;
    QLabel* presetPreview_ = nullptr;
    QLabel* notice_ = nullptr;
    QCheckBox* autoCheck_ = nullptr;
    QCheckBox* retainOriginalCheck_ = nullptr;
    QPushButton* addTextButton_ = nullptr;
    QPushButton* addImageButton_ = nullptr;
    QPushButton* editAssetButton_ = nullptr;
    QPushButton* deleteAssetButton_ = nullptr;
    QPushButton* addPresetButton_ = nullptr;
    QPushButton* editPresetButton_ = nullptr;
    QPushButton* duplicatePresetButton_ = nullptr;
    QPushButton* deletePresetButton_ = nullptr;
    QPushButton* defaultPresetButton_ = nullptr;
    bool saved_ = false;
};
}
