#pragma once
#include "core/multi_display.h"
#include "output/image_output.h"
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
namespace waibusnap
{
struct MultiDisplayStitchRequest
{
    // 与 layout.ordered 一一对应，逐屏原始采集图（DPR 已归一为 1，未加水印）。
    QVector<QImage> frames;
    MultiDisplayLayout layout;
    ImageFormat format = ImageFormat::Png;
};
// 纯合成：按布局把各屏缩放到统一倍率贴到相对位置；屏幕之间留空。
// PNG 留透明，JPEG 留白；不加水印（由调用方决定整张还是逐屏）。
// 输入与布局不匹配、画布超限或分配失败时返回空图。
QImage composeMultiDisplayStitch(const MultiDisplayStitchRequest& request);

struct MultiDisplaySaveRequest
{
    QString directory;
    ImageFormat format = ImageFormat::Png;
    // 单屏原图，按 layout.ordered 顺序，数量必须等于 layout.ordered.size()。
    QVector<QImage> frames;
    // 开启时额外保存拼接图，占据 0 号位。
    bool stitch = false;
    QImage stitchImage;
};

struct MultiDisplaySaveActions
{
    // 入参为目录、起始时间、单屏数量、是否拼接、扩展名；返回不含序号的前缀。
    std::function<QString(const QString&, const QDateTime&, int, bool, const QString&)>
        reservePrefix;
    std::function<ImageFileResult(const QImage&, const QString&)> write;
    std::function<bool(const QString&)> remove;
    std::function<QDateTime()> timestamp;
};

struct MultiDisplaySaveResult
{
    bool success = false;
    // 按序号顺序，拼接图在前。
    QStringList paths;
    QString explanation;
};
// 整组原子落盘：先预留前缀，再逐张写入；任一张失败即逆序删除本次已写文件。
MultiDisplaySaveResult saveMultiDisplayGroup(const MultiDisplaySaveRequest& request,
                                             const MultiDisplaySaveActions& actions = {});
}
