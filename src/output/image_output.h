#pragma once
#include <QDateTime>
#include <QImage>
#include <QString>
namespace waibusnap
{
struct ImageOutputResult
{
    bool success = false;
    QString explanation;
};
// 只裁剪冻结帧，不读取屏幕显示层；输出副本的 DPR 固定为 1。
QImage cropFrozenSelection(const QImage& frozen, QRect pixels);
ImageOutputResult copyImageToClipboard(const QImage& image);
QString pngFilePath(const QString& path);
QString suggestedPngPath(const QString& directory,
                         QDateTime timestamp = QDateTime::currentDateTime());
// 路径须已由调用者确认；原子提交失败时不破坏原文件，不启用直接覆盖回退。
ImageOutputResult exportPngToPath(const QImage& image, const QString& path);
}
