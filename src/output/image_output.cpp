#include "output/image_output.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
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
}
