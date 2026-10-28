#include "platform/macos/recording_audio_pipeline.h"
#include "core/recording_audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
namespace waibusnap
{
namespace
{
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr qsizetype kJitterFrames = 2880; // 60 ms，超出即用静音补齐，不让主时钟等待。
CMTime endTime(const RecordingPcmChunk& chunk)
{
    return CMTimeAdd(chunk.pts, CMTimeMake(chunk.frames(), kRate));
}
} // namespace
bool RecordingAudioConverter::convert(CMSampleBufferRef source, CMTime pts,
                                      RecordingPcmChunk* output)
{
    if (!source || !output || !CMSampleBufferDataIsReady(source) || !CMTIME_IS_NUMERIC(pts))
        return false;
    CMAudioFormatDescriptionRef description = CMSampleBufferGetFormatDescription(source);
    const AudioStreamBasicDescription* asbd =
        description ? CMAudioFormatDescriptionGetStreamBasicDescription(description) : nullptr;
    const CMItemCount frames = CMSampleBufferGetNumSamples(source);
    // 只接收有界 PCM，不把异常设备格式传入分配器 / 转换器。
    if (!asbd || asbd->mFormatID != kAudioFormatLinearPCM || !std::isfinite(asbd->mSampleRate) ||
        asbd->mSampleRate < 8000 || asbd->mSampleRate > 192000 || asbd->mChannelsPerFrame == 0 ||
        asbd->mChannelsPerFrame > 32 || frames <= 0 || frames > asbd->mSampleRate)
        return false;
    AVAudioFormat* format = [[AVAudioFormat alloc] initWithCMAudioFormatDescription:description];
    if (!format)
        return false;
    if (!inputFormat_ || ![inputFormat_ isEqual:format])
    {
        AVAudioFormat* target = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                                 sampleRate:kRate
                                                                   channels:kChannels
                                                                interleaved:YES];
        converter_ = [[AVAudioConverter alloc] initFromFormat:format toFormat:target];
        inputFormat_ = format;
        // 实时流不能等待未来缓冲作为预热；时间戳由输入 PTS 统一锚定。
        converter_.primeMethod = AVAudioConverterPrimeMethod_None;
    }
    if (!converter_)
        return false;
    AVAudioPCMBuffer* input =
        [[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:AVAudioFrameCount(frames)];
    if (!input)
        return false;
    input.frameLength = AVAudioFrameCount(frames);
    if (CMSampleBufferCopyPCMDataIntoAudioBufferList(source, 0, int32_t(frames),
                                                     input.mutableAudioBufferList) != noErr)
        return false;
    struct ConversionInput
    {
        AVAudioFrameCount offset = 0;
        bool failed = false;
    };
    const auto supplied = std::make_shared<ConversionInput>();
    const AVAudioFrameCount capacity =
        AVAudioFrameCount(std::ceil(double(frames) * kRate / asbd->mSampleRate)) + 256;
    AVAudioPCMBuffer* converted =
        [[AVAudioPCMBuffer alloc] initWithPCMFormat:converter_.outputFormat frameCapacity:capacity];
    if (!converted)
        return false;
    NSError* error = nil;
    const AVAudioConverterOutputStatus status =
        [converter_ convertToBuffer:converted
                              error:&error
                 withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount count,
                                                    AVAudioConverterInputStatus* inputStatus) {
                   const AVAudioFrameCount remaining = input.frameLength - supplied->offset;
                   if (remaining == 0 || count == 0)
                   {
                       *inputStatus = AVAudioConverterInputStatus_NoDataNow;
                       return nil;
                   }
                   const AVAudioFrameCount offered = std::min(remaining, count);
                   // 转换器可能只请求 4096 帧；返回整块会使尾部样本被丢弃。
                   AVAudioPCMBuffer* portion = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format
                                                                             frameCapacity:offered];
                   if (!portion)
                   {
                       supplied->failed = true;
                       *inputStatus = AVAudioConverterInputStatus_NoDataNow;
                       return nil;
                   }
                   portion.frameLength = offered;
                   for (UInt32 i = 0; i < input.audioBufferList->mNumberBuffers; ++i)
                   {
                       const auto& sourceBuffer = input.audioBufferList->mBuffers[i];
                       const size_t bytesPerFrame = sourceBuffer.mDataByteSize / input.frameLength;
                       std::memcpy(portion.mutableAudioBufferList->mBuffers[i].mData,
                                   static_cast<const char*>(sourceBuffer.mData) +
                                       supplied->offset * bytesPerFrame,
                                   offered * bytesPerFrame);
                   }
                   supplied->offset += offered;
                   *inputStatus = AVAudioConverterInputStatus_HaveData;
                   return portion;
                 }];
    if (error || supplied->failed || status == AVAudioConverterOutputStatus_Error)
        return false;
    const auto* data = static_cast<const float*>(converted.audioBufferList->mBuffers[0].mData);
    output->pts = pts;
    output->samples.clear();
    if (converted.frameLength > 0)
    {
        if (!data)
            return false;
        output->samples.assign(data, data + size_t(converted.frameLength) * kChannels);
    }
    return true;
}
void RecordingAudioConverter::reset()
{
    converter_ = nil;
    inputFormat_ = nil;
}
CMSampleBufferRef createRecordingPcmSample(const RecordingPcmChunk& chunk)
{
    if (chunk.frames() <= 0 || !CMTIME_IS_NUMERIC(chunk.pts))
        return nullptr;
    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate = kRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = sizeof(float) * kChannels;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = asbd.mBytesPerPacket;
    asbd.mChannelsPerFrame = kChannels;
    asbd.mBitsPerChannel = 32;
    CMAudioFormatDescriptionRef format = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr, 0, nullptr, nullptr,
                                       &format) != noErr)
        return nullptr;
    const size_t bytes = chunk.samples.size() * sizeof(float);
    CMBlockBufferRef block = nullptr;
    CMSampleBufferRef sample = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault,
                                           nullptr, 0, bytes, 0, &block) == noErr)
    {
        if (CMBlockBufferReplaceDataBytes(chunk.samples.data(), block, 0, bytes) == noErr)
        {
            CMSampleTimingInfo timing = {CMTimeMake(1, kRate), chunk.pts, kCMTimeInvalid};
            const size_t sampleSize = sizeof(float) * kChannels;
            if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, chunk.frames(), 1,
                                          &timing, 1, &sampleSize, &sample) != noErr)
                sample = nullptr;
        }
        CFRelease(block);
    }
    CFRelease(format);
    return sample;
}
void RecordingAudioMixer::setSources(bool system, bool microphone)
{
    if (system_ != system)
    {
        systemQueue_.clear();
        systemFrames_ = 0;
        systemStarted_ = false;
    }
    if (!microphone)
    {
        microphoneQueue_.clear();
        microphoneFrames_ = 0;
        microphoneOffset_ = 0;
    }
    system_ = system;
    microphone_ = microphone;
}
void RecordingAudioMixer::setGains(float system, float microphone)
{
    systemGain_ = system;
    microphoneGain_ = microphone;
}
void RecordingAudioMixer::pushSystem(RecordingPcmChunk chunk)
{
    if (!system_ || chunk.frames() <= 0)
        return;
    systemStarted_ = true;
    systemFrames_ += chunk.frames();
    systemQueue_.push_back(std::move(chunk));
    while (systemFrames_ > kRate && systemQueue_.size() > 1)
    {
        systemFrames_ -= systemQueue_.front().frames();
        systemQueue_.pop_front();
        overflowed_ = true;
    }
}
void RecordingAudioMixer::pushMicrophone(RecordingPcmChunk chunk)
{
    if (!microphone_ || chunk.frames() <= 0 || (system_ && !systemStarted_))
        return;
    microphoneFrames_ += chunk.frames();
    microphoneQueue_.push_back(std::move(chunk));
    boundMicrophone();
}
void RecordingAudioMixer::boundMicrophone()
{
    while (microphoneFrames_ > kRate && !microphoneQueue_.empty())
    {
        const qsizetype drop = std::min(microphoneFrames_ - kRate,
                                        microphoneQueue_.front().frames() - microphoneOffset_);
        microphoneOffset_ += drop;
        microphoneFrames_ -= drop;
        overflowed_ = true;
        if (microphoneOffset_ == microphoneQueue_.front().frames())
        {
            microphoneQueue_.pop_front();
            microphoneOffset_ = 0;
        }
    }
}
std::optional<RecordingPcmChunk> RecordingAudioMixer::take(bool force)
{
    if (!system_)
    {
        if (microphoneQueue_.empty())
            return {};
        RecordingPcmChunk chunk = std::move(microphoneQueue_.front());
        microphoneQueue_.pop_front();
        microphoneFrames_ -= chunk.frames() - microphoneOffset_;
        if (microphoneOffset_ > 0)
        {
            chunk.pts = CMTimeAdd(chunk.pts, CMTimeMake(microphoneOffset_, kRate));
            chunk.samples.erase(chunk.samples.begin(),
                                chunk.samples.begin() + microphoneOffset_ * 2);
            microphoneOffset_ = 0;
        }
        mixRecordingAudio(chunk.samples.data(), nullptr, chunk.samples.data(), chunk.frames(),
                          kChannels, systemGain_, microphoneGain_);
        return chunk;
    }
    if (systemQueue_.empty())
        return {};
    const auto& next = systemQueue_.front();
    if (!force && microphone_ && systemFrames_ < kJitterFrames &&
        (microphoneQueue_.empty() ||
         CMTimeCompare(endTime(microphoneQueue_.back()), endTime(next)) < 0))
        return {};
    RecordingPcmChunk chunk = std::move(systemQueue_.front());
    systemQueue_.pop_front();
    systemFrames_ -= chunk.frames();
    std::vector<float> microphone(chunk.samples.size(), 0);
    while (!microphoneQueue_.empty())
    {
        auto& source = microphoneQueue_.front();
        const CMTime sourcePts = CMTimeAdd(source.pts, CMTimeMake(microphoneOffset_, kRate));
        const qsizetype position =
            qRound64(CMTimeGetSeconds(CMTimeSubtract(sourcePts, chunk.pts)) * kRate);
        if (position >= chunk.frames())
            break;
        qsizetype consume = 0;
        const qsizetype remaining = source.frames() - microphoneOffset_;
        if (position < 0)
            consume = std::min(-position, remaining);
        else
        {
            consume = std::min(chunk.frames() - position, remaining);
            std::copy_n(source.samples.data() + microphoneOffset_ * 2, consume * 2,
                        microphone.data() + position * 2);
        }
        microphoneOffset_ += consume;
        microphoneFrames_ -= consume;
        if (microphoneOffset_ == source.frames())
        {
            microphoneQueue_.pop_front();
            microphoneOffset_ = 0;
        }
    }
    mixRecordingAudio(chunk.samples.data(), chunk.samples.data(), microphone.data(), chunk.frames(),
                      kChannels, systemGain_, microphoneGain_);
    return chunk;
}
void RecordingAudioMixer::reset()
{
    systemQueue_.clear();
    microphoneQueue_.clear();
    systemFrames_ = 0;
    microphoneFrames_ = 0;
    microphoneOffset_ = 0;
    systemStarted_ = false;
}
bool RecordingAudioMixer::takeOverflowed()
{
    const bool result = overflowed_;
    overflowed_ = false;
    return result;
}
} // namespace waibusnap
