#pragma once
#import <AVFoundation/AVFoundation.h>
#include <QtGlobal>
#include <deque>
#include <optional>
#include <vector>
namespace waibusnap
{
// 平台内私有 PCM：48 kHz / 双声道 / 交错 float32，PTS 使用 host clock。
struct RecordingPcmChunk
{
    CMTime pts = kCMTimeInvalid;
    std::vector<float> samples;
    qsizetype frames() const { return qsizetype(samples.size() / 2); }
};
class RecordingAudioConverter
{
  public:
    bool convert(CMSampleBufferRef source, CMTime pts, RecordingPcmChunk* output);
    void reset();

  private:
    AVAudioFormat* __strong inputFormat_ = nil;
    AVAudioConverter* __strong converter_ = nil;
};
CMSampleBufferRef createRecordingPcmSample(const RecordingPcmChunk& chunk) CF_RETURNS_RETAINED;

// 仅在录制工作队列访问；短暂等待另一来源以吸收两个回调队列的到达抖动。
class RecordingAudioMixer
{
  public:
    void setSources(bool system, bool microphone);
    void setGains(float system, float microphone);
    void pushSystem(RecordingPcmChunk chunk);
    void pushMicrophone(RecordingPcmChunk chunk);
    std::optional<RecordingPcmChunk> take(bool force = false);
    void reset();
    bool takeOverflowed();

  private:
    void boundMicrophone();
    bool system_ = false;
    bool microphone_ = false;
    bool systemStarted_ = false;
    bool overflowed_ = false;
    float systemGain_ = 1;
    float microphoneGain_ = 1;
    qsizetype systemFrames_ = 0;
    qsizetype microphoneFrames_ = 0;
    qsizetype microphoneOffset_ = 0;
    std::deque<RecordingPcmChunk> systemQueue_;
    std::deque<RecordingPcmChunk> microphoneQueue_;
};
} // namespace waibusnap
