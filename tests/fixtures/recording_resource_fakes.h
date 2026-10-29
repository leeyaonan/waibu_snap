#pragma once
#include "core/gif_encoder.h"
#include "ui/recording_resource_source.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
namespace waibusnap::test
{
inline QString writeMedia(const QString& directory, const QString& name, QByteArray data = "media")
{
    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        qFatal("临时媒体文件写入失败。");
    return QFileInfo(path).canonicalFilePath();
}
inline QString writeGif(const QString& directory, const QString& name = QStringLiteral("动画.gif"))
{
    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(name);
    GifEncoder encoder;
    QString error;
    if (!encoder.start(path, {48, 32}, 10, &error))
        qFatal("临时 GIF 初始化失败。");
    for (const auto color : {Qt::red, Qt::blue, Qt::green})
    {
        QImage image(48, 32, QImage::Format_RGB32);
        image.fill(color);
        if (!encoder.addFrame(image, &error))
            qFatal("临时 GIF 写帧失败。");
    }
    if (!encoder.finish(&error))
        qFatal("临时 GIF 收尾失败。");
    return QFileInfo(path).canonicalFilePath();
}
class FakeMediaProbe final : public RecordingMediaProbe
{
  public:
    mutable std::atomic<int> probes{0}, thumbnails{0}, running{0}, peak{0};
    int delayMs = 0;
    bool fail = false;
    RecordingMediaInfo probe(const QString& path) const override
    {
        ++probes;
        const int count = ++running;
        int previous = peak.load();
        while (previous < count && !peak.compare_exchange_weak(previous, count))
        {
        }
        if (delayMs)
            QThread::msleep(static_cast<unsigned long>(delayMs));
        --running;
        if (fail)
            return {false, 0, {}, QStringLiteral("损坏媒体替身")};
        if (QFileInfo(path).suffix() == QStringLiteral("gif"))
            return probeRecordingGif(path);
        return {true, 12500, {640, 360}, {}};
    }
    QImage thumbnail(const QString&, QSize bounds) const override
    {
        ++thumbnails;
        QImage image(bounds.isValid() ? QSize(640, 360).scaled(bounds, Qt::KeepAspectRatio)
                                      : QSize(640, 360),
                     QImage::Format_RGB32);
        image.fill(Qt::cyan);
        return image;
    }
};
}
