#include "core/recording.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <limits>
namespace waibusnap
{
namespace
{
constexpr qint64 mebibyte = 1024LL * 1024LL;
QString defaultDirectory(QStandardPaths::StandardLocation location, const QString& suffix)
{
    QString base = QStandardPaths::writableLocation(location);
    if (base.isEmpty())
        base = QDir::homePath();
    QDir directory(base);
    const auto parts = suffix.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const auto& part : parts)
        directory = QDir(directory.filePath(part));
    return directory.absolutePath();
}
QSize fitWithin(QSize source, QSize limit)
{
    if (source.isEmpty() || limit.isEmpty())
        return source;
    if (source.width() <= limit.width() && source.height() <= limit.height())
        return source;
    const qint64 sourceWidth = source.width();
    const qint64 sourceHeight = source.height();
    const qint64 limitWidth = limit.width();
    const qint64 limitHeight = limit.height();
    // 交叉相乘避免浮点误差；相等时按宽度受限处理。
    if (sourceWidth * limitHeight >= sourceHeight * limitWidth)
    {
        const auto height = qRound64(double(sourceHeight) * double(limitWidth) / sourceWidth);
        return {limit.width(), int(qBound<qint64>(1, height, limitHeight))};
    }
    const auto width = qRound64(double(sourceWidth) * double(limitHeight) / sourceHeight);
    return {int(qBound<qint64>(1, width, limitWidth)), limit.height()};
}
int evenFloor(int value) { return value <= 2 ? 2 : value - value % 2; }
}
QString defaultRecordingVideoDirectory()
{
    return defaultDirectory(QStandardPaths::MoviesLocation, QStringLiteral("WaibuSnap"));
}
QString defaultRecordingGifDirectory()
{
    return defaultDirectory(QStandardPaths::PicturesLocation, QStringLiteral("WaibuSnap/GIF"));
}
QString suggestedRecordingPath(const QString& directory, RecordingOutput output,
                               QDateTime timestamp)
{
    QString folder = directory;
    if (folder.isEmpty() || !QDir::isAbsolutePath(folder))
    {
        folder = output == RecordingOutput::Gif ? defaultRecordingGifDirectory()
                                                : defaultRecordingVideoDirectory();
    }
    const QString base = QStringLiteral("WaibuSnap_%1")
                             .arg(timestamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")));
    const QString extension =
        output == RecordingOutput::Gif ? QStringLiteral(".gif") : QStringLiteral(".mp4");
    const QDir target(folder);
    QString path = target.filePath(base + extension);
    int suffix = 2;
    while (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
        path = target.filePath(QStringLiteral("%1_%2%3").arg(base).arg(suffix++).arg(extension));
    return path;
}
QSize recordingOutputSize(QSize sourcePixels, RecordingOutput output,
                          RecordingResolution resolution, int gifMaxEdge)
{
    if (sourcePixels.isEmpty())
        return {};
    QSize result = sourcePixels;
    if (output == RecordingOutput::Gif)
    {
        if (gifMaxEdge > 0)
            result = fitWithin(sourcePixels, {gifMaxEdge, gifMaxEdge});
    }
    else
    {
        QSize limit;
        switch (resolution)
        {
        case RecordingResolution::Native:
            break;
        case RecordingResolution::P1080:
            limit = {1920, 1080};
            break;
        case RecordingResolution::P720:
            limit = {1280, 720};
            break;
        }
        if (!limit.isEmpty())
            result = fitWithin(sourcePixels, limit);
        result = {evenFloor(result.width()), evenFloor(result.height())};
    }
    result.setWidth(std::max(2, result.width()));
    result.setHeight(std::max(2, result.height()));
    return result;
}
qint64 estimatedVideoBitrate(QSize pixels, int fps, RecordingQuality quality)
{
    double bitsPerPixel = 0.18;
    qint64 minimum = 6000000;
    qint64 maximum = 40000000;
    switch (quality)
    {
    case RecordingQuality::High:
        break;
    case RecordingQuality::Medium:
        bitsPerPixel = 0.12;
        minimum = 4000000;
        maximum = 24000000;
        break;
    case RecordingQuality::Low:
        bitsPerPixel = 0.07;
        minimum = 2500000;
        maximum = 12000000;
        break;
    }
    if (pixels.width() <= 0 || pixels.height() <= 0 || fps <= 0)
        return minimum;
    const double estimate =
        double(pixels.width()) * double(pixels.height()) * double(fps) * bitsPerPixel;
    if (estimate <= double(minimum))
        return minimum;
    if (estimate >= double(maximum))
        return maximum;
    return qint64(std::llround(estimate));
}
qint64 estimatedGifBytes(QSize pixels, int fps, int seconds)
{
    constexpr qint64 headerBytes = 1024;
    if (pixels.width() <= 0 || pixels.height() <= 0 || fps <= 0 || seconds <= 0)
        return headerBytes;
    const double estimate =
        double(pixels.width()) * double(pixels.height()) * double(fps) * double(seconds) * 0.12 +
        double(headerBytes);
    if (estimate >= double(std::numeric_limits<qint64>::max()))
        return std::numeric_limits<qint64>::max();
    return qint64(std::llround(estimate));
}
RecordingDiskState recordingDiskState(qint64 availableBytes)
{
    if (availableBytes < 512 * mebibyte)
        return RecordingDiskState::Critical;
    if (availableBytes < 2048 * mebibyte)
        return RecordingDiskState::Low;
    return RecordingDiskState::Normal;
}
}
