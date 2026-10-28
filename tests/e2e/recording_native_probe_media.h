#pragma once
#include <QString>

// 原生探针专用：用 AVFoundation 判断成品是否带音轨，避免把音频探测塞进 R2 公共接口。
bool recordingProbeHasAudioTrack(const QString& path);
