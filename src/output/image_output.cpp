#include "output/image_output.h"
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageWriter>
#include <QMimeData>
#include <QSaveFile>
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
QString pngFilePath(const QString& path)
{
    if (path.isEmpty() || path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        return path;
    return path + QStringLiteral(".png");
}
QString suggestedPngPath(const QString& directory, QDateTime timestamp)
{
    const QString base = QStringLiteral("WaibuSnap_%1")
                             .arg(timestamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")));
    const QDir folder(directory);
    QString path = folder.filePath(base + QStringLiteral(".png"));
    int suffix = 2;
    while (QFileInfo::exists(path))
        path = folder.filePath(QStringLiteral("%1_%2.png").arg(base).arg(suffix++));
    return path;
}
ImageOutputResult exportPngToPath(const QImage& image, const QString& path)
{
    if (image.isNull() || path.isEmpty())
        return {false, QStringLiteral("没有可保存的选区或目标路径无效。")};
    QSaveFile file(pngFilePath(path));
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
}
