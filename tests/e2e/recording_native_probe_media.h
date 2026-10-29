#pragma once
#include <QString>
struct RecordingProbeAudioInfo
{
    bool valid = false;
    int tracks = 0;
    bool aac = false;
    double sampleRate = 0;
    int channels = 0;
    qint64 durationMs = 0;
};
// 仅探针使用，不改变 RecordingMediaProbe 的公共契约。
RecordingProbeAudioInfo recordingProbeAudioInfo(const QString& path);

struct RecordingProbeUsage
{
    qint64 residentBytes = 0, peakBytes = 0;
    double cpuSeconds = 0;
};
RecordingProbeUsage recordingProbeProcessUsage();
