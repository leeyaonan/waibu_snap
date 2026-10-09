#pragma once
#include "output/image_output.h"
#include <functional>
class QWidget;
namespace waibusnap
{
struct ImageSaveActions
{
    std::function<QString(QWidget*, const QString&)> chooseSavePath;
    std::function<SavePreferences()> loadSavePreferences;
    // 默认取当前时刻；固定时刻便于稳定验证连续保存与命名冲突。
    std::function<QDateTime()> saveTimestamp;
};
struct ImageSaveTarget
{
    QString path;
    QString quickDirectory;
    ImageFormat format = ImageFormat::Png;
    QDateTime timestamp;
};
QString savePanelFilePath(const QString& path, ImageFormat selectedFormat);
QString imageSaveFilters();
ImageSaveTarget chooseImageSaveTarget(QWidget* parent, const ImageSaveActions& actions,
                                      const std::function<bool()>& canContinue,
                                      const std::function<void(const QString&)>& showNotice);
ImageFileResult saveImageToTarget(const QImage& image, const ImageSaveTarget& target);
}
