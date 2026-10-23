#include "app/watermark_service.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <utility>
namespace waibusnap
{
namespace
{
constexpr int maximumImportedImageSide = 1024;
}
WatermarkService::WatermarkService(AppSettings settings, QString assetDirectory, QObject* parent)
    : QObject(parent), settings_(std::move(settings)), assetDirectory_(std::move(assetDirectory))
{
    if (assetDirectory_.isEmpty())
        assetDirectory_ =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("watermarks"));
}
bool WatermarkService::initialize(QString* notice)
{
    preferences_ = settings_.loadWatermarkPreferences(notice);
    if (preferences_.presets.isEmpty())
        preferences_.autoEnabled = false;
    currentPresetId_ = resolveCurrentPresetId(preferences_);
    initialized_ = true;
    return true;
}
const WatermarkAsset* WatermarkService::asset(const QString& id) const
{
    return findWatermarkAsset(preferences_, id);
}
const WatermarkPreset* WatermarkService::preset(const QString& id) const
{
    return findWatermarkPreset(preferences_, id);
}
const WatermarkPreset* WatermarkService::defaultPreset() const
{
    return defaultWatermarkPreset(preferences_);
}
const WatermarkPreset* WatermarkService::currentPreset() const { return preset(currentPresetId_); }
QString WatermarkService::assetPath(const WatermarkAsset& asset) const
{
    if (asset.imageFileName.isEmpty())
        return {};
    return QDir(assetDirectory_).filePath(asset.imageFileName);
}
QImage WatermarkService::assetImage(const QString& assetId, QString* error) const
{
    if (error)
        error->clear();
    const WatermarkAsset* asset = findWatermarkAsset(preferences_, assetId);
    if (!asset || asset->kind != WatermarkKind::Image)
    {
        if (error)
            *error = QStringLiteral("水印图片资产不存在。");
        return {};
    }
    const auto cached = imageCache_.constFind(assetId);
    if (cached != imageCache_.cend())
        return *cached;
    QImageReader reader(assetPath(*asset));
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
    {
        if (error)
            *error = QStringLiteral("水印图片无法读取：%1").arg(reader.errorString());
        return {};
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1);
    imageCache_.insert(assetId, image);
    return image;
}
QString WatermarkService::importImage(const QString& sourcePath, QString* managedFileName,
                                      QString* error) const
{
    if (managedFileName)
        managedFileName->clear();
    if (error)
        error->clear();
    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);
    const QImage source = reader.read();
    if (source.isNull())
    {
        const QString message = QStringLiteral("水印图片无法读取：%1").arg(reader.errorString());
        if (error)
            *error = message;
        return message;
    }
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1);
    if (image.width() > maximumImportedImageSide || image.height() > maximumImportedImageSide)
        image = image.scaled(maximumImportedImageSide, maximumImportedImageSide,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!QDir().mkpath(assetDirectory_))
    {
        const QString message = QStringLiteral("无法创建水印资产目录：%1").arg(assetDirectory_);
        if (error)
            *error = message;
        return message;
    }
    const QString fileName =
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".png");
    const QString target = QDir(assetDirectory_).filePath(fileName);
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
    {
        const QString message = QStringLiteral("无法写入水印图片：%1").arg(target);
        if (error)
            *error = message;
        return message;
    }
    if (managedFileName)
        *managedFileName = fileName;
    return {};
}
QString WatermarkService::removeAssetImage(const QString& managedFileName) const
{
    if (managedFileName.isEmpty() || managedFileName.contains(QLatin1Char('/')) ||
        managedFileName.contains(QLatin1Char('\\')))
        return QStringLiteral("水印图片文件名无效。");
    const QString path = QDir(assetDirectory_).filePath(managedFileName);
    if (QFileInfo::exists(path) && !QFile::remove(path))
        return QStringLiteral("无法删除水印图片：%1").arg(path);
    imageCache_.clear();
    return {};
}
QString WatermarkService::savePreferences(const WatermarkPreferences& preferences)
{
    WatermarkPreferences next = preferences;
    if (next.presets.isEmpty())
        next.autoEnabled = false;
    const QString validation = watermarkPreferencesError(next);
    if (!validation.isEmpty())
        return validation;
    for (const auto& asset : next.assets)
        if (asset.kind == WatermarkKind::Image && !QFileInfo::exists(assetPath(asset)))
            return QStringLiteral("水印图片资产不存在：%1").arg(asset.name);
    const bool oldAuto = preferences_.autoEnabled;
    const QString oldCurrent = currentPresetId_;
    const QString error = settings_.saveWatermarkPreferences(next);
    if (!error.isEmpty())
        return error;
    preferences_ = std::move(next);
    imageCache_.clear();
    currentPresetId_ = resolveCurrentPresetId(preferences_, oldCurrent);
    emit preferencesChanged();
    if (currentPresetId_ != oldCurrent)
        emit currentPresetChanged();
    if (preferences_.autoEnabled != oldAuto)
        emit autoChanged(preferences_.autoEnabled);
    return {};
}
QString WatermarkService::setCurrentPreset(const QString& presetId)
{
    if (!findWatermarkPreset(preferences_, presetId))
        return QStringLiteral("水印预设不存在。");
    if (currentPresetId_ == presetId)
        return {};
    currentPresetId_ = presetId;
    emit currentPresetChanged();
    return {};
}
QString WatermarkService::setAutoEnabled(bool enabled)
{
    if (enabled && !currentPreset())
        return QStringLiteral("没有可用的水印预设，无法开启常驻水印。");
    if (preferences_.autoEnabled == enabled)
        return {};
    WatermarkPreferences next = preferences_;
    next.autoEnabled = enabled;
    return savePreferences(next);
}
QString WatermarkService::resolveCurrentPresetId(const WatermarkPreferences& preferences,
                                                 const QString& preferred) const
{
    if (!preferred.isEmpty() && findWatermarkPreset(preferences, preferred))
        return preferred;
    if (findWatermarkPreset(preferences, preferences.defaultPresetId))
        return preferences.defaultPresetId;
    if (!preferences.presets.isEmpty())
        return preferences.presets.first().id;
    return {};
}
}
