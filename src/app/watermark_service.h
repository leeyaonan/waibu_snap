#pragma once
#include "app/app_settings.h"
#include "core/watermark.h"
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
namespace waibusnap
{
// 水印设置、资产文件和本次运行的当前预设的唯一状态源。
class WatermarkService final : public QObject
{
    Q_OBJECT
  public:
    explicit WatermarkService(AppSettings settings, QString assetDirectory = {},
                              QObject* parent = nullptr);
    bool initialize(QString* notice = nullptr);
    const WatermarkPreferences& preferences() const { return preferences_; }
    const WatermarkAsset* asset(const QString& id) const;
    const WatermarkPreset* preset(const QString& id) const;
    const WatermarkPreset* defaultPreset() const;
    const WatermarkPreset* currentPreset() const;
    QString currentPresetId() const { return currentPresetId_; }
    bool autoEnabled() const { return preferences_.autoEnabled; }
    bool retainOriginal() const { return preferences_.retainOriginal; }
    QString assetDirectory() const { return assetDirectory_; }
    QString assetPath(const WatermarkAsset& asset) const;
    // 失败返回空 QImage，并在 error 中给出可展示原因。
    QImage assetImage(const QString& assetId, QString* error = nullptr) const;
    // 导入并规范化图片资产；成功时 managedFileName 为受管相对文件名。
    QString importImage(const QString& sourcePath, QString* managedFileName = nullptr,
                        QString* error = nullptr) const;
    QString removeAssetImage(const QString& managedFileName) const;
    QString savePreferences(const WatermarkPreferences& preferences);
    QString setCurrentPreset(const QString& presetId);
    QString setAutoEnabled(bool enabled);
  signals:
    void preferencesChanged();
    void currentPresetChanged();
    void autoChanged(bool enabled);

  private:
    QString resolveCurrentPresetId(const WatermarkPreferences& preferences,
                                   const QString& preferred = {}) const;
    AppSettings settings_;
    QString assetDirectory_;
    WatermarkPreferences preferences_;
    QString currentPresetId_;
    mutable QHash<QString, QImage> imageCache_;
    bool initialized_ = false;
};
}
