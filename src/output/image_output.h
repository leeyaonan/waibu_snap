#pragma once
#include <QDateTime>
#include <QImage>
#include <QString>
namespace waibusnap
{
enum class ImageFormat
{
    Png,
    Jpeg
};
struct SavePreferences
{
    QString quickDirectory;
    ImageFormat format = ImageFormat::Png;
};
struct ImageOutputResult
{
    bool success = false;
    QString explanation;
};
struct ImageFileResult
{
    ImageOutputResult result;
    QString path;
};
QString imageFilePath(const QString& path, ImageFormat format = ImageFormat::Png);
ImageFormat imageFormatForPath(const QString& path);
QString suggestedImagePath(const QString& directory, ImageFormat format = ImageFormat::Png,
                           QDateTime timestamp = QDateTime::currentDateTime());
ImageOutputResult exportJpegToPath(const QImage& image, const QString& path);
ImageFileResult exportImageToPath(const QImage& image, const QString& path);
// 同目录临时编码后原子重命名，绝不覆盖；竞争冲突重新生成名字。
ImageFileResult exportImageToNewPath(const QImage& image, const QString& directory,
                                     ImageFormat format,
                                     QDateTime timestamp = QDateTime::currentDateTime());
// 只裁剪冻结帧，不读取屏幕显示层；输出副本的 DPR 固定为 1。
QImage cropFrozenSelection(const QImage& frozen, QRect pixels);
ImageOutputResult copyImageToClipboard(const QImage& image);
QString pngFilePath(const QString& path);
QString suggestedPngPath(const QString& directory,
                         QDateTime timestamp = QDateTime::currentDateTime());
// 路径须已由调用者确认；原子提交失败时不破坏原文件，不启用直接覆盖回退。
ImageOutputResult exportPngToPath(const QImage& image, const QString& path);
}
