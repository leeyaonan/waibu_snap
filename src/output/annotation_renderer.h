#pragma once
#include "core/annotation.h"
#include <QImage>
#include <QPainter>
namespace waibusnap
{
struct MosaicPatch
{
    QRect pixels;
    QImage image;
};
// 按提交位置缓存补丁；基图 cacheKey 或几何变化时重算，撤销时缩减缓存。
class AnnotationRenderCache
{
    struct Entry
    {
        QRectF region;
        MosaicPatch patch;
    };
    qint64 baseKey_ = 0;
    QVector<Entry> entries_;
    friend void paintAnnotations(QPainter&, const QVector<Annotation>&, const QImage&,
                                 AnnotationRenderCache*);
};
// 面积平均缩小到块网格，再最近邻铺回；输入区域与块边长均为物理像素。
int mosaicBlockSize(QSizeF pixels);
MosaicPatch pixelateRegion(const QImage& base, QRectF region);
// 调用者设置物理像素坐标变换；base 始终为不可变基图，不采样标注层。
void paintAnnotation(QPainter& painter, const Annotation& annotation, const QImage& base);
void paintAnnotations(QPainter& painter, const QVector<Annotation>& annotations, const QImage& base,
                      AnnotationRenderCache* cache = nullptr);
// 从冻结帧合成后裁剪；只分配选区大小的输出缓冲，输出 DPR 固定为 1。
QImage renderAnnotatedSelection(const QImage& frozen, const QVector<Annotation>& annotations,
                                QRect pixels);
}
