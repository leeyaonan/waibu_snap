#pragma once
#include <QtGlobal>
namespace waibusnap
{
// 交错 float32；空输入按静音处理，混音后限幅到 [-1, 1]。
// 非正帧数 / 声道数或空输出不写内存；允许 output 与任一路输入相同。
void mixRecordingAudio(float* output, const float* system, const float* microphone,
                       qsizetype frames, int channels, float systemGain, float microphoneGain);
} // namespace waibusnap
