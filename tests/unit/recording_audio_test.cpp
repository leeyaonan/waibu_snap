#include "core/recording_audio.h"
#include <QtTest>
#include <array>
#include <limits>
#include <vector>
class RecordingAudioTest final : public QObject
{
    Q_OBJECT
  private slots:
    void sourcesAndGains_data()
    {
        QTest::addColumn<int>("sources");
        QTest::addColumn<float>("systemGain");
        QTest::addColumn<float>("microphoneGain");
        for (int sources = 0; sources < 4; ++sources)
            for (int system = 0; system <= 2; ++system)
                for (int microphone = 0; microphone <= 2; ++microphone)
                    QTest::newRow(qPrintable(
                        QStringLiteral("%1-%2-%3").arg(sources).arg(system).arg(microphone)))
                        << sources << float(system) << float(microphone);
    }
    void sourcesAndGains()
    {
        QFETCH(int, sources);
        QFETCH(float, systemGain);
        QFETCH(float, microphoneGain);
        constexpr qsizetype frames = 48000;
        std::vector<float> system(frames * 2), microphone(frames * 2), output(frames * 2);
        for (qsizetype i = 0; i < frames * 2; ++i)
        {
            system[i] = i % 2 == 0 ? 0.2f : -0.2f;
            microphone[i] = i % 2 == 0 ? 0.3f : -0.3f;
        }
        waibusnap::mixRecordingAudio(output.data(), sources & 1 ? system.data() : nullptr,
                                     sources & 2 ? microphone.data() : nullptr, frames, 2,
                                     systemGain, microphoneGain);
        const float expected =
            (sources & 1 ? 0.2f * systemGain : 0) + (sources & 2 ? 0.3f * microphoneGain : 0);
        for (qsizetype i = 0; i < frames * 2; ++i)
            QCOMPARE(output[i], i % 2 == 0 ? expected : -expected);
    }
    void clipsAndAllowsInPlace()
    {
        std::array<float, 4> system{0.9f, -0.9f, 0.3f, -0.3f};
        const std::array<float, 4> microphone{0.8f, -0.8f, -0.4f, 0.4f};
        waibusnap::mixRecordingAudio(system.data(), system.data(), microphone.data(), 2, 2, 2, 2);
        QCOMPARE(system[0], 1.0f);
        QCOMPARE(system[1], -1.0f);
        QVERIFY(qAbs(system[2] + 0.2f) < 0.000001f);
        QVERIFY(qAbs(system[3] - 0.2f) < 0.000001f);
    }
    void boundaries()
    {
        float output = 0.75f;
        const float input = 0.25f;
        for (const qsizetype frames : {qsizetype(-1), qsizetype(0)})
        {
            waibusnap::mixRecordingAudio(&output, &input, nullptr, frames, 1, 1, 1);
            QCOMPARE(output, 0.75f);
        }
        waibusnap::mixRecordingAudio(&output, &input, nullptr, 1, 0, 1, 1);
        QCOMPARE(output, 0.75f);
        waibusnap::mixRecordingAudio(&output, &input, nullptr,
                                     std::numeric_limits<qsizetype>::max(), 2, 1, 1);
        QCOMPARE(output, 0.75f);
        waibusnap::mixRecordingAudio(nullptr, &input, nullptr, 1, 1, 1, 1);
        waibusnap::mixRecordingAudio(&output, &input, nullptr, 1, 1, 2, 1);
        QCOMPARE(output, 0.5f);
        waibusnap::mixRecordingAudio(&output, nullptr, nullptr, 1, 1, 2, 2);
        QCOMPARE(output, 0.0f);
    }
    void deterministic()
    {
        std::array<float, 8> system{0.1f, -0.2f, 0.3f, -0.4f, 0.9f, -0.9f, 0, 0};
        std::array<float, 8> first{}, second{};
        waibusnap::mixRecordingAudio(first.data(), system.data(), system.data(), 4, 2, 1, 2);
        waibusnap::mixRecordingAudio(second.data(), system.data(), system.data(), 4, 2, 1, 2);
        QVERIFY(first == second);
    }
};
int main(int argc, char* argv[])
{
    RecordingAudioTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "recording_audio_test.moc"
