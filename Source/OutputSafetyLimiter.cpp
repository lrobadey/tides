#include "OutputSafetyLimiter.h"

#include <algorithm>
#include <cmath>

namespace tide
{
void OutputSafetyLimiter::prepare(const double sampleRate) noexcept
{
    const auto safeSampleRate = juce::jmax(1.0, sampleRate);
    releaseCoefficient = static_cast<float>(
        std::exp(-1.0 / (releaseSeconds * safeSampleRate)));
    reset();
}

void OutputSafetyLimiter::reset() noexcept
{
    currentGain = 1.0f;
}

void OutputSafetyLimiter::process(juce::AudioBuffer<float>& buffer) noexcept
{
    const auto channelCount = buffer.getNumChannels();
    const auto sampleCount = buffer.getNumSamples();

    for (auto sample = 0; sample < sampleCount; ++sample)
    {
        auto linkedPeak = 0.0f;
        for (auto channel = 0; channel < channelCount; ++channel)
        {
            auto& value = buffer.getWritePointer(channel)[sample];
            if (! std::isfinite(value))
                value = 0.0f;
            linkedPeak = juce::jmax(linkedPeak, std::abs(value));
        }

        const auto requiredGain = linkedPeak > ceilingGain
            ? ceilingGain / linkedPeak
            : 1.0f;

        if (requiredGain < currentGain)
            currentGain = requiredGain;
        else
            currentGain = 1.0f - (1.0f - currentGain) * releaseCoefficient;

        for (auto channel = 0; channel < channelCount; ++channel)
        {
            auto& value = buffer.getWritePointer(channel)[sample];
            value = juce::jlimit(-ceilingGain, ceilingGain, value * currentGain);
        }
    }
}

float OutputSafetyLimiter::getGainReduction() const noexcept
{
    return juce::jlimit(0.0f, 1.0f, 1.0f - currentGain);
}
} // namespace tide
