#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace tide
{
class WaveformHistory
{
public:
    struct WindowInfo
    {
        double newestSampleSequence = 0.0;
        double visibleSampleCount = 0.0;
        std::uint64_t generation = 0;
    };

    WaveformHistory();

    void prepare(double sampleRate) noexcept;
    void reset(std::uint64_t generation = 0) noexcept;
    void push(const juce::AudioBuffer<float>& buffer, int inputChannelCount) noexcept;
    void pushMono(const float* samples, int sampleCount) noexcept;
    WindowInfo readWindow(std::vector<juce::Range<float>>& destination,
                          int columnCount,
                          float seconds) const;

private:
    static constexpr int displayBucketsPerSecond = 2000;
    static constexpr int maximumSeconds = 15;
    static constexpr int bucketCapacity = displayBucketsPerSecond * maximumSeconds + 2;

    std::unique_ptr<std::atomic<float>[]> minima;
    std::unique_ptr<std::atomic<float>[]> maxima;
    std::atomic<std::uint64_t> publishedBucketCount { 0 };
    std::atomic<float> currentBucketRate { static_cast<float>(displayBucketsPerSecond) };
    std::atomic<int> currentSamplesPerBucket { 1 };
    std::atomic<std::uint64_t> currentGeneration { 0 };

    void pushSample(float sample, std::uint64_t& bucketSequence) noexcept;
    float pendingMinimum = 0.0f;
    float pendingMaximum = 0.0f;
    int samplesInPendingBucket = 0;
    int samplesPerBucket = 1;
};
} // namespace tide
