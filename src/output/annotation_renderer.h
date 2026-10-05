#pragma once
#include "core/annotation.h"
#include <QImage>
#include <QPainter>
namespace waibusnap
{
// 调用者设置物理像素坐标变换；函数不读取任何 QWidget 或 UI 状态。
void paintAnnotation(QPainter& painter, const Annotation& annotation);
void paintAnnotations(QPainter& painter, const QVector<Annotation>& annotations);
// 从冻结帧合成后裁剪；只分配选区大小的输出缓冲，输出 DPR 固定为 1。
QImage renderAnnotatedSelection(const QImage& frozen, const QVector<Annotation>& annotations,
                                QRect pixels);
}
