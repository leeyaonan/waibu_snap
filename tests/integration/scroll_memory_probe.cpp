#include "core/scroll_stitcher.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <algorithm>
using namespace waibusnap;
namespace
{
// 直接生成无限页的当前视口，不额外持有一张巨大源文档；每帧有独立像素存储。
QImage frame(int width, int height, int offset, bool dynamic, quint32 salt = 0)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int row = 0; row < height; ++row)
    {
        auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(row));
        for (int column = 0; column < width; ++column)
        {
            quint32 value =
                quint32(row + offset) * 0x9e3779b9u ^ quint32(column) * 0x85ebca6bu ^ salt;
            value = (value ^ (value >> 16)) * 0x7feb352du;
            value = (value ^ (value >> 15)) * 0x846ca68bu;
            value ^= value >> 16;
            pixels[column] = qRgb(value >> 24, (value >> 16) & 255, (value >> 8) & 255);
        }
    }
    if (dynamic)
    {
        QPainter painter(&image);
        painter.fillRect(width / 4, height / 3, width / 2, height / 3,
                         QColor::fromHsv(offset % 360, 220, 200));
    }
    return image;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4)
    {
        qInfo("用法：scroll_memory_probe baseline|cache|normal|dynamic|unreliable 宽 "
              "高；物理尺寸，DPR2。");
        return 2;
    }
    bool widthOk = false, heightOk = false;
    const int width = args[2].toInt(&widthOk), height = args[3].toInt(&heightOk);
    const QString mode = args[1];
    if (!widthOk || !heightOk || width < 64 || height < 64 || width > 8000 || height > 8000 ||
        !QStringList{"baseline", "cache", "normal", "dynamic", "unreliable"}.contains(mode))
        return 2;
    if (mode == QStringLiteral("baseline"))
    {
        qInfo("空载进程：未分配源图或画布。");
        return 0;
    }
    auto limits = ScrollStitchLimits::forDisplay(2);
    if (mode == QStringLiteral("cache"))
        limits.maxAreaPx = qint64(width) * (height + 1);
    ScrollStitcher stitcher(limits);
    const bool dynamic = mode == QStringLiteral("dynamic");
    stitcher.addFrame(frame(width, height, 0, dynamic));
    const qint64 limit = stitcher.lengthLimit();
    const int step = mode == QStringLiteral("cache") ? 1 : std::max(1, height / 8);
    for (int position = 0; !stitcher.stats().limitReached;)
    {
        position = int(std::min<qint64>(position + step, limit - height));
        // 接近最终容量时强制一次全幅失配，覆盖局部验证临时图及恢复路径。
        if (mode == QStringLiteral("unreliable") && position + height == limit)
            stitcher.addFrame(frame(width, height, position, false, 0x12345678u));
        stitcher.addFrame(frame(width, height, position, dynamic));
        if (position + height >= limit)
            break;
    }
    const QImage result = stitcher.takeResult();
    const auto& s = stitcher.stats();
    const QJsonObject report{{"模式", mode},
                             {"源宽", width},
                             {"源高", height},
                             {"源像素", qint64(width) * height},
                             {"帧", s.frames},
                             {"接受", s.accepted},
                             {"跳过", s.skipped},
                             {"低置信", s.lowConfidence},
                             {"警告", s.warnings},
                             {"内存收敛", s.memoryLimited},
                             {"物理上限", limit},
                             {"结果宽", result.width()},
                             {"结果高", result.height()},
                             {"画布字节", qint64(result.width()) * result.height() * 4}};
    qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
    return result.height() == limit && !s.memoryLimited && !s.warnings ? 0 : 1;
}
