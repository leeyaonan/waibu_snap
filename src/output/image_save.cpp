#include "output/image_save.h"
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
namespace waibusnap
{
namespace
{
const QString pngFilter = QStringLiteral("PNG 图片 (*.png)");
const QString jpegFilter = QStringLiteral("JPEG 图片（有损）(*.jpg *.jpeg)");
QString chooseNativePath(QWidget* parent, QString suggestion,
                         const std::function<bool()>& canContinue,
                         const std::function<void(const QString&)>& showNotice)
{
    QString selected = imageFormatForPath(suggestion) == ImageFormat::Jpeg ? jpegFilter : pngFilter;
    while (true)
    {
        const QString chosen = QFileDialog::getSaveFileName(
            parent, QStringLiteral("保存图片"), suggestion, imageSaveFilters(), &selected);
        if (!canContinue() || chosen.isEmpty())
            return {};
        const QString path = savePanelFilePath(chosen, selected == jpegFilter ? ImageFormat::Jpeg
                                                                              : ImageFormat::Png);
        if (path == chosen || (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()))
            return path;
        // 规范化后的另一个已有目标尚未获覆盖确认，重新交给原生面板。
        suggestion = path;
        showNotice(QStringLiteral("补全后缀后文件已存在，请在保存面板确认覆盖。"));
    }
}
}
QString imageSaveFilters() { return pngFilter + QStringLiteral(";;") + jpegFilter; }
QString savePanelFilePath(const QString& path, ImageFormat selectedFormat)
{
    if (path.isEmpty())
        return {};
    const QString suffix = QFileInfo(path).suffix();
    if ((selectedFormat == ImageFormat::Png &&
         suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0) ||
        (selectedFormat == ImageFormat::Jpeg &&
         (suffix.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0 ||
          suffix.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0)))
        return path;
    const QString base = suffix.isEmpty() ? path : path.left(path.size() - suffix.size() - 1);
    return imageFilePath(base, selectedFormat);
}
ImageSaveTarget chooseImageSaveTarget(QWidget* parent, const ImageSaveActions& actions,
                                      const std::function<bool()>& canContinue,
                                      const std::function<void(const QString&)>& showNotice)
{
    const auto preferences =
        actions.loadSavePreferences ? actions.loadSavePreferences() : SavePreferences{};
    const auto timestamp =
        actions.saveTimestamp ? actions.saveTimestamp() : QDateTime::currentDateTime();
    const QFileInfo quick(preferences.quickDirectory);
    if (!preferences.quickDirectory.isEmpty() && quick.isAbsolute() && quick.exists() &&
        quick.isDir() && quick.isWritable())
        return {suggestedImagePath(preferences.quickDirectory, preferences.format, timestamp),
                preferences.quickDirectory, preferences.format, timestamp};
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString suggestion = suggestedImagePath(directory, preferences.format, timestamp);
    while (canContinue())
    {
        const QString chosen = actions.chooseSavePath
                                   ? actions.chooseSavePath(parent, suggestion)
                                   : chooseNativePath(parent, suggestion, canContinue, showNotice);
        if (!canContinue() || chosen.isEmpty())
            return {};
        const auto format = imageFormatForPath(chosen);
        const QString path = imageFilePath(chosen, format);
        if (path == chosen || (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()))
            return {path, {}, format, timestamp};
        // 注入缝仍接收原签名，补后缀产生的冲突也必须确认最终路径。
        suggestion = path;
        showNotice(QStringLiteral("补全后缀后文件已存在，请在保存面板确认覆盖。"));
    }
    return {};
}
ImageFileResult saveImageToTarget(const QImage& image, const ImageSaveTarget& target)
{
    return target.quickDirectory.isEmpty() ? exportImageToPath(image, target.path)
                                           : exportImageToNewPath(image, target.quickDirectory,
                                                                  target.format, target.timestamp);
}
}
