#include "core/recording_audio.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace waibusnap
{
void mixRecordingAudio(float* output, const float* system, const float* microphone,
                       qsizetype frames, int channels, float systemGain, float microphoneGain)
{
    if (!output || frames <= 0 || channels <= 0 ||
        frames > std::numeric_limits<qsizetype>::max() / channels)
        return;
    for (qsizetype i = 0; i < frames * channels; ++i)
    {
        const float mixed = (system ? system[i] * systemGain : 0.0f) +
                            (microphone ? microphone[i] * microphoneGain : 0.0f);
        output[i] = std::isnan(mixed) ? 0.0f : std::clamp(mixed, -1.0f, 1.0f);
    }
}
} // namespace waibusnap
