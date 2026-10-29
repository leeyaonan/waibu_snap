#include "core/gif_encoder.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMovie>
#include <QStandardPaths>
#include <QThread>
#include <cstdio>
#include <ctime>
#include <mach/mach.h>
#include <sys/resource.h>
// 独立进程测量，不注册CTest；--paced按真实30秒节奏输入，--static重复画面。
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const bool stationary = app.arguments().contains(QStringLiteral("--static"));
    const bool paced = app.arguments().contains(QStringLiteral("--paced"));
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("waibusnap-r5-stream-%1.gif")
                                           .arg(QDateTime::currentMSecsSinceEpoch()));
    mach_task_basic_info_data_t mem{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&mem, &n);
    const auto base = mem.resident_size;
    waibusnap::GifEncoder encoder;
    QString error;
    if (!encoder.start(path, QSize(1280, 720), 12, &error))
        return 1;
    QElapsedTimer timer;
    timer.start();
    const auto cpu = std::clock();
    qint64 encodeNs = 0, maxNs = 0;
    for (int i = 0; i < 360; ++i)
    {
        QImage frame(1280, 720, QImage::Format_RGB32);
        const int content = stationary ? 0 : i;
        for (int y = 0; y < 720; ++y)
        {
            auto* row = (QRgb*)frame.scanLine(y);
            for (int x = 0; x < 1280; ++x)
                row[x] = qRgb((x + content * 7) % 256, y % 256, (x / 8 + y / 8 + content) % 256);
        }
        QElapsedTimer step;
        step.start();
        if (!encoder.addFrame(frame, &error))
        {
            fprintf(stderr, "%s\n", qPrintable(error));
            return 2;
        }
        const auto elapsed = step.nsecsElapsed();
        encodeNs += elapsed;
        maxNs = qMax(maxNs, elapsed);
        if (paced)
        {
            const qint64 remaining = (i + 1) * 1000 / 12 - timer.elapsed();
            if (remaining > 0)
                QThread::msleep(remaining);
        }
    }
    QElapsedTimer finish;
    finish.start();
    if (!encoder.finish(&error))
        return 3;
    const double finishMs = finish.nsecsElapsed() / 1e6;
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    printf("baselineMiB=%.2f peakMiB=%.2f deltaMiB=%.2f encodeMs=%.2f avgMs=%.2f maxMs=%.2f "
           "finishMs=%.2f totalMs=%lld cpuSeconds=%.3f bytes=%lld path=%s\n",
           base / 1048576., usage.ru_maxrss / 1048576., (usage.ru_maxrss - base) / 1048576.,
           encodeNs / 1e6, encodeNs / 360e6, maxNs / 1e6, finishMs, timer.elapsed(),
           double(std::clock() - cpu) / CLOCKS_PER_SEC, QFileInfo(path).size(), qPrintable(path));
    QMovie movie(path);
    QImageReader reader(path);
    int frames = 0, delay = 0;
    while (!reader.read().isNull())
    {
        ++frames;
        delay += reader.nextImageDelay();
    }
    printf("frames=%d durationMs=%d loop=%d movie=%d\n", frames, delay, reader.loopCount(),
           movie.jumpToFrame(0));
}
