#pragma once
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
}
