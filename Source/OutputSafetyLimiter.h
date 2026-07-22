#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace tide
{
class OutputSafetyLimiter
{
public:
    static constexpr float ceilingDecibels = -1.0f;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void process(juce::AudioBuffer<float>& buffer) noexcept;
    float getGainReduction() const noexcept;

private:
    static constexpr float ceilingGain = 0.89125094f;
    static constexpr double releaseSeconds = 0.080;

    float currentGain = 1.0f;
    float releaseCoefficient = 0.0f;
};
} // namespace tide
