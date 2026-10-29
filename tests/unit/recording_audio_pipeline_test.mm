#include "platform/macos/recording_audio_pipeline.h"
#include <QtTest>
#include <cmath>
namespace
{
waibusnap::RecordingPcmChunk chunk(int frame, int count, float value)
{
    return {CMTimeMake(frame, 48000), std::vector<float>(size_t(count) * 2, value)};
}
CMSampleBufferRef inputSample(double rate, int channels, bool interleaved, bool integer)
{
    AVAudioFormat* format = [[AVAudioFormat alloc]
        initWithCommonFormat:integer ? AVAudioPCMFormatInt16 : AVAudioPCMFormatFloat32
                  sampleRate:rate
                    channels:AVAudioChannelCount(channels)
                 interleaved:interleaved];
    AVAudioPCMBuffer* pcm =
        [[AVAudioPCMBuffer alloc] initWithPCMFormat:format
                                      frameCapacity:AVAudioFrameCount(rate / 10)];
    pcm.frameLength = pcm.frameCapacity;
    for (UInt32 i = 0; i < pcm.mutableAudioBufferList->mNumberBuffers; ++i)
    {
        auto& buffer = pcm.mutableAudioBufferList->mBuffers[i];
        if (integer)
            std::fill_n(static_cast<int16_t*>(buffer.mData), buffer.mDataByteSize / sizeof(int16_t),
                        int16_t(8192));
        else
            std::fill_n(static_cast<float*>(buffer.mData), buffer.mDataByteSize / sizeof(float),
                        i == 0 ? 0.25f : -0.25f);
    }
    CMAudioFormatDescriptionRef description = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, format.streamDescription, 0, nullptr, 0,
                                       nullptr, nullptr, &description) != noErr)
        return nullptr;
    CMSampleBufferRef sample = nullptr;
    CMSampleTimingInfo timing = {CMTimeMake(1, int32_t(rate)), CMTimeMake(1, 1), kCMTimeInvalid};
    if (CMSampleBufferCreate(kCFAllocatorDefault, nullptr, false, nullptr, nullptr, description,
                             pcm.frameLength, 1, &timing, 0, nullptr, &sample) == noErr)
    {
        if (CMSampleBufferSetDataBufferFromAudioBufferList(
                sample, kCFAllocatorDefault, kCFAllocatorDefault, 0, pcm.audioBufferList) != noErr)
        {
            CFRelease(sample);
            sample = nullptr;
        }
    }
    CFRelease(description);
    return sample;
}
} // namespace
class RecordingAudioPipelineTest final : public QObject
{
    Q_OBJECT
  private slots:
    void conversion_data()
    {
        QTest::addColumn<double>("rate");
        QTest::addColumn<int>("channels");
        QTest::addColumn<bool>("interleaved");
        QTest::addColumn<bool>("integer");
        QTest::newRow("44100-mono-int16") << 44100.0 << 1 << true << true;
        QTest::newRow("48000-planar-stereo") << 48000.0 << 2 << false << false;
        QTest::newRow("96000-stereo") << 96000.0 << 2 << true << false;
    }
    void conversion()
    {
        QFETCH(double, rate);
        QFETCH(int, channels);
        QFETCH(bool, interleaved);
        QFETCH(bool, integer);
        CMSampleBufferRef sample = inputSample(rate, channels, interleaved, integer);
        QVERIFY(sample);
        waibusnap::RecordingAudioConverter converter;
        for (int i = 0; i < 3; ++i)
        {
            waibusnap::RecordingPcmChunk output;
            const bool ok = converter.convert(sample, CMTimeMake(i, 10), &output);
            QVERIFY(ok);
            QVERIFY2(
                output.frames() >= 4600 && output.frames() <= 4900,
                qPrintable(QStringLiteral("iteration=%1 frames=%2").arg(i).arg(output.frames())));
            QCOMPARE(output.pts.value, qint64(i));
            QVERIFY(std::all_of(output.samples.cbegin(), output.samples.cend(), [](float value)
                                { return std::isfinite(value) && qAbs(value) <= 1; }));
            const auto middle = size_t(output.frames() / 2) * 2;
            QVERIFY(qAbs(output.samples[middle] - 0.25f) < 0.02f);
            const float right = !interleaved && channels == 2 ? -0.25f : 0.25f;
            QVERIFY(qAbs(output.samples[middle + 1] - right) < 0.02f);
        }
        CFRelease(sample);
    }
    void formatChangeAndReset()
    {
        waibusnap::RecordingAudioConverter converter;
        for (double rate : {44100.0, 48000.0, 96000.0, 44100.0})
        {
            CMSampleBufferRef sample = inputSample(rate, 1, true, true);
            QVERIFY(sample);
            waibusnap::RecordingPcmChunk output;
            QVERIFY(converter.convert(sample, CMTimeMake(1, 1), &output));
            QVERIFY2(
                output.frames() > 4600,
                qPrintable(QStringLiteral("rate=%1 frames=%2").arg(rate).arg(output.frames())));
            CFRelease(sample);
        }
        converter.reset();
        waibusnap::RecordingPcmChunk output;
        QVERIFY(!converter.convert(nullptr, kCMTimeZero, &output));
    }
    void pcmPackaging()
    {
        const auto input = chunk(48000, 480, 0.25f);
        CMSampleBufferRef sample = waibusnap::createRecordingPcmSample(input);
        QVERIFY(sample);
        QCOMPARE(CMSampleBufferGetNumSamples(sample), CMItemCount(480));
        QVERIFY(qAbs(CMTimeGetSeconds(CMSampleBufferGetDuration(sample)) - 0.01) < 0.000001);
        const auto* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(
            CMSampleBufferGetFormatDescription(sample));
        QVERIFY(asbd);
        QCOMPARE(asbd->mSampleRate, 48000.0);
        QCOMPARE(asbd->mChannelsPerFrame, UInt32(2));
        QVERIFY(!(asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved));
        CFRelease(sample);
    }
    void singleSources()
    {
        waibusnap::RecordingAudioMixer mixer;
        mixer.setSources(true, false);
        mixer.setGains(2, 0);
        mixer.pushSystem(chunk(0, 480, 0.75f));
        auto output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], 1.0f);
        QVERIFY(!mixer.take());
        mixer.setSources(false, true);
        mixer.setGains(0, 2);
        mixer.pushMicrophone(chunk(480, 480, -0.75f));
        output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], -1.0f);
    }
    void discardsBeforeFirstSystemAndPadsShortage()
    {
        waibusnap::RecordingAudioMixer mixer;
        mixer.setSources(true, true);
        mixer.pushMicrophone(chunk(0, 100, 0.5f));
        mixer.pushSystem(chunk(0, 480, 0.25f));
        auto output = mixer.take(true);
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.25f);
        mixer.pushSystem(chunk(480, 480, 0.25f));
        mixer.pushMicrophone(chunk(480, 240, 0.5f));
        output = mixer.take(true);
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.75f);
        QCOMPARE(output->samples[478], 0.75f);
        QCOMPARE(output->samples[480], 0.25f);
    }
    void alignsTimestampsAndConsumesAcrossBuffers()
    {
        waibusnap::RecordingAudioMixer mixer;
        mixer.setSources(true, true);
        mixer.pushSystem(chunk(480, 480, 0.25f));
        mixer.pushMicrophone(chunk(240, 960, 0.5f));
        auto output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.75f);
        mixer.pushSystem(chunk(960, 480, 0.25f));
        output = mixer.take(true);
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.75f);
        QCOMPARE(output->samples[480], 0.25f);
        mixer.pushSystem(chunk(1440, 480, 0.25f));
        mixer.pushMicrophone(chunk(1680, 240, 0.5f));
        output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.25f);
        QCOMPARE(output->samples[480], 0.75f);
    }
    void jitterWaitIsBounded()
    {
        waibusnap::RecordingAudioMixer mixer;
        mixer.setSources(true, true);
        for (int i = 0; i < 6; ++i)
        {
            mixer.pushSystem(chunk(i * 480, 480, 0.25f));
            if (i < 5)
                QVERIFY(!mixer.take());
        }
        QVERIFY(mixer.take());
    }
    void overflowDropsOldestAndRealigns()
    {
        waibusnap::RecordingAudioMixer mixer;
        mixer.setSources(true, true);
        mixer.pushSystem(chunk(48000, 480, 0.25f));
        mixer.pushMicrophone(chunk(0, 24000, 0.5f));
        mixer.pushMicrophone(chunk(24000, 48000, 0.5f));
        QVERIFY(mixer.takeOverflowed());
        QVERIFY(!mixer.takeOverflowed());
        auto output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.75f);
        mixer.setSources(false, true);
        output = mixer.take();
        QVERIFY(output);
        QCOMPARE(qRound64(CMTimeGetSeconds(output->pts) * 48000), qint64(48480));
    }
    void resetAndSourceFailure()
    {
        waibusnap::RecordingAudioMixer mixer;
        mixer.setSources(true, true);
        mixer.pushSystem(chunk(0, 480, 0.25f));
        mixer.pushMicrophone(chunk(0, 480, 0.5f));
        mixer.reset();
        QVERIFY(!mixer.take(true));
        mixer.pushMicrophone(chunk(480, 480, 0.5f));
        mixer.pushSystem(chunk(480, 480, 0.25f));
        auto output = mixer.take(true);
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.25f);
        mixer.pushSystem(chunk(960, 480, 0.25f));
        mixer.setSources(true, false);
        output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.25f);
        mixer.setSources(false, true);
        mixer.pushMicrophone(chunk(1440, 480, 0.5f));
        output = mixer.take();
        QVERIFY(output);
        QCOMPARE(output->samples[0], 0.5f);
    }
};
int main(int argc, char* argv[])
{
    RecordingAudioPipelineTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "recording_audio_pipeline_test.moc"
