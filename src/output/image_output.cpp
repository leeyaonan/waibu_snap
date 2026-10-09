#include "output/image_output.h"
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageWriter>
#include <QMimeData>
#include <QSaveFile>
#include <QTemporaryFile>
namespace waibusnap
{
QImage cropFrozenSelection(const QImage& frozen, QRect pixels)
{
    if (frozen.isNull() || pixels.isEmpty() || !frozen.rect().contains(pixels))
        return {};
    QImage image = frozen.copy(pixels);
    image.setDevicePixelRatio(1);
    return image;
}
ImageOutputResult copyImageToClipboard(const QImage& image)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard || image.isNull())
        return {false, QStringLiteral("剪贴板不可用，截图已保留，请重试复制。")};
    QImage output = image;
    output.setDevicePixelRatio(1);
    clipboard->setImage(output, QClipboard::Clipboard);
    // setImage 无返回值，检查 Qt 是否接受了图片；外部应用实际粘贴仍需实机验证。
    const QMimeData* data = clipboard->mimeData(QClipboard::Clipboard);
    if (!data || !data->hasImage() ||
        clipboard->image(QClipboard::Clipboard).size() != output.size())
        return {false, QStringLiteral("复制未完成，截图已保留，请重试复制。")};
    return {true, {}};
}
QString imageFilePath(const QString& path, ImageFormat format)
{
    if (path.isEmpty())
        return path;
    if (format == ImageFormat::Jpeg)
    {
        if (path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive) ||
            path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive))
            return path;
        return path + QStringLiteral(".jpg");
    }
    if (path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        return path;
    return path + QStringLiteral(".png");
}
ImageFormat imageFormatForPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg") ? ImageFormat::Jpeg
                                                                               : ImageFormat::Png;
}
QString suggestedImagePath(const QString& directory, ImageFormat format, QDateTime timestamp)
{
    const QString base = QStringLiteral("WaibuSnap_%1")
                             .arg(timestamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")));
    const QDir folder(directory);
    const QString extension =
        format == ImageFormat::Jpeg ? QStringLiteral(".jpg") : QStringLiteral(".png");
    QString path = folder.filePath(base + extension);
    int suffix = 2;
    while (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
        path = folder.filePath(QStringLiteral("%1_%2%3").arg(base).arg(suffix++).arg(extension));
    return path;
}
// 保留 PNG 单格式契约，既有逐像素与命名用例无需改写。
QString pngFilePath(const QString& path) { return imageFilePath(path, ImageFormat::Png); }
QString suggestedPngPath(const QString& directory, QDateTime timestamp)
{
    return suggestedImagePath(directory, ImageFormat::Png, timestamp);
}
ImageOutputResult exportPngToPath(const QImage& image, const QString& path)
{
    if (image.isNull() || path.isEmpty())
        return {false, QStringLiteral("没有可保存的选区或目标路径无效。")};
    QSaveFile file(imageFilePath(path, ImageFormat::Png));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return {false, QStringLiteral("无法写入目标目录：%1").arg(file.errorString())};
    QImage output = image;
    output.setDevicePixelRatio(1);
    QImageWriter writer(&file, "png");
    if (!writer.write(output))
    {
        file.cancelWriting();
        return {false, QStringLiteral("PNG 编码或写入失败：%1").arg(writer.errorString())};
    }
    if (!file.commit())
        return {false, QStringLiteral("文件提交失败：%1").arg(file.errorString())};
    return {true, {}};
}
ImageOutputResult exportJpegToPath(const QImage& image, const QString& path)
{
    if (image.isNull() || path.isEmpty())
        return {false, QStringLiteral("没有可保存的选区或目标路径无效。")};
    QSaveFile file(imageFilePath(path, ImageFormat::Jpeg));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return {false, QStringLiteral("无法写入目标目录：%1").arg(file.errorString())};
    QImage output = image;
    output.setDevicePixelRatio(1);
    QImageWriter writer(&file, "jpeg");
    writer.setQuality(90);
    if (!writer.write(output))
    {
        file.cancelWriting();
        return {false, QStringLiteral("JPEG 编码或写入失败：%1").arg(writer.errorString())};
    }
    if (!file.commit())
        return {false, QStringLiteral("文件提交失败：%1").arg(file.errorString())};
    return {true, {}};
}
ImageFileResult exportImageToPath(const QImage& image, const QString& path)
{
    const auto format = imageFormatForPath(path);
    const QString finalPath = imageFilePath(path, format);
    const auto result = format == ImageFormat::Jpeg ? exportJpegToPath(image, finalPath)
                                                    : exportPngToPath(image, finalPath);
    return {result, result.success ? QFileInfo(finalPath).absoluteFilePath() : QString()};
}
ImageFileResult exportImageToNewPath(const QImage& image, const QString& directory,
                                     ImageFormat format, QDateTime timestamp)
{
    if (image.isNull() || directory.isEmpty())
        return {{false, QStringLiteral("没有可保存的选区或目标路径无效。")}, {}};
    QTemporaryFile file(QDir(directory).filePath(QStringLiteral(".WaibuSnap_XXXXXX")));
    if (!file.open())
        return {{false, QStringLiteral("无法写入目标目录：%1").arg(file.errorString())}, {}};
    QImage output = image;
    output.setDevicePixelRatio(1);
    QImageWriter writer(&file, format == ImageFormat::Jpeg ? "jpeg" : "png");
    if (format == ImageFormat::Jpeg)
        writer.setQuality(90);
    if (!writer.write(output))
        return {{false, QStringLiteral("%1 编码或写入失败：%2")
                            .arg(format == ImageFormat::Jpeg ? QStringLiteral("JPEG")
                                                             : QStringLiteral("PNG"),
                                 writer.errorString())},
                {}};
    if (!file.flush())
        return {{false, QStringLiteral("文件提交失败：%1").arg(file.errorString())}, {}};
    file.close();
    while (true)
    {
        const QString path = suggestedImagePath(directory, format, timestamp);
        if (file.rename(path))
        {
            file.setAutoRemove(false);
            return {{true, {}}, QFileInfo(path).absoluteFilePath()};
        }
        if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
            return {{false, QStringLiteral("文件提交失败：%1").arg(file.errorString())}, {}};
    }
}

}
