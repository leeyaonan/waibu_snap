#include "output/multi_display_output.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <algorithm>
namespace waibusnap
{
namespace
{
QString extensionOf(ImageFormat format)
{
    return format == ImageFormat::Jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
}
QStringList targetNames(const QString& base, int displayCount, bool stitch, ImageFormat format)
{
    return multiDisplayFileNames(base, displayCount, stitch, extensionOf(format));
}
}
QImage composeMultiDisplayStitch(const MultiDisplayStitchRequest& request)
{
    const auto& layout = request.layout;
    if (!layout.isValid() || layout.stitchExceedsLimit ||
        layout.placements.size() != layout.ordered.size() ||
        request.frames.size() != layout.ordered.size() || layout.canvasPixels.isEmpty())
        return {};
    const bool jpeg = request.format == ImageFormat::Jpeg;
    QImage canvas(layout.canvasPixels,
                  jpeg ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull())
        return {};
    // 未覆盖区域：PNG 保持透明，JPEG 无 alpha 通道必须先刷白。
    canvas.fill(jpeg ? QColor(Qt::white) : QColor(Qt::transparent));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int index = 0; index < layout.ordered.size(); ++index)
    {
        QImage frame = request.frames[index];
        if (frame.isNull())
            return {};
        frame.setDevicePixelRatio(1);
        const QRect target = layout.placements[index];
        if (target.isEmpty())
            return {};
        if (frame.size() == target.size())
        {
            painter.drawImage(target.topLeft(), frame);
            continue;
        }
        const QImage scaled =
            frame.scaled(target.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (scaled.isNull())
            return {};
        painter.drawImage(target.topLeft(), scaled);
    }
    painter.end();
    canvas.setDevicePixelRatio(1);
    return canvas;
}
MultiDisplaySaveResult saveMultiDisplayGroup(const MultiDisplaySaveRequest& request,
                                             const MultiDisplaySaveActions& actionsInput)
{
    MultiDisplaySaveActions actions = actionsInput;
    if (!actions.reservePrefix)
        actions.reservePrefix = [](const QString& directory, const QDateTime& timestamp, int count,
                                   bool stitch, const QString& extension)
        { return multiDisplayPrefix(directory, timestamp, count, stitch, extension); };
    if (!actions.timestamp)
        actions.timestamp = [] { return QDateTime::currentDateTime(); };
    if (!actions.remove)
        actions.remove = [](const QString& path) { return QFile::remove(path); };
    const int count = int(request.frames.size());
    if (request.directory.isEmpty() || count <= 0)
        return {false, {}, QStringLiteral("没有可保存的屏幕截图或目标目录无效。")};
    if (request.stitch && request.stitchImage.isNull())
        return {false, {}, QStringLiteral("拼接图为空，已取消本次保存。")};
    const QString extension = extensionOf(request.format);
    if (!actions.write)
    {
        actions.write = [format = request.format](const QImage& image, const QString& path)
        {
            return format == ImageFormat::Jpeg
                       ? ImageFileResult{exportJpegToPath(image, path), path}
                       : ImageFileResult{exportPngToPath(image, path), path};
        };
    }
    const QDateTime timestamp = actions.timestamp();
    const QString base =
        actions.reservePrefix(request.directory, timestamp, count, request.stitch, extension);
    if (base.isEmpty())
        return {false, {}, QStringLiteral("无法生成不冲突的文件名，请检查目录后重试。")};
    const auto names = targetNames(base, count, request.stitch, request.format);
    if (names.isEmpty())
        return {false, {}, QStringLiteral("无法生成整组文件名，已取消本次保存。")};
    const QDir folder(request.directory);
    MultiDisplaySaveResult result;
    result.paths.reserve(names.size());
    const auto rollback = [&actions, &result]()
    {
        // 逆序删除本次已写文件，尽力回收；单个失败不掩盖首个错误。
        for (auto it = result.paths.rbegin(); it != result.paths.rend(); ++it)
            actions.remove(*it);
        result.paths.clear();
    };
    int nameIndex = 0;
    if (request.stitch)
    {
        const QString path = folder.filePath(names.at(nameIndex++));
        const auto written = actions.write(request.stitchImage, path);
        if (!written.result.success)
        {
            rollback();
            result.explanation =
                QStringLiteral("拼接图保存失败：%1").arg(written.result.explanation);
            return result;
        }
        result.paths.append(written.path.isEmpty() ? path : written.path);
    }
    for (int index = 0; index < count; ++index)
    {
        const QString path = folder.filePath(names.at(nameIndex++));
        const auto written = actions.write(request.frames[index], path);
        if (!written.result.success)
        {
            rollback();
            result.explanation = QStringLiteral("第 %1 屏保存失败：%2")
                                     .arg(index + 1)
                                     .arg(written.result.explanation);
            return result;
        }
        result.paths.append(written.path.isEmpty() ? path : written.path);
    }
    result.success = true;
    return result;
}
}
