#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <array>

namespace tide
{
struct GranulatorEngineTestAccess;

class GranulatorEngine
{
public:
    static constexpr int maximumGrains = 64;

    struct Controls
    {
        float timeSeconds = 1.0f;
        float grainSizeMilliseconds = 40.0f;
        float mix = 0.5f;
        float density = 10.0f;
        float shape = 0.5f;
        float spread = 0.35f;
        float feedback = 0.0f;
        float tide = 0.0f;
        float drift = 0.2f;
    };

    struct GrainVisualState
    {
        std::uint64_t eventId = 0;
        double sourceDelaySamples = 0.0;
        double playbackSpeed = 1.0;
        float envelope = 0.0f;
        float pan = 0.0f;
        int ageInSamples = 0;
        int lengthInSamples = 0;
    };

    struct VisualFrame
    {
        std::array<GrainVisualState, maximumGrains> grains {};
        std::int64_t totalSamplesWritten = 0;
        double sampleRate = 0.0;
        std::uint64_t sequence = 0;
        std::uint64_t generation = 0;
        int grainCount = 0;
    };

    void prepare(double sampleRate,
                 int maximumBlockSize,
                 int channelCount,
                 float initialMix,
                 float initialFeedback = 0.0f,
                 float initialTide = 0.0f,
                 float initialDrift = 0.2f);
    void reset() noexcept;
    void resetPlayback(std::int64_t timelineSample = 0) noexcept;
    void resetHistory(std::int64_t timelineSample = 0) noexcept;
    void setRandomSeed(std::uint64_t seed) noexcept;
    void process(juce::AudioBuffer<float>& buffer,
                 const Controls& controls,
                 float* internalHistoryTap = nullptr,
                 int internalHistoryTapCapacity = 0) noexcept;
    void getVisualFrame(VisualFrame& destination) const noexcept;

private:
    friend struct GranulatorEngineTestAccess;

    struct Grain
    {
        double readPosition = 0.0;
        double playbackSpeed = 1.0;
        double sourceDelaySamples = 0.0;
        std::int64_t birthSample = -1;
        std::uint64_t eventId = 0;
        float shapeAtBirth = 0.5f;
        float envelopeMassAtBirth = 0.5f;
        float phaseScale = 1.0f; // 1 / max(1, lengthInSamples - 1)
        float tideAtBirth = 0.0f;
        float pan = 0.0f;
        float panAmount = 0.0f;
        int ageInSamples = 0;
        int lengthInSamples = 0;
        bool active = false;
        bool hasReadableSource = false;
    };

    void reconcilePopulation(const Controls& controls) noexcept;
    void scheduleBirths(const Controls& controls, float tide, float drift) noexcept;
    bool beginOneShot(Grain& grain,
                      const Controls& controls,
                      float tide,
                      float drift) noexcept;
    double nextBirthInterval(const Grain& grain,
                             const Controls& controls,
                             float drift) const noexcept;
    int requestedLengthInSamples(const Controls& controls) const noexcept;
    float randomUnit(std::int64_t eventSample, std::uint64_t stream) const noexcept;
    static std::uint64_t hash(std::uint64_t value) noexcept;
    static float wrappedUnit(float value) noexcept;
    static float roundedSawEnvelope(float phase) noexcept;
    static float grainEnvelope(float phase, float shape) noexcept;
    float readHistorySample(const float* channelData, double position) const noexcept;

    juce::AudioBuffer<float> history;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tideSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driftSmoother;
    std::array<Grain, maximumGrains> grains {};
    double currentSampleRate = 0.0;
    int currentChannelCount = 0;
    int historyLength = 0;
    int writePosition = 0;
    std::int64_t totalSamplesWritten = 0;
    std::int64_t timelineSample = 0;
    double nextBirthSample = 0.0;
    float nominalWetNormalization = 1.0f;
    float normalizationAttack = 0.0f;
    float normalizationRelease = 0.0f;
    int activeGrainCount = 0;
    int targetPopulation = 0;
    std::uint64_t birthOrdinal = 0;
    std::uint64_t randomSeed = 0x4d595df4d0f33173ULL;
};
} // namespace tide
