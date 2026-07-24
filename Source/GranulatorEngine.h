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
    static constexpr int maximumLanes = 64;
    static constexpr int maximumGrains = maximumLanes;

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
        bool sync = false;
        int syncDivision = 4;
        bool gridEnd = true;
        float activity = 0.5f;
        float envelopePhase = 0.0f;
    };

    struct Timing
    {
        double bpm = 0.0;
        double ppqPosition = 0.0;
        bool clockValid = false;
        bool playing = false;
        bool discontinuity = false;
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
        float lifeProgress = 0.0f;
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
                 const Timing& timing,
                 float* internalHistoryTap = nullptr,
                 int internalHistoryTapCapacity = 0) noexcept;
    void process(juce::AudioBuffer<float>& buffer,
                 const Controls& controls,
                 float* internalHistoryTap = nullptr,
                 int internalHistoryTapCapacity = 0) noexcept
    {
        process(buffer, controls, {}, internalHistoryTap, internalHistoryTapCapacity);
    }
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
        float envelopePhaseAtBirth = 0.0f;
        float envelopeMassAtBirth = 0.5f;
        float phaseScale = 1.0f; // 1 / max(1, lengthInSamples - 1)
        float tideAtBirth = 0.0f;
        float pan = 0.0f;
        float panAmount = 0.0f;
        int ageInSamples = 0;
        int lengthInSamples = 0;
        bool active = false;
        bool hasReadableSource = false;
        bool syncVoice = false;
        int sourceLane = -1;
        double lifeProgress = 0.0;
        double lifetimePpq = 0.0;
        int releaseSamplesRemaining = 0;
        int releaseSamplesTotal = 0;
        float lastOutputLeft = 0.0f;
        float lastOutputRight = 0.0f;
    };

    struct FreeState
    {
        struct Lane
        {
            double nextOnsetSample = 0.0;
            float rateTendency = 1.0f;
            std::uint64_t articulationOrdinal = 0;
            bool active = false;
        };
        std::array<Lane, maximumLanes> lanes {};
        int activeLaneCount = 0;
    };

    struct SyncState
    {
        struct Lane
        {
            double offsetPpq = 0.0;
            double onsetPpq = 0.0;
            std::int64_t sourceAnchorSample = 0;
            bool anchorValid = false;
            bool onsetPending = false;
        };
        // Holds only the cut voice's final output level while it decays over
        // one millisecond; it never reads history or occupies a grain slot.
        struct CutTail
        {
            float levelLeft = 0.0f;
            float levelRight = 0.0f;
            int samplesRemaining = 0;
            int samplesTotal = 0;
        };
        std::array<Lane, maximumLanes> lanes {};
        std::array<CutTail, maximumLanes> cutTails {};
        int activeCutTails = 0;
        double phaseOriginPpq = 0.0;
        double nextBoundaryPpq = 0.0;
        double durationPpq = 0.5;
        std::uint64_t boundaryOrdinal = 0;
        int density = 1;
        int division = 4;
        float drift = 0.0f;
        bool gridEnd = true;
        bool active = false;
        bool awaitingBoundary = false;
        bool clockWasValid = false;
        bool wasPlaying = false;
        bool disabling = false;
    };

    void reconcileFreeLanes(const Controls& controls) noexcept;
    void processFreeEvents(const Controls& controls, float tide, float drift) noexcept;
    bool beginOneShot(Grain& grain,
                      int laneIndex,
                      const Controls& controls,
                      float tide,
                      float drift) noexcept;
    double laneRestSamples(FreeState::Lane& lane,
                           int laneIndex,
                           int grainLengthSamples,
                           float wind,
                           float drift) noexcept;
    int requestedLengthInSamples(const Controls& controls) const noexcept;
    float randomUnit(std::int64_t eventSample, std::uint64_t stream) const noexcept;
    static std::uint64_t hash(std::uint64_t value) noexcept;
    static float wrappedUnit(float value) noexcept;
    static float roundedSawEnvelope(float phase) noexcept;
    static float warpEnvelopePhase(float phase, float nativePeak, float shift) noexcept;
    static float grainEnvelope(float phase, float shape, float envelopePhase = 0.0f) noexcept;
    static float envelopeMass(float shape, float envelopePhase) noexcept;
    float readHistorySample(const float* channelData, double position) const noexcept;
    void processSyncEvents(const Controls&, const Timing&, double currentPpq) noexcept;
    void commitSyncBoundary(const Controls&, double boundaryPpq, double bpm) noexcept;
    bool beginSyncLane(int laneIndex, const Controls&, double bpm) noexcept;
    void releaseAllSyncVoices() noexcept;
    static double divisionPpq(int division) noexcept;

    juce::AudioBuffer<float> history;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tideSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driftSmoother;
    std::array<Grain, maximumGrains> grains {};
    FreeState freeState;
    double currentSampleRate = 0.0;
    int currentChannelCount = 0;
    int historyLength = 0;
    int writePosition = 0;
    std::int64_t totalSamplesWritten = 0;
    std::int64_t timelineSample = 0;

    float nominalWetNormalization = 1.0f;
    float normalizationAttack = 0.0f;
    float normalizationRelease = 0.0f;
    int activeGrainCount = 0;

    std::uint64_t birthOrdinal = 0;
    std::uint64_t randomSeed = 0x4d595df4d0f33173ULL;
    SyncState syncState;
};
} // namespace tide
