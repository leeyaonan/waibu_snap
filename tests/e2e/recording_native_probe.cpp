#include "interfaces/display_topology.h"
#include "interfaces/recording.h"
#include "recording_native_probe_media.h"
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace
{
class Transcript
{
  public:
    void line(const QString& text)
    {
        lines_.append(text);
        QTextStream(stdout) << text << '\n';
    }
    void save(const QString& path) const
    {
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        QTextStream out(&file);
        for (const QString& text : lines_)
            out << text << '\n';
    }

  private:
    QStringList lines_;
};

bool check(Transcript& transcript, bool condition, const QString& description)
{
    transcript.line(QStringLiteral("%1 %2").arg(
        condition ? QStringLiteral("PASS") : QStringLiteral("FAIL"), description));
    return condition;
}

struct ProbeOptions
{
    bool audio = false;
    bool pause = false;
    bool discard = false;
    bool region = false;
    bool requestPermission = false;
    int seconds = 2;
    QString keepDirectory;
    QString reportPath;
};

struct ProbeResult
{
    bool started = false;
    bool finished = false;
    bool discarded = false;
    bool failed = false;
    QString error;
    QString lastWarning;
    waibusnap::RecordingEvent finishedEvent;
};

int finish(Transcript& transcript, const ProbeOptions& options, int code)
{
    transcript.save(options.reportPath);
    if (!options.reportPath.isEmpty())
        QTextStream(stdout) << "REPORT: " << options.reportPath << '\n';
    return code;
}

int runProbe(const ProbeOptions& options)
{
    Transcript transcript;
    transcript.line(QStringLiteral("== WaibuSnap 录制原生探针 =="));
    transcript.line(
        QStringLiteral("日期：%1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    transcript.line(QStringLiteral("模式：%1；目标：%2；系统声音：%3；暂停时间轴：%4")
                        .arg(options.discard ? QStringLiteral("录制 1 秒后丢弃")
                             : options.pause ? QStringLiteral("1 秒 + 暂停 1 秒 + 1 秒")
                                             : QStringLiteral("%1 秒").arg(options.seconds))
                        .arg(options.region ? QStringLiteral("区域") : QStringLiteral("全屏"))
                        .arg(options.audio ? QStringLiteral("开") : QStringLiteral("关"))
                        .arg(options.pause ? QStringLiteral("验证") : QStringLiteral("不验证")));
    if (!waibusnap::recordingCaptureSupported())
    {
        transcript.line(QStringLiteral("SKIP: 当前平台不支持录制采集。"));
        return finish(transcript, options, 2);
    }
    if (!waibusnap::hasScreenRecordingPermission() && options.requestPermission)
    {
        transcript.line(QStringLiteral("请求屏幕录制权限…"));
        (void)waibusnap::requestScreenRecordingPermission();
        transcript.line(QStringLiteral(
            "等待授权（最多 120 秒）：请在系统弹窗点「允许」，或到系统设置 → 隐私与安全性 → "
            "屏幕与系统音频录制中打开 WaibuSnapRecordingProbe。"));
        QElapsedTimer timer;
        timer.start();
        while (!waibusnap::hasScreenRecordingPermission() && timer.elapsed() < 120000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            QThread::msleep(100);
        }
    }
    if (!waibusnap::hasScreenRecordingPermission())
    {
        transcript.line(QStringLiteral("SKIP: 未获得屏幕录制权限，未执行录制，也未伪造通过。"));
        transcript.line(
            QStringLiteral("授权步骤：系统设置 → 隐私与安全性 → 屏幕与系统音频录制 → 允许 "
                           "WaibuSnapRecordingProbe；授权后重新运行。"));
        transcript.line(QStringLiteral(
            "macOS 27 起请从访达或用 open 命令启动探针（终端直接执行不会继承授权）。"));
        return finish(transcript, options, 2);
    }
    const auto topology = waibusnap::createDisplayTopology();
    if (!topology)
    {
        transcript.line(QStringLiteral("SKIP: 无法创建显示器拓扑。"));
        return finish(transcript, options, 2);
    }
    const auto display = topology->cursorDisplay();
    if (!display.error.isEmpty() || display.target.id == 0)
    {
        transcript.line(QStringLiteral("SKIP: 未找到可录制显示器：%1").arg(display.error));
        return finish(transcript, options, 2);
    }
    const auto& target = display.target;
    const QSize displayPixels(qRound(target.logicalGeometry.width() * target.devicePixelRatio),
                              qRound(target.logicalGeometry.height() * target.devicePixelRatio));
    QRect sourcePixelRect(QPoint(0, 0), displayPixels);
    if (options.region)
    {
        // 取主屏中央的一半区域，验证 sourceRect 只截取选区而不是整屏。
        sourcePixelRect =
            QRect(displayPixels.width() / 4, displayPixels.height() / 4,
                  std::max(2, displayPixels.width() / 2), std::max(2, displayPixels.height() / 2));
    }
    QSize outputPixels =
        waibusnap::recordingOutputSize(sourcePixelRect.size(), waibusnap::RecordingOutput::Video,
                                       waibusnap::RecordingResolution::P720, 0);
    if (outputPixels.isEmpty())
        outputPixels = QSize(1280, 720);
    transcript.line(QStringLiteral("目标显示器：%1，逻辑 %2×%3，DPR %4，选区 %5×%6，输出 %7×%8")
                        .arg(target.id)
                        .arg(target.logicalGeometry.width())
                        .arg(target.logicalGeometry.height())
                        .arg(target.devicePixelRatio)
                        .arg(sourcePixelRect.width())
                        .arg(sourcePixelRect.height())
                        .arg(outputPixels.width())
                        .arg(outputPixels.height()));

    QTemporaryDir directory;
    if (!directory.isValid())
    {
        transcript.line(QStringLiteral("FAIL: 无法创建临时目录。"));
        return finish(transcript, options, 1);
    }
    directory.setAutoRemove(false);
    const QString tempPath =
        directory.filePath(QStringLiteral(".waibusnap-recording-%1.mp4.part")
                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QString finalPath = directory.filePath(QStringLiteral("WaibuSnap-native-probe.mp4"));
    transcript.line(QStringLiteral("临时目录：%1").arg(directory.path()));

    waibusnap::RecordingRequest request;
    request.output = waibusnap::RecordingOutput::Video;
    request.targetKind = options.region ? waibusnap::RecordingTargetKind::Region
                                        : waibusnap::RecordingTargetKind::Display;
    request.display = target;
    request.sourcePixelRect = sourcePixelRect;
    request.outputPixels = outputPixels;
    request.fps = 30;
    request.quality = waibusnap::RecordingQuality::Low;
    request.systemAudio = options.audio;
    request.showCursor = true;
    request.tempPath = tempPath;
    request.finalPath = finalPath;

    auto session = waibusnap::createRecordingSession();
    if (!session)
    {
        transcript.line(QStringLiteral("FAIL: createRecordingSession() 返回空。"));
        return finish(transcript, options, 1);
    }
    ProbeResult result;
    QEventLoop loop;
    QObject actionContext;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [&]
                     {
                         result.failed = true;
                         result.error = QStringLiteral("等待会话终态超时（30 秒）。");
                         loop.quit();
                     });
    timeout.start(30000);
    QString startError;
    const bool started = session->start(
        request,
        [&](waibusnap::RecordingEvent event)
        {
            switch (event.type)
            {
            case waibusnap::RecordingEventType::Started:
                result.started = true;
                transcript.line(QStringLiteral("事件：Started（%1×%2）")
                                    .arg(event.pixelSize.width())
                                    .arg(event.pixelSize.height()));
                if (options.audio)
                {
                    // 让系统混音里有确定的声音，便于验证音轨；不影响仅画面路径。
                    QProcess::startDetached(QStringLiteral("/usr/bin/afplay"),
                                            {QStringLiteral("/System/Library/Sounds/Glass.aiff")});
                }
                if (options.discard)
                {
                    QTimer::singleShot(1000, &actionContext,
                                       [&]
                                       {
                                           session->discard();
                                           QTimer::singleShot(800, &actionContext,
                                                              [&]
                                                              {
                                                                  result.discarded = true;
                                                                  loop.quit();
                                                              });
                                       });
                }
                else if (options.pause)
                {
                    QTimer::singleShot(1000, &actionContext, [&] { session->pause(); });
                    QTimer::singleShot(2000, &actionContext, [&] { session->resume(); });
                    QTimer::singleShot(3000, &actionContext, [&] { session->stop(); });
                }
                else
                {
                    QTimer::singleShot(options.seconds * 1000, &actionContext,
                                       [&] { session->stop(); });
                }
                break;
            case waibusnap::RecordingEventType::Progress:
                transcript.line(QStringLiteral("事件：Progress %1 ms，%2 字节，丢帧 %3")
                                    .arg(event.recordedMs)
                                    .arg(event.bytes)
                                    .arg(event.droppedFrames));
                break;
            case waibusnap::RecordingEventType::Warning:
                result.lastWarning = event.message;
                transcript.line(QStringLiteral("事件：Warning %1").arg(event.message));
                break;
            case waibusnap::RecordingEventType::AutoStop:
                transcript.line(QStringLiteral("事件：AutoStop %1").arg(event.message));
                break;
            case waibusnap::RecordingEventType::Finished:
                result.finished = true;
                result.finishedEvent = event;
                transcript.line(QStringLiteral("事件：Finished %1 ms，%2 字节，丢帧 %3，路径 %4")
                                    .arg(event.recordedMs)
                                    .arg(event.bytes)
                                    .arg(event.droppedFrames)
                                    .arg(event.path));
                loop.quit();
                break;
            case waibusnap::RecordingEventType::Failed:
                result.failed = true;
                result.error = event.message;
                transcript.line(QStringLiteral("事件：Failed %1（可恢复路径 %2）")
                                    .arg(event.message)
                                    .arg(event.path));
                loop.quit();
                break;
            case waibusnap::RecordingEventType::PermissionDenied:
                result.failed = true;
                result.error = event.message;
                transcript.line(QStringLiteral("事件：PermissionDenied %1").arg(event.message));
                loop.quit();
                break;
            }
        },
        &startError);
    if (!started)
    {
        transcript.line(QStringLiteral("FAIL: start 返回 false：%1").arg(startError));
        return finish(transcript, options, 1);
    }
    loop.exec();
    timeout.stop();
    QObject::disconnect(&actionContext, nullptr, nullptr, nullptr);
    session.reset();

    int failures = 0;
    const auto expect = [&](bool condition, const QString& description)
    {
        if (!check(transcript, condition, description))
            ++failures;
    };
    if (options.discard)
    {
        expect(result.discarded, QStringLiteral("丢弃流程已执行"));
        expect(!result.finished, QStringLiteral("丢弃后没有 Finished 事件"));
        expect(!QFileInfo::exists(finalPath), QStringLiteral("丢弃后成品不存在"));
        expect(!QFileInfo::exists(tempPath), QStringLiteral("丢弃后临时文件不存在"));
        const QStringList leftovers =
            QDir(directory.path())
                .entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden);
        expect(
            leftovers.isEmpty(),
            QStringLiteral("丢弃后没有残留 .part：%1").arg(leftovers.join(QStringLiteral(", "))));
        transcript.line(failures == 0 ? QStringLiteral("结果：PASS")
                                      : QStringLiteral("结果：FAIL（%1 项）").arg(failures));
        return finish(transcript, options, failures == 0 ? 0 : 1);
    }
    if (!result.finished)
    {
        transcript.line(QStringLiteral("FAIL: 会话未到达 Finished：%1").arg(result.error));
        return finish(transcript, options, 1);
    }
    expect(!result.failed, QStringLiteral("会话无失败事件"));
    expect(QFileInfo::exists(finalPath), QStringLiteral("成品存在：%1").arg(finalPath));
    expect(!QFileInfo::exists(tempPath), QStringLiteral("没有遗留 .part 临时文件"));
    const QStringList leftovers =
        QDir(directory.path()).entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden);
    expect(leftovers.isEmpty(),
           QStringLiteral("临时目录没有其他 .part：%1").arg(leftovers.join(QStringLiteral(", "))));

    const auto probe = waibusnap::createRecordingMediaProbe();
    expect(bool(probe), QStringLiteral("媒体探测可创建"));
    if (probe)
    {
        const auto info = probe->probe(finalPath);
        transcript.line(QStringLiteral("回读：valid=%1，时长 %2 ms，尺寸 %3×%4，错误 %5")
                            .arg(info.valid)
                            .arg(info.durationMs)
                            .arg(info.pixelSize.width())
                            .arg(info.pixelSize.height())
                            .arg(info.error));
        expect(info.valid, QStringLiteral("成品可被媒体探测读取"));
        expect(info.pixelSize == outputPixels, QStringLiteral("成品尺寸等于请求尺寸 %1×%2")
                                                   .arg(outputPixels.width())
                                                   .arg(outputPixels.height()));
        const qint64 minimum = options.pause ? 1600 : options.seconds * 1000 - 350;
        const qint64 maximum = options.pause ? 2600 : options.seconds * 1000 + 900;
        expect(info.durationMs >= minimum && info.durationMs <= maximum,
               QStringLiteral("成品时长 %1 ms 在 %2–%3 ms 内（暂停不计入）")
                   .arg(info.durationMs)
                   .arg(minimum)
                   .arg(maximum));
        const QImage thumbnail = probe->thumbnail(finalPath, QSize(320, 320));
        expect(!thumbnail.isNull(), QStringLiteral("首帧缩略图非空"));
        if (options.pause)
            expect(info.durationMs < 2900,
                   QStringLiteral("暂停 1 秒未进入成品（时长明显小于 3 秒）"));
    }
    const bool hasAudio = recordingProbeHasAudioTrack(finalPath);
    transcript.line(QStringLiteral("音轨：%1")
                        .arg(hasAudio ? QStringLiteral("存在") : QStringLiteral("不存在")));
    expect(hasAudio == options.audio, options.audio ? QStringLiteral("系统声音路径生成了音轨")
                                                    : QStringLiteral("无声路径没有生成空音轨"));
    expect(result.finishedEvent.droppedFrames >= 0,
           QStringLiteral("丢帧计数已上报（%1）").arg(result.finishedEvent.droppedFrames));

    if (!options.keepDirectory.isEmpty())
    {
        QDir().mkpath(options.keepDirectory);
        const QString keepPath = QDir(options.keepDirectory)
                                     .filePath(QStringLiteral("recording-native-probe-%1.mp4")
                                                   .arg(QDateTime::currentDateTime().toString(
                                                       QStringLiteral("yyyyMMdd-HHmmss"))));
        if (QFile::copy(finalPath, keepPath))
            transcript.line(QStringLiteral("证据副本：%1").arg(keepPath));
        else
            transcript.line(QStringLiteral("WARN: 证据副本写入失败：%1").arg(keepPath));
    }
    transcript.line(failures == 0 ? QStringLiteral("结果：PASS")
                                  : QStringLiteral("结果：FAIL（%1 项）").arg(failures));
    return finish(transcript, options, failures == 0 ? 0 : 1);
}
} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("WaibuSnapRecordingProbe"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("WaibuSnap v009 R2 录制原生探针"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("audio"), QStringLiteral("开启系统声音并断言音轨存在。")});
    parser.addOption({QStringLiteral("pause"),
                      QStringLiteral("录 1 秒 → 暂停 1 秒 → 再录 1 秒，断言成片约 2 秒。")});
    parser.addOption({QStringLiteral("discard"),
                      QStringLiteral("录制 1 秒后丢弃，断言无成品、无 .part、无 Finished。")});
    parser.addOption(
        {QStringLiteral("region"), QStringLiteral("改为录制主屏中央一半区域，验证 sourceRect。")});
    parser.addOption(
        {QStringLiteral("request-permission"), QStringLiteral("未授权时主动请求屏幕录制权限。")});
    parser.addOption({QStringLiteral("seconds"), QStringLiteral("普通模式录制秒数。"),
                      QStringLiteral("seconds"), QStringLiteral("2")});
    parser.addOption({QStringLiteral("keep"), QStringLiteral("成功后复制成品到指定目录。"),
                      QStringLiteral("directory")});
    parser.addOption({QStringLiteral("report"), QStringLiteral("把完整记录写入指定文件。"),
                      QStringLiteral("path")});
    parser.process(app);
    ProbeOptions options;
    options.audio = parser.isSet(QStringLiteral("audio"));
    options.pause = parser.isSet(QStringLiteral("pause"));
    options.discard = parser.isSet(QStringLiteral("discard"));
    options.region = parser.isSet(QStringLiteral("region"));
    options.requestPermission = parser.isSet(QStringLiteral("request-permission"));
    options.seconds = qBound(1, parser.value(QStringLiteral("seconds")).toInt(), 10);
    options.keepDirectory = parser.value(QStringLiteral("keep"));
    options.reportPath = parser.value(QStringLiteral("report"));
    if (!options.reportPath.isEmpty())
        options.reportPath = QFileInfo(options.reportPath).absoluteFilePath();
    if (!options.keepDirectory.isEmpty())
        options.keepDirectory = QFileInfo(options.keepDirectory).absoluteFilePath();
    return runProbe(options);
}
