#pragma once
#include <QDateTime>
#include <QSize>
#include <QString>
namespace waibusnap
{
enum class RecordingOutput
{
    Video,
    Gif
};
enum class RecordingTargetKind
{
    Region,
    Window,
    Display
};
enum class RecordingResolution
{
    Native,
    P1080,
    P720
};
enum class RecordingQuality
{
    High,
    Medium,
    Low
};
enum class RecordingDiskState
{
    Normal,
    Low,
    Critical
};
struct RecordingPreferences
{
    // 空路径表示使用默认目录；读取时解析为绝对路径。
    QString videoDirectory;
    QString gifDirectory;
    RecordingOutput defaultOutput = RecordingOutput::Video;
    int videoFps = 30; // 30 / 60
    RecordingResolution resolution = RecordingResolution::Native;
    RecordingQuality quality = RecordingQuality::High;
    int gifMaxEdge = 1280; // 960 / 1280 / 0 = 原尺寸
    int gifFps = 12;       // 10 / 12 / 15
    int gifMaxSeconds = 30;
    int countdownSeconds = 3; // 0 / 3 / 5
    bool showCursor = true;
    bool systemAudio = true;
    bool microphone = false;
    QString microphoneDeviceId;        // 空 = 系统默认输入
    int systemVolumePercent = 100;     // 0–200
    int microphoneVolumePercent = 100; // 0–200
};
// 默认目录只拼接标准位置，不创建、不写入磁盘。
QString defaultRecordingVideoDirectory();
QString defaultRecordingGifDirectory();
// 目录为空或非绝对路径时回退到对应默认目录；同名文件已存在时追加 _2、_3，绝不覆盖。
QString suggestedRecordingPath(const QString& directory, RecordingOutput output,
                               QDateTime timestamp = QDateTime::currentDateTime());
// 保持宽高比、只缩小不放大；视频宽高取偶数；输出最小 2×2。无效来源返回空尺寸。
QSize recordingOutputSize(QSize sourcePixels, RecordingOutput output,
                          RecordingResolution resolution, int gifMaxEdge);
// bits/s 估算；只用于设置说明与磁盘预估，不替代编码器实际码率。
qint64 estimatedVideoBitrate(QSize pixels, int fps, RecordingQuality quality);
// 字节估算：pixels × frames × 0.12 + 1024，依据 R5 实测校准，明确为估算值。
qint64 estimatedGifBytes(QSize pixels, int fps, int seconds);
// < 512 MiB 为 Critical，< 2 GiB 为 Low，其余 Normal。
RecordingDiskState recordingDiskState(qint64 availableBytes);
}
