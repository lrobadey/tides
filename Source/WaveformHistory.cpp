#include "WaveformHistory.h"

#include <algorithm>
#include <cmath>

namespace tide
{
WaveformHistory::WaveformHistory()
    : minima(std::make_unique<std::atomic<float>[]>(bucketCapacity)),
      maxima(std::make_unique<std::atomic<float>[]>(bucketCapacity))
{
    prepare(48000.0);
}

void WaveformHistory::prepare(const double sampleRate) noexcept
{
    const auto validSampleRate = juce::jmax(1.0, sampleRate);
    samplesPerBucket = juce::jmax(1,
                                  juce::roundToInt(validSampleRate
                                                   / static_cast<double>(displayBucketsPerSecond)));
    currentBucketRate.store(static_cast<float>(validSampleRate
                                                / static_cast<double>(samplesPerBucket)),
                            std::memory_order_relaxed);
    currentSamplesPerBucket.store(samplesPerBucket, std::memory_order_relaxed);

    for (size_t index = 0; index < static_cast<size_t>(bucketCapacity); ++index)
    {
        minima[index].store(0.0f, std::memory_order_relaxed);
        maxima[index].store(0.0f, std::memory_order_relaxed);
    }

    reset(0);
}

void WaveformHistory::reset(const std::uint64_t generation) noexcept
{
    pendingMinimum = 0.0f;
    pendingMaximum = 0.0f;
    samplesInPendingBucket = 0;
    publishedBucketCount.store(0, std::memory_order_release);
    currentGeneration.store(generation, std::memory_order_release);
}

void WaveformHistory::push(const juce::AudioBuffer<float>& buffer,
                           const int inputChannelCount) noexcept
{
    const auto channels = juce::jlimit(0, buffer.getNumChannels(), inputChannelCount);
    if (channels == 0 || buffer.getNumSamples() == 0)
        return;

    auto bucketSequence = publishedBucketCount.load(std::memory_order_relaxed);
    for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        auto monoSample = 0.0f;
        for (auto channel = 0; channel < channels; ++channel)
            monoSample += buffer.getSample(channel, sample);
        monoSample /= static_cast<float>(channels);

        pushSample(monoSample, bucketSequence);
    }
}

void WaveformHistory::pushMono(const float* samples, const int sampleCount) noexcept
{
    if (samples == nullptr || sampleCount <= 0)
        return;

    auto bucketSequence = publishedBucketCount.load(std::memory_order_relaxed);
    for (auto sample = 0; sample < sampleCount; ++sample)
        pushSample(samples[sample], bucketSequence);
}

void WaveformHistory::pushSample(const float sample,
                                 std::uint64_t& bucketSequence) noexcept
{
    if (samplesInPendingBucket == 0)
    {
        pendingMinimum = sample;
        pendingMaximum = sample;
    }
    else
    {
        pendingMinimum = juce::jmin(pendingMinimum, sample);
        pendingMaximum = juce::jmax(pendingMaximum, sample);
    }

    ++samplesInPendingBucket;
    if (samplesInPendingBucket < samplesPerBucket)
        return;

    const auto index = static_cast<size_t>(bucketSequence
                                           % static_cast<std::uint64_t>(bucketCapacity));
    minima[index].store(pendingMinimum, std::memory_order_relaxed);
    maxima[index].store(pendingMaximum, std::memory_order_relaxed);
    ++bucketSequence;
    publishedBucketCount.store(bucketSequence, std::memory_order_release);
    samplesInPendingBucket = 0;
}

WaveformHistory::WindowInfo WaveformHistory::readWindow(
    std::vector<juce::Range<float>>& destination,
    const int columnCount,
    const float seconds) const
{
    destination.assign(static_cast<size_t>(juce::jmax(0, columnCount)), {});
    if (columnCount <= 0)
        return {};

    const auto generationBefore = currentGeneration.load(std::memory_order_acquire);
    const auto totalPublished = publishedBucketCount.load(std::memory_order_acquire);
    const auto bucketSize = currentSamplesPerBucket.load(std::memory_order_relaxed);
    const auto requestedBuckets = juce::jlimit(
        1,
        bucketCapacity,
        juce::roundToInt(std::clamp(seconds, 0.02f, static_cast<float>(maximumSeconds))
                         * currentBucketRate.load(std::memory_order_relaxed)));
    const auto availableBuckets = static_cast<int>(juce::jmin(
        totalPublished,
        static_cast<std::uint64_t>(requestedBuckets)));

    const WindowInfo info {
        static_cast<double>(totalPublished) * static_cast<double>(bucketSize),
        static_cast<double>(requestedBuckets) * static_cast<double>(bucketSize),
        generationBefore
    };

    if (availableBuckets == 0)
        return info;

    const auto firstSequence = totalPublished - static_cast<std::uint64_t>(availableBuckets);
    const auto missingBuckets = requestedBuckets - availableBuckets;

    for (auto column = 0; column < columnCount; ++column)
    {
        const auto requestedFirst = column * requestedBuckets / columnCount;
        const auto requestedNext = juce::jmin(
            requestedBuckets,
            juce::jmax(requestedFirst + 1,
                       (column + 1) * requestedBuckets / columnCount));
        if (requestedNext <= missingBuckets)
            continue;

        const auto firstOffset = juce::jmax(requestedFirst, missingBuckets)
            - missingBuckets;
        const auto nextOffset = requestedNext - missingBuckets;
        auto minimum = 0.0f;
        auto maximum = 0.0f;
        auto hasSample = false;

        for (auto offset = firstOffset; offset < juce::jmin(nextOffset, availableBuckets); ++offset)
        {
            const auto sequence = firstSequence + static_cast<std::uint64_t>(offset);
            const auto index = static_cast<size_t>(sequence
                                                   % static_cast<std::uint64_t>(bucketCapacity));
            const auto bucketMinimum = minima[index].load(std::memory_order_relaxed);
            const auto bucketMaximum = maxima[index].load(std::memory_order_relaxed);

            if (!hasSample)
            {
                minimum = bucketMinimum;
                maximum = bucketMaximum;
                hasSample = true;
            }
            else
            {
                minimum = juce::jmin(minimum, bucketMinimum);
                maximum = juce::jmax(maximum, bucketMaximum);
            }
        }

        destination[static_cast<size_t>(column)] = { minimum, maximum };
    }

    if (currentGeneration.load(std::memory_order_acquire) != generationBefore)
    {
        std::fill(destination.begin(), destination.end(), juce::Range<float>());
        return {};
    }

    return info;
}
} // namespace tide
