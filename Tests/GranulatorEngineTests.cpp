#include "../Source/GranulatorEngine.h"
#include "../Source/ParameterIDs.h"
#include "../Source/PluginProcessor.h"
#include "../Source/WaveformHistory.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace tide
{
struct GranulatorEngineTestAccess
{
    struct VoiceState
    {
        int slot = 0;
        int age = 0;
        int length = 0;
        std::int64_t birthSample = -1;
        std::uint64_t eventId = 0;
        float shapeAtBirth = 0.5f;
        double sourceDelaySamples = 0.0;
        double playbackSpeed = 1.0;
        float pan = 0.0f;
        float panAmount = 0.0f;
        bool readable = false;
    };

    static int activeCount(const GranulatorEngine& engine) noexcept
    {
        return static_cast<int>(std::count_if(engine.grains.begin(),
                                              engine.grains.end(),
                                              [](const auto& grain)
                                              {
                                                  return grain.active;
                                              }));
    }

    static int simulatedPopulation(const GranulatorEngine& engine) noexcept
    {
        return engine.targetPopulation;
    }

    static std::int64_t timeline(const GranulatorEngine& engine) noexcept
    {
        return engine.timelineSample;
    }

    static std::uint64_t birthsStarted(const GranulatorEngine& engine) noexcept
    {
        return engine.birthOrdinal;
    }

    static int requestedLengthInSamples(const GranulatorEngine& engine,
                                        const GranulatorEngine::Controls& controls) noexcept
    {
        return engine.requestedLengthInSamples(controls);
    }

    static std::vector<VoiceState> voiceStates(const GranulatorEngine& engine)
    {
        std::vector<VoiceState> result;
        result.reserve(engine.grains.size());
        for (size_t index = 0; index < engine.grains.size(); ++index)
        {
            const auto& grain = engine.grains[index];
            if (! grain.active)
                continue;

            result.push_back({ static_cast<int>(index),
                               grain.ageInSamples,
                               grain.lengthInSamples,
                               grain.birthSample,
                               grain.eventId,
                               grain.shapeAtBirth,
                               grain.sourceDelaySamples,
                               grain.playbackSpeed,
                               grain.pan,
                               grain.panAmount,
                               grain.hasReadableSource });
        }
        return result;
    }

    static bool visualFrameMatchesEngine(const GranulatorEngine& engine) noexcept
    {
        GranulatorEngine::VisualFrame frame;
        engine.getVisualFrame(frame);
        auto frameIndex = 0;

        if (frame.totalSamplesWritten != engine.totalSamplesWritten
            || std::memcmp(&frame.sampleRate,
                           &engine.currentSampleRate,
                           sizeof(double)) != 0)
        {
            return false;
        }

        for (const auto& grain : engine.grains)
        {
            if (! grain.active || ! grain.hasReadableSource)
                continue;

            if (frameIndex >= frame.grainCount)
                return false;

            auto expectedDelay = static_cast<double>(engine.writePosition)
                - grain.readPosition;
            while (expectedDelay < 0.0)
                expectedDelay += static_cast<double>(engine.historyLength);
            while (expectedDelay >= static_cast<double>(engine.historyLength))
                expectedDelay -= static_cast<double>(engine.historyLength);

            const auto denominator = juce::jmax(1, grain.lengthInSamples - 1);
            const auto phase = static_cast<float>(grain.ageInSamples)
                / static_cast<float>(denominator);
            const auto expectedEnvelope = GranulatorEngine::grainEnvelope(
                phase, grain.shapeAtBirth);
            const auto& visual = frame.grains[static_cast<size_t>(frameIndex++)];
            if (visual.eventId != grain.eventId
                || std::abs(visual.sourceDelaySamples - expectedDelay) > 1.0e-9
                || std::memcmp(&visual.playbackSpeed,
                               &grain.playbackSpeed,
                               sizeof(double)) != 0
                || std::memcmp(&visual.envelope,
                               &expectedEnvelope,
                               sizeof(float)) != 0
                || std::memcmp(&visual.pan, &grain.pan, sizeof(float)) != 0
                || visual.ageInSamples != grain.ageInSamples
                || visual.lengthInSamples != grain.lengthInSamples)
            {
                return false;
            }
        }

        return frameIndex == frame.grainCount;
    }
};
} // namespace tide

namespace
{
bool approximatelyEqual(const float left, const float right)
{
    return std::abs(left - right) < 1.0e-6f;
}

class TestPlayHead final : public juce::AudioPlayHead
{
public:
    void setPosition(const std::int64_t sample, const bool playing) noexcept
    {
        samplePosition = sample;
        isPlaying = playing;
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo position;
        position.setTimeInSamples(samplePosition);
        position.setIsPlaying(isPlaying);
        return position;
    }

private:
    std::int64_t samplePosition = 0;
    bool isPlaying = false;
};

bool drySignalPassesThrough()
{
    tide::GranulatorEngine engine;
    engine.prepare(48000.0, 64, 2, 0.0f);

    juce::AudioBuffer<float> buffer(2, 64);
    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(channel, sample, 0.1f * static_cast<float>(channel + 1));
    }

    const tide::GranulatorEngine::Controls controls { 0.02f, 20.0f, 0.0f };
    engine.process(buffer, controls);

    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto expected = 0.1f * static_cast<float>(channel + 1);
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            if (!approximatelyEqual(buffer.getSample(channel, sample), expected))
                return false;
        }
    }

    return true;
}

bool largestSizeLastsOneSecond()
{
    constexpr auto sampleRate = 48000.0;
    tide::GranulatorEngine engine;
    engine.prepare(sampleRate, 64, 2, 1.0f);

    tide::GranulatorEngine::Controls controls;
    controls.grainSizeMilliseconds = 1000.0f;
    if (tide::GranulatorEngineTestAccess::requestedLengthInSamples(engine, controls)
        != static_cast<int>(sampleRate))
    {
        return false;
    }

    TideGrainsAudioProcessor processor;
    const auto sizeRange = processor.parameters.getParameterRange(tide::parameter::size);
    return approximatelyEqual(sizeRange.start, 5.0f)
        && approximatelyEqual(sizeRange.end, 1000.0f);
}

bool delayedGrainBecomesAudibleAndStaysBounded()
{
    tide::GranulatorEngine engine;
    engine.prepare(48000.0, 64, 2, 1.0f);

    juce::AudioBuffer<float> buffer(2, 64);
    const tide::GranulatorEngine::Controls controls { 0.02f, 20.0f, 1.0f };
    auto heardWetSignal = false;

    for (auto block = 0; block < 40; ++block)
    {
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 1.0f);
        }

        engine.process(buffer, controls);

        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = buffer.getSample(channel, sample);
                // The engine exposes the uncompromised cloud; the processor's
                // final safety limiter owns the external -1 dBFS ceiling.
                if (!std::isfinite(value) || std::abs(value) > 64.0f)
                    return false;
                heardWetSignal = heardWetSignal || std::abs(value) > 0.01f;
            }
        }
    }

    return heardWetSignal;
}

bool cachedPanAmountTracksBirthPanExactly()
{
    tide::GranulatorEngine engine;
    engine.prepare(8000.0, 64, 2, 1.0f);

    juce::AudioBuffer<float> buffer(2, 64);
    tide::GranulatorEngine::Controls controls {
        0.5f, 40.0f, 1.0f, 64.0f, 0.5f, 1.0f, 0.0f, 0.5f, 1.0f
    };
    auto checkedReadableVoices = 0;
    auto sawNonCentredPan = false;

    for (auto block = 0; block < 160; ++block)
    {
        controls.grainSizeMilliseconds = block < 55 ? 5.0f
            : (block < 110 ? 40.0f : 250.0f);
        controls.spread = static_cast<float>(block % 5) * 0.25f;
        controls.drift = static_cast<float>((block * 3) % 5) * 0.25f;

        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto phase = static_cast<float>(block * buffer.getNumSamples() + sample);
            buffer.setSample(0, sample, 0.2f * std::sin(phase * 0.017f));
            buffer.setSample(1, sample, 0.17f * std::cos(phase * 0.023f));
        }

        engine.process(buffer, controls);
        for (const auto& state : tide::GranulatorEngineTestAccess::voiceStates(engine))
        {
            const auto expected = std::sin(juce::MathConstants<float>::halfPi
                                           * std::abs(state.pan));
            if (std::memcmp(&state.panAmount, &expected, sizeof(float)) != 0)
                return false;

            if (state.readable)
            {
                ++checkedReadableVoices;
                sawNonCentredPan = sawNonCentredPan || std::abs(state.pan) > 0.01f;
            }
        }
    }

    return checkedReadableVoices > 1000 && sawNonCentredPan;
}

bool densityRangeAndEngineCeilingAreOneToSixtyFour()
{
    TideGrainsAudioProcessor processor;
    const auto range = processor.parameters.getParameterRange(tide::parameter::density);
    if (range.start != 1.0f
        || range.end != 64.0f
        || range.interval != 1.0f
        || std::abs(range.convertFrom0to1(0.5f) - 16.0f) > 1.0e-4f)
    {
        return false;
    }

    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 1, 1, 1.0f);
    const tide::GranulatorEngine::Controls controls {
        0.5f, 40.0f, 1.0f, 640.0f, 0.5f, 0.0f
    };
    juce::AudioBuffer<float> buffer(1, 1);
    auto maximumActive = 0;
    for (auto sample = 0; sample < 2000; ++sample)
    {
        buffer.clear();
        engine.process(buffer, controls);
        maximumActive = juce::jmax(maximumActive,
                                   tide::GranulatorEngineTestAccess::activeCount(engine));
    }
    const auto maximumPasses = tide::GranulatorEngineTestAccess::simulatedPopulation(engine) == 64
        && tide::GranulatorEngineTestAccess::activeCount(engine) >= 60
        && maximumActive <= 64;
    if (! maximumPasses)
        return false;

    tide::GranulatorEngine single;
    single.prepare(1000.0, 1, 1, 1.0f);
    auto singleControls = controls;
    singleControls.density = -100.0f;
    for (auto sample = 0; sample < 300; ++sample)
    {
        buffer.clear();
        single.process(buffer, singleControls);
        if (tide::GranulatorEngineTestAccess::activeCount(single) > 1)
            return false;
    }
    return tide::GranulatorEngineTestAccess::simulatedPopulation(single) == 1
        && tide::GranulatorEngineTestAccess::activeCount(single) == 1;
}

struct ObservedBirth
{
    std::int64_t sample = -1;
    std::uint64_t eventId = 0;
    int length = 0;
    double sourceDelay = 0.0;
    double speed = 1.0;
    float pan = 0.0f;
};

bool grainsAreIndependentOneShotEventsWithTargetOverlap()
{
    constexpr auto sampleRate = 1000.0;
    constexpr auto target = 12;
    tide::GranulatorEngine engine;
    engine.prepare(sampleRate, 1, 1, 1.0f, 0.0f, 0.5f, 1.0f);
    engine.setRandomSeed(0xa51c0deULL);
    tide::GranulatorEngine::Controls controls {
        0.4f, 100.0f, 1.0f, static_cast<float>(target), 0.6f, 0.0f, 0.0f, 0.5f, 1.0f
    };
    juce::AudioBuffer<float> sampleBuffer(1, 1);
    std::array<std::uint64_t, 64> previousEvent {};
    std::array<int, 64> previousAge {};
    std::array<int, 64> previousLength {};
    std::array<std::int64_t, 64> previousBirth {};
    previousBirth.fill(-1);
    std::vector<std::int64_t> birthTimes;
    auto completedEvents = 0;
    auto populationSum = 0;
    auto populationSamples = 0;
    auto maximumPopulation = 0;

    for (auto sample = 0; sample < 3000; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.3f);
        engine.process(sampleBuffer, controls);
        const auto states = tide::GranulatorEngineTestAccess::voiceStates(engine);
        maximumPopulation = juce::jmax(maximumPopulation, static_cast<int>(states.size()));
        if (sample >= 600)
        {
            populationSum += static_cast<int>(states.size());
            ++populationSamples;
        }

        for (const auto& state : states)
        {
            const auto slot = static_cast<size_t>(state.slot);
            if (state.length < 80 || state.length > 120
                || state.age < 0 || state.age >= state.length)
            {
                return false;
            }

            if (previousEvent[slot] == state.eventId && state.eventId != 0)
            {
                if (state.age != previousAge[slot] + 1
                    || state.length != previousLength[slot]
                    || state.birthSample != previousBirth[slot])
                {
                    return false;
                }
            }
            else if (state.eventId != 0)
            {
                if (previousEvent[slot] != 0)
                {
                    if (state.eventId <= previousEvent[slot]
                        || state.birthSample <= previousBirth[slot]
                        || previousAge[slot] < previousLength[slot] - 2)
                    {
                        return false;
                    }
                    ++completedEvents;
                }
                if (state.birthSample >= 600)
                    birthTimes.push_back(state.birthSample);
            }

            previousEvent[slot] = state.eventId;
            previousAge[slot] = state.age;
            previousLength[slot] = state.length;
            previousBirth[slot] = state.birthSample;
        }
    }

    std::sort(birthTimes.begin(), birthTimes.end());
    std::vector<std::int64_t> intervals;
    for (size_t index = 1; index < birthTimes.size(); ++index)
        if (birthTimes[index] > birthTimes[index - 1])
            intervals.push_back(birthTimes[index] - birthTimes[index - 1]);
    std::sort(intervals.begin(), intervals.end());
    const auto distinctIntervals = static_cast<int>(std::unique(intervals.begin(),
                                                                 intervals.end())
                                                     - intervals.begin());
    const auto meanPopulation = populationSamples > 0
        ? static_cast<double>(populationSum) / static_cast<double>(populationSamples)
        : 0.0;
    const auto passes = tide::GranulatorEngineTestAccess::simulatedPopulation(engine) == target
        && maximumPopulation <= target
        && meanPopulation >= target * 0.85
        && completedEvents >= target * 8
        && birthTimes.size() >= static_cast<size_t>(target * 12)
        && distinctIntervals >= 3;
    if (! passes)
        std::cerr << "One-shot events: mean population " << meanPopulation
                  << ", completed " << completedEvents
                  << ", births " << birthTimes.size()
                  << ", distinct intervals " << distinctIntervals << '\n';
    return passes;
}

bool soundingGrainsKeepTheirBirthSettingsDuringAutomation()
{
    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 1, 1, 1.0f, 0.0f, 0.0f, 0.0f);
    engine.setRandomSeed(0xb17a5eedULL);
    tide::GranulatorEngine::Controls controls {
        0.4f, 160.0f, 1.0f, 8.0f, 0.2f, 0.15f, 0.0f, 0.0f, 0.0f
    };
    juce::AudioBuffer<float> sampleBuffer(1, 1);
    tide::GranulatorEngineTestAccess::VoiceState retained;
    auto foundRetained = false;
    for (auto sample = 0; sample < 1200 && ! foundRetained; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.25f);
        engine.process(sampleBuffer, controls);
        for (const auto& state : tide::GranulatorEngineTestAccess::voiceStates(engine))
        {
            if (state.readable && state.age < state.length / 3)
            {
                retained = state;
                foundRetained = true;
                break;
            }
        }
    }
    if (! foundRetained)
        return false;

    controls.timeSeconds = 0.2f;
    controls.grainSizeMilliseconds = 260.0f;
    controls.shape = 0.9f;
    controls.spread = 1.0f;
    controls.tide = 1.0f;
    controls.drift = 1.0f;
    auto sawRetainedAdvance = false;
    auto sawNewSettings = false;
    for (auto sample = 0; sample < 700; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.25f);
        engine.process(sampleBuffer, controls);
        for (const auto& state : tide::GranulatorEngineTestAccess::voiceStates(engine))
        {
            if (state.slot == retained.slot && state.eventId == retained.eventId)
            {
                sawRetainedAdvance = true;
                if (state.age <= retained.age
                    || state.length != retained.length
                    || state.birthSample != retained.birthSample
                    || std::memcmp(&state.shapeAtBirth,
                                   &retained.shapeAtBirth,
                                   sizeof(state.shapeAtBirth)) != 0
                    || std::memcmp(&state.sourceDelaySamples,
                                   &retained.sourceDelaySamples,
                                   sizeof(state.sourceDelaySamples)) != 0
                    || std::memcmp(&state.playbackSpeed,
                                   &retained.playbackSpeed,
                                   sizeof(state.playbackSpeed)) != 0
                    || std::memcmp(&state.pan, &retained.pan, sizeof(state.pan)) != 0)
                {
                    return false;
                }
            }
            if (state.eventId > retained.eventId
                && state.length >= 208 && state.length <= 312
                && std::abs(state.shapeAtBirth - 0.9f) < 1.0e-6f)
            {
                sawNewSettings = true;
            }
        }
    }
    return sawRetainedAdvance && sawNewSettings;
}

bool densityChangesPreserveTailsAndFillWithoutABurst()
{
    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 1, 1, 1.0f, 0.0f, 0.4f, 0.6f);
    engine.setRandomSeed(0xd3517eULL);
    tide::GranulatorEngine::Controls controls {
        0.4f, 120.0f, 1.0f, 32.0f, 0.5f, 0.0f, 0.0f, 0.4f, 0.6f
    };
    juce::AudioBuffer<float> sampleBuffer(1, 1);
    for (auto sample = 0; sample < 1400; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.2f);
        engine.process(sampleBuffer, controls);
    }

    const auto beforeDrop = tide::GranulatorEngineTestAccess::voiceStates(engine);
    if (beforeDrop.size() < 26)
    {
        std::cerr << "Density drop setup active: " << beforeDrop.size() << '\n';
        return false;
    }
    const auto birthsAtDrop = tide::GranulatorEngineTestAccess::birthsStarted(engine);
    controls.density = 4.0f;
    auto lastActive = static_cast<int>(beforeDrop.size());
    for (auto sample = 0; sample < 400; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.2f);
        engine.process(sampleBuffer, controls);
        const auto active = tide::GranulatorEngineTestAccess::activeCount(engine);
        if ((active > lastActive && lastActive >= 4) || active > 64)
        {
            std::cerr << "Density drop active rose " << lastActive << " -> " << active << '\n';
            return false;
        }
        if (active > 4
            && tide::GranulatorEngineTestAccess::birthsStarted(engine) != birthsAtDrop)
        {
            std::cerr << "Density drop started a birth while active=" << active << '\n';
            return false;
        }
        lastActive = active;
    }
    if (lastActive != 4)
    {
        std::cerr << "Density drop settled at " << lastActive << '\n';
        return false;
    }

    controls.density = 16.0f;
    std::vector<std::int64_t> newBirthTimes;
    auto previousBirths = tide::GranulatorEngineTestAccess::birthsStarted(engine);
    for (auto sample = 0; sample < 500; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.2f);
        engine.process(sampleBuffer, controls);
        const auto births = tide::GranulatorEngineTestAccess::birthsStarted(engine);
        if (births > previousBirths)
            newBirthTimes.push_back(tide::GranulatorEngineTestAccess::timeline(engine));
        previousBirths = births;
    }
    std::sort(newBirthTimes.begin(), newBirthTimes.end());
    const auto distinctBirthSamples = std::unique(newBirthTimes.begin(), newBirthTimes.end())
        - newBirthTimes.begin();
    const auto finalActive = tide::GranulatorEngineTestAccess::activeCount(engine);
    const auto passes = finalActive >= 14
        && tide::GranulatorEngineTestAccess::activeCount(engine) <= 16
        && distinctBirthSamples >= 6;
    if (! passes)
        std::cerr << "Density rise active " << finalActive
                  << ", distinct birth samples " << distinctBirthSamples << '\n';
    return passes;
}

bool densityAutomationDoesNotClickTheWetField()
{
    tide::GranulatorEngine engine;
    engine.prepare(48000.0, 64, 1, 1.0f);
    engine.setRandomSeed(0x1234ULL);

    tide::GranulatorEngine::Controls controls { 0.5f, 40.0f, 1.0f, 64.0f, 0.5f, 0.0f };
    juce::AudioBuffer<float> buffer(1, 64);
    auto previous = 0.0f;
    std::int64_t sourceSample = 0;

    const auto run = [&](const int blocks, float* jumpMaximum)
    {
        for (auto block = 0; block < blocks; ++block)
        {
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(0,
                                 sample,
                                 0.5f * std::sin(static_cast<float>(sourceSample++) * 0.05f));
            engine.process(buffer, controls);
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = buffer.getSample(0, sample);
                if (jumpMaximum != nullptr)
                    *jumpMaximum = juce::jmax(*jumpMaximum, std::abs(value - previous));
                previous = value;
            }
        }
    };

    auto steadyMaximum = 0.0f;
    auto dropMaximum = 0.0f;
    auto riseMaximum = 0.0f;

    run(800, nullptr);
    run(200, &steadyMaximum);
    controls.density = 4.0f;
    run(60, &dropMaximum);
    run(60, nullptr);
    controls.density = 64.0f;
    run(60, &riseMaximum);

    // Keep population automation within a small absolute jump and twice the
    // asynchronous field's own steepest steady-state step.
    const auto allowed = juce::jmax(0.02f, steadyMaximum * 2.0f);
    const auto passes = dropMaximum <= allowed && riseMaximum <= allowed;
    if (! passes)
        std::cerr << "Steady max jump: " << steadyMaximum
                  << ", drop max jump: " << dropMaximum
                  << ", rise max jump: " << riseMaximum << '\n';
    return passes;
}

std::vector<ObservedBirth> observeBirths(const float tideAmount,
                                         const float driftAmount,
                                         const std::uint64_t seed)
{
    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 1, 2, 1.0f, 0.0f, tideAmount, driftAmount);
    engine.setRandomSeed(seed);
    tide::GranulatorEngine::Controls controls {
        0.5f, 120.0f, 1.0f, 16.0f, 0.65f, 0.8f, 0.0f, tideAmount, driftAmount
    };
    juce::AudioBuffer<float> sampleBuffer(2, 1);
    std::array<std::uint64_t, 64> previousEvent {};
    std::vector<ObservedBirth> result;
    for (auto sample = 0; sample < 5000; ++sample)
    {
        sampleBuffer.setSample(0, 0, 0.3f);
        sampleBuffer.setSample(1, 0, -0.2f);
        engine.process(sampleBuffer, controls);
        for (const auto& state : tide::GranulatorEngineTestAccess::voiceStates(engine))
        {
            const auto slot = static_cast<size_t>(state.slot);
            if (state.eventId == previousEvent[slot])
                continue;
            previousEvent[slot] = state.eventId;
            if (state.readable && state.birthSample >= 800)
            {
                result.push_back({ state.birthSample,
                                   state.eventId,
                                   state.length,
                                   state.sourceDelaySamples,
                                   state.playbackSpeed,
                                   state.pan });
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right)
    {
        return left.eventId < right.eventId;
    });
    return result;
}

double medianCohortSpan(const std::vector<ObservedBirth>& births)
{
    constexpr auto cohortSize = size_t { 16 };
    std::vector<double> spans;
    for (size_t start = 0; start + cohortSize <= births.size(); start += cohortSize)
    {
        auto minimum = births[start].sourceDelay;
        auto maximum = minimum;
        for (auto index = start + 1; index < start + cohortSize; ++index)
        {
            minimum = std::min(minimum, births[index].sourceDelay);
            maximum = std::max(maximum, births[index].sourceDelay);
        }
        spans.push_back(maximum - minimum);
    }
    if (spans.empty())
        return 0.0;
    std::sort(spans.begin(), spans.end());
    return spans[spans.size() / 2];
}

bool tideWidensSourceTrailWithoutChangingBirthTiming()
{
    const auto low = observeBirths(0.0f, 0.0f, 0x71de5eedULL);
    const auto high = observeBirths(1.0f, 0.0f, 0x71de5eedULL);
    if (low.size() != high.size() || low.size() < 100)
        return false;

    for (size_t index = 0; index < low.size(); ++index)
    {
        if (low[index].eventId != high[index].eventId
            || low[index].sample != high[index].sample
            || low[index].length != high[index].length)
        {
            return false;
        }
    }

    const auto lowSpan = medianCohortSpan(low);
    const auto highSpan = medianCohortSpan(high);
    std::vector<double> cohortCentroids;
    for (size_t start = 0; start + 16 <= high.size(); start += 16)
    {
        auto sum = 0.0;
        for (auto index = start; index < start + 16; ++index)
            sum += high[index].sourceDelay;
        cohortCentroids.push_back(sum / 16.0);
    }
    const auto [minimumCentroid, maximumCentroid] = std::minmax_element(
        cohortCentroids.begin(), cohortCentroids.end());
    const auto centroidTravel = cohortCentroids.empty()
        ? 0.0
        : *maximumCentroid - *minimumCentroid;
    const auto passes = lowSpan <= 35.0
        && highSpan >= lowSpan * 2.0
        && highSpan >= 35.0
        && highSpan <= 500.0
        && centroidTravel >= 8.0;
    if (! passes)
        std::cerr << "Tide birth trail: low span " << lowSpan
                  << ", high span " << highSpan
                  << ", centroid travel " << centroidTravel << '\n';
    return passes;
}

bool driftAddsBoundedBirthVariation()
{
    const auto still = observeBirths(0.55f, 0.0f, 0xd21f7ULL);
    const auto organic = observeBirths(0.55f, 1.0f, 0xd21f7ULL);
    if (still.size() < 100 || organic.size() < 100)
        return false;

    const auto stillNominal = std::all_of(still.begin(), still.end(), [](const auto& birth)
    {
        return birth.length == 120
            && std::abs(birth.speed - 1.0) < 1.0e-12;
    });
    auto variedLength = false;
    auto variedSpeed = false;
    auto variedPan = false;
    for (const auto& birth : organic)
    {
        if (birth.length < 96 || birth.length > 144
            || birth.speed < 0.9899 || birth.speed > 1.0101
            || std::abs(birth.pan) > 0.8001f
            || birth.sourceDelay < 0.0 || birth.sourceDelay > 500.0)
        {
            return false;
        }
        variedLength = variedLength || birth.length != 120;
        variedSpeed = variedSpeed || std::abs(birth.speed - 1.0) > 1.0e-5;
        variedPan = variedPan || std::abs(birth.pan) > 1.0e-3f;
    }

    auto timingDiffers = still.size() != organic.size();
    for (size_t index = 0; ! timingDiffers && index < still.size(); ++index)
        timingDiffers = still[index].sample != organic[index].sample;
    return stillNominal && variedLength && variedSpeed && variedPan && timingDiffers;
}

float percentile(std::vector<float> values, const float proportion)
{
    if (values.empty())
        return 0.0f;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(std::round(
        juce::jlimit(0.0f, 1.0f, proportion) * static_cast<float>(values.size() - 1)));
    return values[index];
}

float renderCollectiveModulation(const float tideAmount, const float driftAmount)
{
    constexpr auto sampleRate = 4000.0;
    constexpr auto nominalSizeSamples = 800;
    tide::GranulatorEngine engine;
    engine.prepare(sampleRate, 32, 1, 1.0f, 0.0f, tideAmount, driftAmount);
    tide::GranulatorEngine::Controls controls;
    controls.timeSeconds = 0.5f;
    controls.grainSizeMilliseconds = 200.0f;
    controls.mix = 1.0f;
    controls.density = 48.0f;
    controls.shape = 1.0f;
    controls.spread = 0.0f;
    controls.feedback = 0.0f;
    controls.tide = tideAmount;
    controls.drift = driftAmount;

    juce::AudioBuffer<float> buffer(1, 20);
    std::vector<float> windowLevels;
    for (auto window = 0; window < 20 * nominalSizeSamples / buffer.getNumSamples(); ++window)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(0, sample, 0.35f);
        engine.process(buffer, controls);
        if (window * buffer.getNumSamples() >= nominalSizeSamples * 5)
            windowLevels.push_back(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
    }

    const auto middle = percentile(windowLevels, 0.5f);
    return middle > 1.0e-6f
        ? (percentile(windowLevels, 0.95f) - percentile(windowLevels, 0.05f)) / middle
        : 0.0f;
}

bool collectiveWaveRemainsAudibleAfterNormalisation()
{
    const auto tightModulation = renderCollectiveModulation(0.0f, 0.0f);
    const auto broadModulation = renderCollectiveModulation(1.0f, 0.0f);
    const auto organicModulation = renderCollectiveModulation(0.65f, 1.0f);
    // Tide must be heard as motion through source position, not as every grain
    // sharing a Size-rate amplitude crest. Both extremes therefore keep a
    // comparatively continuous envelope on a constant source.
    const auto passes = tightModulation < 0.55f
        && broadModulation < 0.55f
        && organicModulation < 0.55f
        && std::abs(tightModulation - broadModulation) < 0.25f;
    if (! passes)
        std::cerr << "Asynchronous texture modulation: low Tide " << tightModulation
                  << ", high Tide " << broadModulation
                  << ", maximum Drift " << organicModulation << '\n';
    else
        std::cout << "Asynchronous texture simulation: low-Tide modulation "
                  << tightModulation << ", high-Tide modulation "
                  << broadModulation << ", maximum-Drift modulation "
                  << organicModulation << '\n';
    return passes;
}

float renderProcessorCollectiveModulation(const float tideAmount)
{
    constexpr auto sampleRate = 4000.0;
    constexpr auto nominalSizeSamples = 800;
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        if (parameter == nullptr)
            return false;
        const auto normalised = processor->parameters.getParameterRange(id).convertTo0to1(value);
        parameter->setValueNotifyingHost(normalised);
        return true;
    };

    if (!setParameter(tide::parameter::time, 0.5f)
        || !setParameter(tide::parameter::size, 200.0f)
        || !setParameter(tide::parameter::density, 48.0f)
        || !setParameter(tide::parameter::shape, 1.0f)
        || !setParameter(tide::parameter::spread, 0.0f)
        || !setParameter(tide::parameter::feedback, 0.0f)
        || !setParameter(tide::parameter::mix, 1.0f)
        || !setParameter(tide::parameter::tide, tideAmount)
        || !setParameter(tide::parameter::drift, 0.0f))
    {
        return 0.0f;
    }

    processor->prepareToPlay(sampleRate, 20);
    juce::AudioBuffer<float> buffer(2, 20);
    juce::MidiBuffer midi;
    std::vector<float> windowLevels;
    for (auto window = 0; window < 20 * nominalSizeSamples / buffer.getNumSamples(); ++window)
    {
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 0.1f);
        processor->processBlock(buffer, midi);
        if (window * buffer.getNumSamples() >= nominalSizeSamples * 5)
            windowLevels.push_back(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
    }

    const auto middle = percentile(windowLevels, 0.5f);
    return middle > 1.0e-6f
        ? (percentile(windowLevels, 0.95f) - percentile(windowLevels, 0.05f)) / middle
        : 0.0f;
}

std::vector<float> renderProcessorTideSignature(const float tideAmount)
{
    constexpr auto sampleRate = 4000.0;
    constexpr auto blockSize = 40;
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        if (parameter == nullptr)
            return false;
        parameter->setValueNotifyingHost(
            processor->parameters.getParameterRange(id).convertTo0to1(value));
        return true;
    };
    if (! setParameter(tide::parameter::time, 0.5f)
        || ! setParameter(tide::parameter::size, 120.0f)
        || ! setParameter(tide::parameter::density, 24.0f)
        || ! setParameter(tide::parameter::shape, 0.7f)
        || ! setParameter(tide::parameter::spread, 0.0f)
        || ! setParameter(tide::parameter::feedback, 0.0f)
        || ! setParameter(tide::parameter::mix, 1.0f)
        || ! setParameter(tide::parameter::tide, tideAmount)
        || ! setParameter(tide::parameter::drift, 0.0f))
    {
        return {};
    }

    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> result;
    for (auto block = 0; block < 260; ++block)
    {
        for (auto sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = block * blockSize + sample;
            const auto phase = static_cast<float>(absoluteSample);
            buffer.setSample(0, sample,
                             0.31f * std::sin(phase * 0.037f)
                                 + 0.17f * std::sin(phase * 0.149f + 0.4f));
            buffer.setSample(1, sample,
                             0.23f * std::cos(phase * 0.061f + 0.3f));
        }
        processor->processBlock(buffer, midi);
        if (block >= 180)
            for (auto sample = 0; sample < blockSize; ++sample)
                result.push_back(buffer.getSample(0, sample));
    }
    return result;
}

bool processorForwardsTideIntoTheAudioEngine()
{
    const auto tightModulation = renderProcessorCollectiveModulation(0.0f);
    const auto broadModulation = renderProcessorCollectiveModulation(1.0f);
    const auto tightSignal = renderProcessorTideSignature(0.0f);
    const auto broadSignal = renderProcessorTideSignature(1.0f);
    auto maximumDifference = 0.0f;
    if (tightSignal.size() != broadSignal.size() || tightSignal.empty())
        return false;
    for (size_t index = 0; index < tightSignal.size(); ++index)
        maximumDifference = juce::jmax(maximumDifference,
                                       std::abs(tightSignal[index] - broadSignal[index]));
    const auto passes = tightModulation < 0.65f
        && broadModulation < 0.65f
        && std::abs(tightModulation - broadModulation) < 0.3f
        && maximumDifference > 1.0e-4f;
    if (! passes)
        std::cerr << "Processor Tide modulation: tight " << tightModulation
                  << ", broad " << broadModulation << '\n';
    else
        std::cout << "Processor integration: Tide avoids a shared amplitude pulse ("
                  << tightModulation << ", " << broadModulation
                  << ") while changing source motion by " << maximumDifference << '\n';
    return passes;
}

std::vector<float> renderProcessorDriftSignature(const float driftAmount)
{
    constexpr auto sampleRate = 8000.0;
    constexpr auto blockSize = 64;
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        if (parameter == nullptr)
            return false;
        parameter->setValueNotifyingHost(
            processor->parameters.getParameterRange(id).convertTo0to1(value));
        return true;
    };
    if (!setParameter(tide::parameter::time, 0.4f)
        || !setParameter(tide::parameter::size, 75.0f)
        || !setParameter(tide::parameter::density, 37.0f)
        || !setParameter(tide::parameter::shape, 0.65f)
        || !setParameter(tide::parameter::spread, 0.8f)
        || !setParameter(tide::parameter::feedback, 0.0f)
        || !setParameter(tide::parameter::mix, 1.0f)
        || !setParameter(tide::parameter::tide, 0.75f)
        || !setParameter(tide::parameter::drift, driftAmount))
    {
        return {};
    }

    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    for (auto block = 0; block < 300; ++block)
    {
        for (auto sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = block * blockSize + sample;
            const auto phase = static_cast<float>(absoluteSample);
            buffer.setSample(0, sample, 0.25f * std::sin(phase * 0.071f));
            buffer.setSample(1, sample, 0.18f * std::cos(phase * 0.113f));
        }
        processor->processBlock(buffer, midi);
        if (block >= 180)
            for (auto sample = 0; sample < blockSize; ++sample)
                output.push_back(buffer.getSample(0, sample));
    }
    return output;
}

bool processorForwardsDriftIntoTheAudioEngine()
{
    const auto still = renderProcessorDriftSignature(0.0f);
    const auto organic = renderProcessorDriftSignature(1.0f);
    if (still.size() != organic.size() || still.empty())
        return false;

    auto maximumDifference = 0.0f;
    for (size_t index = 0; index < still.size(); ++index)
    {
        if (! std::isfinite(still[index]) || ! std::isfinite(organic[index]))
            return false;
        maximumDifference = juce::jmax(maximumDifference,
                                       std::abs(still[index] - organic[index]));
    }
    if (maximumDifference > 1.0e-3f)
        std::cout << "Processor integration: Drift changes rendered audio by up to "
                  << maximumDifference << '\n';
    return maximumDifference > 1.0e-3f;
}

std::vector<float> renderRetainedTone(const float sourceFrequency,
                                      const float tideAmount,
                                      const float driftAmount)
{
    constexpr auto sampleRate = 2000.0;
    constexpr auto blockSize = 40;
    constexpr auto totalSeconds = 4;
    tide::GranulatorEngine engine;
    engine.prepare(sampleRate, blockSize, 1, 1.0f, 0.0f, tideAmount, driftAmount);
    tide::GranulatorEngine::Controls controls;
    controls.timeSeconds = 0.3f;
    controls.grainSizeMilliseconds = 80.0f;
    controls.mix = 1.0f;
    controls.density = 32.0f;
    controls.shape = 1.0f;
    controls.spread = 0.0f;
    controls.feedback = 0.0f;
    controls.tide = tideAmount;
    controls.drift = driftAmount;

    juce::AudioBuffer<float> buffer(1, blockSize);
    std::vector<float> result;
    result.reserve(static_cast<size_t>(sampleRate));
    for (auto block = 0; block < totalSeconds * static_cast<int>(sampleRate) / blockSize; ++block)
    {
        for (auto sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = block * blockSize + sample;
            buffer.setSample(0,
                             sample,
                             0.35f * std::sin(juce::MathConstants<float>::twoPi
                                              * sourceFrequency
                                              * static_cast<float>(absoluteSample)
                                              / static_cast<float>(sampleRate)));
        }
        engine.process(buffer, controls);
        if (block * blockSize >= (totalSeconds - 1) * static_cast<int>(sampleRate))
            for (auto sample = 0; sample < blockSize; ++sample)
                result.push_back(buffer.getSample(0, sample));
    }
    return result;
}

float spectralBandMagnitude(const std::vector<float>& signal,
                            const float centreFrequency)
{
    constexpr auto sampleRate = 2000.0f;
    auto maximum = 0.0f;
    for (auto step = -6; step <= 6; ++step)
    {
        const auto frequency = centreFrequency * (1.0f + static_cast<float>(step) * 0.002f);
        double real = 0.0;
        double imaginary = 0.0;
        for (size_t sample = 0; sample < signal.size(); ++sample)
        {
            const auto angle = juce::MathConstants<double>::twoPi
                * static_cast<double>(frequency)
                * static_cast<double>(sample)
                / static_cast<double>(sampleRate);
            real += static_cast<double>(signal[sample]) * std::cos(angle);
            imaginary -= static_cast<double>(signal[sample]) * std::sin(angle);
        }
        maximum = juce::jmax(maximum,
                             static_cast<float>(std::sqrt(real * real + imaginary * imaginary)));
    }
    return maximum;
}

bool sourceTimbreSurvivesBothTideExtremesAndMaximumDrift()
{
    const auto retainedRatio = [](const float sourceFrequency,
                                  const float unrelatedFrequency,
                                  const float tideAmount,
                                  const float driftAmount)
    {
        const auto signal = renderRetainedTone(sourceFrequency, tideAmount, driftAmount);
        const auto retained = spectralBandMagnitude(signal, sourceFrequency);
        const auto unrelated = spectralBandMagnitude(signal, unrelatedFrequency);
        return signal.empty() ? 0.0f : retained / juce::jmax(1.0e-9f, unrelated);
    };

    const std::array ratios {
        retainedRatio(37.0f, 137.0f, 0.0f, 0.0f),
        retainedRatio(37.0f, 137.0f, 1.0f, 1.0f),
        retainedRatio(137.0f, 37.0f, 0.0f, 0.0f),
        retainedRatio(137.0f, 37.0f, 1.0f, 1.0f)
    };
    const auto minimumRatio = *std::min_element(ratios.begin(), ratios.end());
    if (minimumRatio > 5.0f)
        std::cout << "Timbre-retention simulation: minimum source/unrelated spectral ratio "
                  << minimumRatio << "x across both Tide/Drift extremes\n";
    return minimumRatio > 5.0f;
}

bool denseProcessorRenderKeepsRealtimeHeadroom()
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto blockSize = 256;
    constexpr auto measuredSeconds = 2.0;
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        if (parameter == nullptr)
            return false;
        parameter->setValueNotifyingHost(
            processor->parameters.getParameterRange(id).convertTo0to1(value));
        return true;
    };
    if (!setParameter(tide::parameter::time, 1.0f)
        || !setParameter(tide::parameter::size, 40.0f)
        || !setParameter(tide::parameter::density, 64.0f)
        || !setParameter(tide::parameter::shape, 0.5f)
        || !setParameter(tide::parameter::spread, 1.0f)
        || !setParameter(tide::parameter::feedback, 0.5f)
        || !setParameter(tide::parameter::mix, 1.0f)
        || !setParameter(tide::parameter::tide, 0.0f)
        || !setParameter(tide::parameter::drift, 1.0f))
    {
        return false;
    }

    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    auto absoluteSample = std::int64_t { 0 };
    const auto renderBlock = [&]
    {
        for (auto sample = 0; sample < blockSize; ++sample, ++absoluteSample)
        {
            const auto phase = static_cast<float>(absoluteSample);
            buffer.setSample(0, sample, 0.2f * std::sin(phase * 0.017f));
            buffer.setSample(1, sample, 0.17f * std::cos(phase * 0.023f));
        }
        processor->processBlock(buffer, midi);
    };

    for (auto block = 0; block < 64; ++block)
        renderBlock();

    const auto measuredBlocks = static_cast<int>(std::ceil(
        measuredSeconds * sampleRate / static_cast<double>(blockSize)));
    const auto start = std::chrono::steady_clock::now();
    for (auto block = 0; block < measuredBlocks; ++block)
        renderBlock();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const auto renderedSeconds = static_cast<double>(measuredBlocks * blockSize) / sampleRate;
    const auto realtimeFactor = renderedSeconds / elapsed;
    std::cout << "Dense realtime simulation: 64-grain stereo processor renders at "
              << realtimeFactor << "x realtime\n";

    // Timing in Debug and sanitizer builds is diagnostic only. The optimised
    // Release suite owns the product requirement to render faster than realtime.
#if defined(NDEBUG)
    return realtimeFactor > 1.0;
#else
    return true;
#endif
}

float renderWetFieldRms(const float density)
{
    tide::GranulatorEngine engine;
    engine.prepare(8000.0, 64, 1, 1.0f);
    engine.setRandomSeed(0x1eafbeefULL);
    const tide::GranulatorEngine::Controls controls {
        0.5f, 60.0f, 1.0f, density, 0.5f, 0.0f
    };
    juce::AudioBuffer<float> buffer(1, 64);
    double energy = 0.0;
    std::int64_t measuredSamples = 0;

    for (auto block = 0; block < 250; ++block)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto absoluteSample = block * buffer.getNumSamples() + sample;
            const auto phase = static_cast<float>(absoluteSample);
            buffer.setSample(0,
                             sample,
                             0.42f * std::sin(phase * 0.071f)
                                 + 0.23f * std::sin(phase * 0.193f + 0.4f)
                                 + 0.11f * std::cos(phase * 0.337f));
        }

        engine.process(buffer, controls);
        if (block >= 100)
        {
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = buffer.getSample(0, sample);
                if (! std::isfinite(value) || std::abs(value) > 64.0f)
                    return -1.0f;
                energy += static_cast<double>(value) * static_cast<double>(value);
                ++measuredSamples;
            }
        }
    }

    return measuredSamples > 0
        ? static_cast<float>(std::sqrt(energy / static_cast<double>(measuredSamples)))
        : -1.0f;
}

bool increasingDensityDoesNotAttenuateTheWetField()
{
    const auto sparseRms = renderWetFieldRms(4.0f);
    const auto denseRms = renderWetFieldRms(64.0f);
    const auto passes = sparseRms > 0.0f
        && denseRms >= sparseRms * 0.55f
        && denseRms <= sparseRms * 1.8f;
    if (! passes)
        std::cerr << "Sparse RMS: " << sparseRms << ", dense RMS: " << denseRms << '\n';
    return passes;
}

bool transportResetCannotReadPreSeekAudio()
{
    TideGrainsAudioProcessor processor;
    TestPlayHead playHead;
    processor.setPlayHead(&playHead);

    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor.parameters.getParameter(id);
        if (parameter == nullptr)
            return false;

        const auto normalised = processor.parameters.getParameterRange(id).convertTo0to1(value);
        parameter->setValueNotifyingHost(normalised);
        return true;
    };

    if (!setParameter(tide::parameter::time, 0.2f)
        || !setParameter(tide::parameter::size, 20.0f)
        || !setParameter(tide::parameter::density, 24.0f)
        || !setParameter(tide::parameter::mix, 1.0f))
    {
        processor.setPlayHead(nullptr);
        return false;
    }

    processor.prepareToPlay(1000.0, 20);
    juce::AudioBuffer<float> buffer(2, 20);
    juce::MidiBuffer midi;

    for (auto block = 0; block < 15; ++block)
    {
        playHead.setPosition(block * buffer.getNumSamples(), true);
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 1.0f);

        processor.processBlock(buffer, midi);
    }

    for (auto block = 0; block < 15; ++block)
    {
        playHead.setPosition(5000 + block * buffer.getNumSamples(), true);
        buffer.clear();
        processor.processBlock(buffer, midi);

        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                if (std::abs(buffer.getSample(channel, sample)) > 1.0e-6f)
                {
                    processor.setPlayHead(nullptr);
                    return false;
                }
    }

    processor.setPlayHead(nullptr);
    return true;
}

bool stoppedTransportStillProducesWetOutput()
{
    TideGrainsAudioProcessor processor;
    TestPlayHead playHead;
    processor.setPlayHead(&playHead);

    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor.parameters.getParameter(id);
        if (parameter == nullptr)
            return false;

        const auto normalised = processor.parameters.getParameterRange(id).convertTo0to1(value);
        parameter->setValueNotifyingHost(normalised);
        return true;
    };

    if (!setParameter(tide::parameter::time, 0.2f)
        || !setParameter(tide::parameter::size, 20.0f)
        || !setParameter(tide::parameter::density, 24.0f)
        || !setParameter(tide::parameter::mix, 1.0f))
    {
        processor.setPlayHead(nullptr);
        return false;
    }

    // The block is shorter than the engine's safety margin, so wet output can
    // only appear if the history survives from one block to the next.
    processor.prepareToPlay(1000.0, 8);
    juce::AudioBuffer<float> buffer(2, 8);
    juce::MidiBuffer midi;
    auto heardWetSignal = false;

    // A stopped host keeps calling processBlock with a frozen playhead
    // position; the wet field must still build up and become audible.
    playHead.setPosition(1234, false);
    for (auto block = 0; block < 80; ++block)
    {
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 1.0f);

        processor.processBlock(buffer, midi);
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                heardWetSignal = heardWetSignal
                    || std::abs(buffer.getSample(channel, sample)) > 0.01f;
    }

    processor.setPlayHead(nullptr);
    return heardWetSignal;
}

bool waveformWindowFillsEveryColumnAndTracksTime()
{
    tide::WaveformHistory history;
    history.prepare(1000.0);

    juce::AudioBuffer<float> source(2, 200);
    for (auto sample = 0; sample < source.getNumSamples(); ++sample)
    {
        const auto value = sample < 100 ? 0.25f : -0.75f;
        source.setSample(0, sample, value);
        source.setSample(1, sample, value);
    }
    history.push(source, 2);

    std::vector<juce::Range<float>> shortWindow;
    history.readWindow(shortWindow, 40, 0.1f);
    if (shortWindow.size() != 40)
        return false;

    for (const auto range : shortWindow)
    {
        if (!approximatelyEqual(range.getStart(), -0.75f)
            || !approximatelyEqual(range.getEnd(), -0.75f))
        {
            return false;
        }
    }

    std::vector<juce::Range<float>> longWindow;
    history.readWindow(longWindow, 40, 0.2f);
    return longWindow.size() == 40
        && approximatelyEqual(longWindow.front().getStart(), 0.25f)
        && approximatelyEqual(longWindow.back().getEnd(), -0.75f);
}

bool waveformWindowPreservesItsAbsoluteHistoryCoordinates()
{
    tide::WaveformHistory history;
    history.prepare(1000.0);
    history.reset(17);

    std::array<float, 50> source {};
    source.fill(0.5f);
    history.pushMono(source.data(), static_cast<int>(source.size()));

    std::vector<juce::Range<float>> waveform;
    const auto info = history.readWindow(waveform, 10, 0.1f);
    if (info.generation != 17
        || info.newestSampleSequence != 50.0
        || info.visibleSampleCount != 100.0
        || waveform.size() != 10)
    {
        return false;
    }

    for (auto index = 0; index < 5; ++index)
        if (! waveform[static_cast<size_t>(index)].isEmpty())
            return false;

    for (auto index = 5; index < 10; ++index)
        if (! approximatelyEqual(waveform[static_cast<size_t>(index)].getStart(), 0.5f)
            || ! approximatelyEqual(waveform[static_cast<size_t>(index)].getEnd(), 0.5f))
            return false;

    return true;
}

bool publishedGrainTelemetryMatchesTheAudioEngineExactly()
{
    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 64, 2, 1.0f, 0.6f, 0.7f, 0.8f);
    tide::GranulatorEngine::Controls controls {
        0.5f, 80.0f, 1.0f, 16.0f, 0.7f, 0.9f, 0.6f, 0.7f, 0.8f
    };
    juce::AudioBuffer<float> buffer(2, 64);

    for (auto block = 0; block < 12; ++block)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto phase = static_cast<float>(block * buffer.getNumSamples() + sample);
            buffer.setSample(0, sample, 0.3f * std::sin(phase * 0.031f));
            buffer.setSample(1, sample, 0.2f * std::cos(phase * 0.027f));
        }
        engine.process(buffer, controls);
    }

    return tide::GranulatorEngineTestAccess::activeCount(engine) > 0
        && tide::GranulatorEngineTestAccess::visualFrameMatchesEngine(engine);
}

bool grainSnapshotExchangePublishesOnlyCoherentLatestFrames()
{
    tide::GrainSnapshotExchange exchange;
    tide::GranulatorEngine::VisualFrame first;
    tide::GranulatorEngine::VisualFrame latest;
    first.sequence = 41;
    first.grainCount = 1;
    first.grains[0].eventId = 7;
    latest.sequence = 42;
    latest.grainCount = 2;
    latest.grains[0].eventId = 8;
    latest.grains[1].eventId = 9;

    exchange.publish(first);
    exchange.publish(latest);

    tide::GranulatorEngine::VisualFrame received;
    return exchange.readLatest(received)
        && received.sequence == 42
        && received.grainCount == 2
        && received.grains[0].eventId == 8
        && received.grains[1].eventId == 9;
}

bool grainSnapshotExchangeStaysCoherentAcrossThreads()
{
    tide::GrainSnapshotExchange exchange;
    std::atomic<bool> producerFinished { false };
    std::atomic<bool> coherent { true };

    const auto verifyFrame = [&coherent](const tide::GranulatorEngine::VisualFrame& frame)
    {
        if (frame.sequence == 0)
            return;

        if (frame.generation != frame.sequence
            || frame.grainCount != tide::GranulatorEngine::maximumGrains)
        {
            coherent.store(false, std::memory_order_relaxed);
            return;
        }

        for (auto index = 0; index < frame.grainCount; ++index)
        {
            const auto expected = frame.sequence * 1000ULL
                + static_cast<std::uint64_t>(index);
            if (frame.grains[static_cast<size_t>(index)].eventId != expected)
            {
                coherent.store(false, std::memory_order_relaxed);
                return;
            }
        }
    };

    std::thread producer([&exchange, &producerFinished]
    {
        for (std::uint64_t sequence = 1; sequence <= 25000; ++sequence)
        {
            tide::GranulatorEngine::VisualFrame frame;
            frame.sequence = sequence;
            frame.generation = sequence;
            frame.grainCount = tide::GranulatorEngine::maximumGrains;
            for (auto index = 0; index < frame.grainCount; ++index)
                frame.grains[static_cast<size_t>(index)].eventId = sequence * 1000ULL
                    + static_cast<std::uint64_t>(index);
            exchange.publish(frame);
        }
        producerFinished.store(true, std::memory_order_release);
    });

    tide::GranulatorEngine::VisualFrame received;
    while (! producerFinished.load(std::memory_order_acquire))
    {
        exchange.readLatest(received);
        verifyFrame(received);
    }
    exchange.readLatest(received);
    verifyFrame(received);
    producer.join();

    return coherent.load(std::memory_order_relaxed)
        && received.sequence > 0;
}

bool sourceCoordinateMappingSurvivesWaveformAndFrameSkew()
{
    tide::GranulatorEngine::VisualFrame frame;
    frame.totalSamplesWritten = 10000;
    tide::GranulatorEngine::GrainVisualState grain;
    grain.sourceDelaySamples = 250.0;

    const auto progress = tide::sourceProgressInWindow(
        frame, grain, 9980.0, 1000.0);
    return std::abs(progress - 0.77f) < 1.0e-6f;
}

bool internalHistoryDisplayTapIncludesTheFeedbackReturn()
{
    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 64, 2, 1.0f, 0.85f, 0.8f, 0.5f);
    tide::GranulatorEngine::Controls controls {
        0.2f, 60.0f, 1.0f, 12.0f, 0.5f, 0.0f, 0.85f, 0.8f, 0.5f
    };
    juce::AudioBuffer<float> buffer(2, 64);
    std::array<float, 64> historyTap {};
    auto sawFeedbackInHistory = false;

    for (auto block = 0; block < 20; ++block)
    {
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 0.2f);

        engine.process(buffer,
                       controls,
                       historyTap.data(),
                       static_cast<int>(historyTap.size()));
        for (const auto sample : historyTap)
            sawFeedbackInHistory = sawFeedbackInHistory
                || std::abs(sample - 0.2f) > 1.0e-4f;
    }

    return sawFeedbackInHistory;
}

bool transportResetClearsDisplayedGrainsAndAdvancesGeneration()
{
    TideGrainsAudioProcessor processor;
    TestPlayHead playHead;
    processor.setPlayHead(&playHead);
    processor.prepareToPlay(1000.0, 32);
    juce::AudioBuffer<float> buffer(2, 32);
    juce::MidiBuffer midi;

    playHead.setPosition(0, true);
    for (auto block = 0; block < 20; ++block)
    {
        playHead.setPosition(block * buffer.getNumSamples(), true);
        buffer.clear();
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 0.25f);
        processor.processBlock(buffer, midi);
    }

    tide::GranulatorEngine::VisualFrame before;
    processor.readGrainFrame(before);
    if (before.grainCount == 0)
    {
        processor.setPlayHead(nullptr);
        return false;
    }

    playHead.setPosition(10000, true);
    buffer.clear();
    processor.processBlock(buffer, midi);

    tide::GranulatorEngine::VisualFrame after;
    processor.readGrainFrame(after);
    std::vector<juce::Range<float>> waveform;
    const auto waveformInfo = processor.readWaveform(waveform, 32, 0.2f);
    processor.setPlayHead(nullptr);
    if (after.generation <= before.generation
        || after.generation != waveformInfo.generation
        || after.totalSamplesWritten != buffer.getNumSamples())
    {
        return false;
    }

    for (auto index = 0; index < after.grainCount; ++index)
        if (after.grains[static_cast<size_t>(index)].sourceDelaySamples
            > static_cast<double>(after.totalSamplesWritten))
            return false;

    return true;
}

bool processorDryPathPassesAudioAndFeedsDisplay()
{
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    auto* mix = processor->parameters.getParameter(tide::parameter::mix);
    if (mix == nullptr)
        return false;

    mix->setValueNotifyingHost(0.0f);
    processor->prepareToPlay(48000.0, 256);

    juce::AudioBuffer<float> buffer(2, 256);
    juce::AudioBuffer<float> expected(2, 256);
    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = std::sin(static_cast<float>(sample) * 0.11f)
                * (channel == 0 ? 0.6f : 0.4f);
            buffer.setSample(channel, sample, value);
            expected.setSample(channel, sample, value);
        }
    }

    juce::MidiBuffer midi;
    processor->processBlock(buffer, midi);

    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            if (!approximatelyEqual(buffer.getSample(channel, sample),
                                    expected.getSample(channel, sample)))
            {
                return false;
            }
        }
    }

    std::vector<juce::Range<float>> waveform;
    processor->readWaveform(waveform, 32, 0.02f);
    for (const auto range : waveform)
    {
        if (std::abs(range.getStart()) > 0.01f || std::abs(range.getEnd()) > 0.01f)
            return true;
    }

    return false;
}

bool processorGainStagesControlSignalLevel()
{
    const auto renderLevel = [](const float inputGain, const float outputGain)
    {
        auto processor = std::make_unique<TideGrainsAudioProcessor>();
        const auto setParameter = [&processor](const char* id, const float value)
        {
            auto* parameter = processor->parameters.getParameter(id);
            if (parameter == nullptr)
                return false;

            parameter->setValueNotifyingHost(
                processor->parameters.getParameterRange(id).convertTo0to1(value));
            return true;
        };

        if (!setParameter(tide::parameter::mix, 0.0f)
            || !setParameter(tide::parameter::inputGain, inputGain)
            || !setParameter(tide::parameter::outputGain, outputGain))
        {
            return -1.0f;
        }

        processor->prepareToPlay(48000.0, 32);
        juce::AudioBuffer<float> buffer(2, 32);
        buffer.clear();
        buffer.setSample(0, 0, 0.25f);
        juce::MidiBuffer midi;
        processor->processBlock(buffer, midi);
        return buffer.getSample(0, 0);
    };

    const auto unity = renderLevel(0.0f, 0.0f);
    const auto boostedInput = renderLevel(6.0f, 0.0f);
    const auto attenuatedOutput = renderLevel(0.0f, -6.0f);
    return std::abs(unity - 0.25f) < 1.0e-6f
        && std::abs(boostedInput - 0.25f * juce::Decibels::decibelsToGain(6.0f)) < 1.0e-6f
        && std::abs(attenuatedOutput - 0.25f * juce::Decibels::decibelsToGain(-6.0f)) < 1.0e-6f;
}

bool outputLimiterContainsTheRestoredMaximumGainAndFeedbackState()
{
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        if (parameter == nullptr)
            return false;

        parameter->setValueNotifyingHost(
            processor->parameters.getParameterRange(id).convertTo0to1(value));
        return true;
    };

    if (! setParameter(tide::parameter::outputGain, 12.0f)
        || ! setParameter(tide::parameter::feedback, 0.95f)
        || ! setParameter(tide::parameter::mix, 1.0f)
        || ! setParameter(tide::parameter::density, 64.0f)
        || ! setParameter(tide::parameter::tide, 0.0f)
        || ! setParameter(tide::parameter::drift, 1.0f)
        || ! setParameter(tide::parameter::time, 0.5f)
        || ! setParameter(tide::parameter::size, 40.0f))
    {
        return false;
    }

    constexpr auto sampleRate = 4000.0;
    constexpr auto blockSize = 64;
    constexpr auto blockCount = 600;
    const auto ceiling = juce::Decibels::decibelsToGain(
        tide::OutputSafetyLimiter::ceilingDecibels);

    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    auto finalWindowEnergy = 0.0;

    for (auto block = 0; block < blockCount; ++block)
    {
        for (auto sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = block * blockSize + sample;
            const auto phase = static_cast<float>(absoluteSample);
            buffer.setSample(0, sample, 0.82f * std::sin(phase * 0.071f));
            buffer.setSample(1, sample, 0.76f * std::sin(phase * 0.053f + 0.7f));
        }

        processor->processBlock(buffer, midi);
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            for (auto sample = 0; sample < blockSize; ++sample)
            {
                const auto value = buffer.getSample(channel, sample);
                if (! std::isfinite(value) || std::abs(value) > ceiling + 1.0e-6f)
                    return false;

                if (block >= blockCount - 100)
                    finalWindowEnergy += static_cast<double>(value) * value;
            }
        }
    }

    return finalWindowEnergy > 1.0
        && processor->getOutputLevel() <= ceiling + 1.0e-6f
        && processor->getLimiterActivity() > 0.0f;
}

bool outputLimiterSilencesNonFiniteSamples()
{
    tide::OutputSafetyLimiter limiter;
    limiter.prepare(48000.0);
    juce::AudioBuffer<float> buffer(2, 4);
    buffer.clear();
    buffer.setSample(0, 0, std::numeric_limits<float>::infinity());
    buffer.setSample(1, 1, std::numeric_limits<float>::quiet_NaN());
    buffer.setSample(0, 2, 8.0f);
    buffer.setSample(1, 2, -7.0f);
    limiter.process(buffer);

    const auto ceiling = juce::Decibels::decibelsToGain(
        tide::OutputSafetyLimiter::ceilingDecibels);
    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (! std::isfinite(buffer.getSample(channel, sample))
                || std::abs(buffer.getSample(channel, sample)) > ceiling + 1.0e-6f)
            {
                return false;
            }

    return buffer.getSample(0, 0) == 0.0f
        && buffer.getSample(1, 1) == 0.0f;
}

bool maximumFeedbackDecaysInsteadOfLatchingTheLimiter()
{
    auto processor = std::make_unique<TideGrainsAudioProcessor>();
    const auto setParameter = [&processor](const char* id, const float value)
    {
        auto* parameter = processor->parameters.getParameter(id);
        if (parameter == nullptr)
            return false;

        parameter->setValueNotifyingHost(
            processor->parameters.getParameterRange(id).convertTo0to1(value));
        return true;
    };

    if (! setParameter(tide::parameter::outputGain, 12.0f)
        || ! setParameter(tide::parameter::feedback, 0.95f)
        || ! setParameter(tide::parameter::mix, 1.0f)
        || ! setParameter(tide::parameter::density, 64.0f)
        || ! setParameter(tide::parameter::tide, 0.0f)
        || ! setParameter(tide::parameter::drift, 1.0f)
        || ! setParameter(tide::parameter::time, 0.5f)
        || ! setParameter(tide::parameter::size, 40.0f))
    {
        return false;
    }

    constexpr auto sampleRate = 4000.0;
    constexpr auto blockSize = 64;
    constexpr auto blockCount = 600;
    processor->prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    auto finalWindowPeak = 0.0f;

    for (auto block = 0; block < blockCount; ++block)
    {
        buffer.clear();
        if (block < 20)
        {
            for (auto sample = 0; sample < blockSize; ++sample)
            {
                const auto phase = static_cast<float>(block * blockSize + sample);
                buffer.setSample(0, sample, 0.4f * std::sin(phase * 0.071f));
                buffer.setSample(1, sample, 0.35f * std::sin(phase * 0.053f + 0.7f));
            }
        }

        processor->processBlock(buffer, midi);
        if (block >= blockCount - 100)
            finalWindowPeak = juce::jmax(finalWindowPeak,
                                         buffer.getMagnitude(0, buffer.getNumSamples()));
    }

    const auto limiterActivity = processor->getLimiterActivity();
    const auto passes = finalWindowPeak < 0.5f && limiterActivity < 0.01f;
    if (! passes)
        std::cerr << "Final feedback peak: " << finalWindowPeak
                  << ", limiter activity: " << limiterActivity << '\n';
    return passes;
}

std::vector<float> renderEngine(const tide::GranulatorEngine::Controls& controls,
                                const std::uint64_t seed,
                                const int blockCount = 180)
{
    tide::GranulatorEngine engine;
    engine.prepare(48000.0, 64, 2, controls.mix);
    engine.setRandomSeed(seed);

    juce::AudioBuffer<float> buffer(2, 64);
    std::vector<float> output;
    output.reserve(static_cast<size_t>(blockCount * buffer.getNumSamples() * 2));
    for (auto block = 0; block < blockCount; ++block)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto absoluteSample = block * buffer.getNumSamples() + sample;
            const auto phase = static_cast<float>(absoluteSample) * 0.017f;
            buffer.setSample(0, sample, std::sin(phase) * 0.7f);
            buffer.setSample(1, sample, std::cos(phase * 0.73f) * 0.45f);
        }

        engine.process(buffer, controls);
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            output.push_back(buffer.getSample(0, sample));
            output.push_back(buffer.getSample(1, sample));
        }
    }

    return output;
}

std::vector<float> renderEngineAcrossBlockSize(const int blockSize)
{
    constexpr auto sampleRate = 8000.0;
    constexpr auto totalSamples = 10880;
    tide::GranulatorEngine::Controls controls;
    controls.timeSeconds = 0.4f;
    controls.grainSizeMilliseconds = 75.0f;
    controls.mix = 1.0f;
    controls.density = 37.0f;
    controls.shape = 0.4f;
    controls.spread = 0.7f;
    controls.feedback = 0.0f;
    controls.tide = 0.63f;
    controls.drift = 0.72f;

    tide::GranulatorEngine engine;
    engine.prepare(sampleRate,
                   blockSize,
                   2,
                   controls.mix,
                   controls.feedback,
                   controls.tide,
                   controls.drift);
    engine.setRandomSeed(0xb10c51aeULL);

    juce::AudioBuffer<float> buffer(2, blockSize);
    std::vector<float> output;
    output.reserve(static_cast<size_t>(totalSamples * 2));
    for (auto position = 0; position < totalSamples; position += blockSize)
    {
        for (auto sample = 0; sample < blockSize; ++sample)
        {
            const auto absoluteSample = position + sample;
            const auto phase = static_cast<float>(absoluteSample);
            buffer.setSample(0, sample, 0.31f * std::sin(phase * 0.037f));
            buffer.setSample(1, sample, 0.23f * std::cos(phase * 0.061f + 0.3f));
        }
        engine.process(buffer, controls);
        for (auto sample = 0; sample < blockSize; ++sample)
        {
            output.push_back(buffer.getSample(0, sample));
            output.push_back(buffer.getSample(1, sample));
        }
    }
    return output;
}

bool collectiveTrajectoryIsIndependentOfHostBlockSize()
{
    const auto oneSample = renderEngineAcrossBlockSize(1);
    const auto seventeenSamples = renderEngineAcrossBlockSize(17);
    const auto sixtyFourSamples = renderEngineAcrossBlockSize(64);
    if (oneSample.size() != seventeenSamples.size()
        || oneSample.size() != sixtyFourSamples.size())
    {
        return false;
    }

    auto maximumDifference = 0.0f;
    for (size_t index = 0; index < oneSample.size(); ++index)
    {
        maximumDifference = juce::jmax(
            maximumDifference,
            std::abs(oneSample[index] - seventeenSamples[index]));
        maximumDifference = juce::jmax(
            maximumDifference,
            std::abs(oneSample[index] - sixtyFourSamples[index]));
    }
    if (maximumDifference <= 1.0e-7f)
        std::cout << "Block-size simulation: 1, 17, and 64-sample blocks agree within "
                  << maximumDifference << '\n';
    return maximumDifference <= 1.0e-7f;
}

bool deterministicGrainChoicesRepeatExactly()
{
    const tide::GranulatorEngine::Controls controls { 0.3f, 40.0f, 1.0f, 10.0f, 0.5f, 1.0f };
    const auto first = renderEngine(controls, 0x12345678ULL);
    const auto second = renderEngine(controls, 0x12345678ULL);
    if (first != second)
        return false;

    const auto differentSeed = renderEngine(controls, 0x87654321ULL);
    return std::any_of(first.begin(), first.end(), [index = size_t { 0 }, &differentSeed](const float value) mutable
    {
        const auto differs = std::abs(value - differentSeed[index]) > 1.0e-5f;
        ++index;
        return differs;
    });
}

bool shapeMorphChangesTheGrainEnvelope()
{
    const tide::GranulatorEngine::Controls sawControls { 0.2f, 40.0f, 1.0f, 10.0f, 0.0f, 0.0f };
    const tide::GranulatorEngine::Controls sineControls { 0.2f, 40.0f, 1.0f, 10.0f, 1.0f, 0.0f };
    const auto saw = renderEngine(sawControls, 0xabcdefULL);
    const auto sine = renderEngine(sineControls, 0xabcdefULL);
    if (saw.size() != sine.size())
        return false;

    return std::any_of(saw.begin(), saw.end(), [index = size_t { 0 }, &sine](const float value) mutable
    {
        const auto differs = std::abs(value - sine[index]) > 1.0e-4f;
        ++index;
        return differs;
    });
}

bool spreadMovesStereoContentAcrossTheField()
{
    tide::GranulatorEngine engine;
    engine.prepare(48000.0, 64, 2, 1.0f);
    engine.setRandomSeed(0x424242ULL);
    const tide::GranulatorEngine::Controls controls { 0.2f, 40.0f, 1.0f, 10.0f, 0.5f, 1.0f };
    juce::AudioBuffer<float> buffer(2, 64);
    auto heardRight = false;

    for (auto block = 0; block < 180; ++block)
    {
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            buffer.setSample(0, sample, 0.7f);
            buffer.setSample(1, sample, 0.0f);
        }
        engine.process(buffer, controls);
        for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            heardRight = heardRight || std::abs(buffer.getSample(1, sample)) > 0.01f;
    }

    return heardRight;
}

float renderFeedbackTailEnergy(const float feedback)
{
    tide::GranulatorEngine engine;
    engine.prepare(1000.0, 32, 1, 1.0f, feedback);
    engine.setRandomSeed(0xfeedbacULL);
    const tide::GranulatorEngine::Controls controls {
        0.08f, 20.0f, 1.0f, 24.0f, 0.5f, 0.0f, feedback
    };
    juce::AudioBuffer<float> buffer(1, 32);
    auto tailEnergy = 0.0f;

    for (auto block = 0; block < 40; ++block)
    {
        buffer.clear();
        if (block < 4)
        {
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(0, sample, 0.5f);
        }

        engine.process(buffer, controls);
        if (block >= 8)
        {
            for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = buffer.getSample(0, sample);
                if (!std::isfinite(value) || std::abs(value) > 64.0f)
                    return -1.0f;
                tailEnergy += value * value;
            }
        }
    }

    return tailEnergy;
}

bool feedbackRegeneratesTheWetTailAndStaysBounded()
{
    const auto withoutFeedback = renderFeedbackTailEnergy(0.0f);
    const auto withFeedback = renderFeedbackTailEnergy(0.9f);
    return withoutFeedback >= 0.0f
        && withFeedback > withoutFeedback + 1.0e-5f;
}

bool processorStateRestoresGranularControls()
{
    TideGrainsAudioProcessor source;
    const auto setParameter = [&source](const char* id, const float value)
    {
        if (auto* parameter = source.parameters.getParameter(id))
        {
            const auto normalised = source.parameters.getParameterRange(id).convertTo0to1(value);
            parameter->setValueNotifyingHost(normalised);
            return true;
        }
        return false;
    };

    if (!setParameter(tide::parameter::density, 57.0f)
        || !setParameter(tide::parameter::shape, 0.17f)
        || !setParameter(tide::parameter::spread, 0.83f)
        || !setParameter(tide::parameter::tide, 0.64f)
        || !setParameter(tide::parameter::drift, 0.42f)
        || !setParameter(tide::parameter::feedback, 0.71f)
        || !setParameter(tide::parameter::inputGain, 5.2f)
        || !setParameter(tide::parameter::outputGain, -3.4f))
    {
        return false;
    }

    juce::MemoryBlock state;
    source.getStateInformation(state);
    TideGrainsAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    const auto matches = [&restored](const char* id, const float expected)
    {
        const auto* parameter = restored.parameters.getRawParameterValue(id);
        return parameter != nullptr && std::abs(parameter->load() - expected) < 1.0e-4f;
    };
    return matches(tide::parameter::density, 57.0f)
        && matches(tide::parameter::shape, 0.17f)
        && matches(tide::parameter::spread, 0.83f)
        && matches(tide::parameter::tide, 0.64f)
        && matches(tide::parameter::drift, 0.42f)
        && matches(tide::parameter::feedback, 0.71f)
        && matches(tide::parameter::inputGain, 5.2f)
        && matches(tide::parameter::outputGain, -3.4f)
        && restored.parameters.state.getProperty("randomSeed").toString()
            == source.parameters.state.getProperty("randomSeed").toString();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;

    if (!drySignalPassesThrough())
    {
        std::cerr << "Dry pass-through test failed\n";
        return 1;
    }

    if (!largestSizeLastsOneSecond())
    {
        std::cerr << "Maximum Size lifetime test failed\n";
        return 1;
    }

    if (!delayedGrainBecomesAudibleAndStaysBounded())
    {
        std::cerr << "Delayed grain test failed\n";
        return 1;
    }

    if (!cachedPanAmountTracksBirthPanExactly())
    {
        std::cerr << "Cached pan-amount test failed\n";
        return 1;
    }

    if (!densityRangeAndEngineCeilingAreOneToSixtyFour())
    {
        std::cerr << "Density range/engine ceiling test failed\n";
        return 1;
    }

    if (!grainsAreIndependentOneShotEventsWithTargetOverlap())
    {
        std::cerr << "Asynchronous one-shot lifecycle test failed\n";
        return 1;
    }

    if (!soundingGrainsKeepTheirBirthSettingsDuringAutomation())
    {
        std::cerr << "Per-birth parameter retention test failed\n";
        return 1;
    }

    if (!densityChangesPreserveTailsAndFillWithoutABurst())
    {
        std::cerr << "Density tail/fill scheduler test failed\n";
        return 1;
    }

    if (!densityAutomationDoesNotClickTheWetField())
    {
        std::cerr << "Density-automation click test failed\n";
        return 1;
    }

    if (!tideWidensSourceTrailWithoutChangingBirthTiming())
    {
        std::cerr << "Tide source-trail test failed\n";
        return 1;
    }

    if (!driftAddsBoundedBirthVariation())
    {
        std::cerr << "Bounded Drift birth-variation test failed\n";
        return 1;
    }

    if (!collectiveWaveRemainsAudibleAfterNormalisation())
    {
        std::cerr << "Asynchronous texture anti-pulse test failed\n";
        return 1;
    }

    if (!processorForwardsTideIntoTheAudioEngine())
    {
        std::cerr << "Processor Tide-forwarding test failed\n";
        return 1;
    }

    if (!processorForwardsDriftIntoTheAudioEngine())
    {
        std::cerr << "Processor Drift-forwarding test failed\n";
        return 1;
    }

    if (!sourceTimbreSurvivesBothTideExtremesAndMaximumDrift())
    {
        std::cerr << "Source-timbre retention test failed\n";
        return 1;
    }

    if (!denseProcessorRenderKeepsRealtimeHeadroom())
    {
        std::cerr << "Dense realtime-throughput test failed\n";
        return 1;
    }

    if (!increasingDensityDoesNotAttenuateTheWetField())
    {
        std::cerr << "Density energy-normalisation test failed\n";
        return 1;
    }

    if (!transportResetCannotReadPreSeekAudio())
    {
        std::cerr << "Transport history reset test failed\n";
        return 1;
    }

    if (!stoppedTransportStillProducesWetOutput())
    {
        std::cerr << "Stopped-transport wet output test failed\n";
        return 1;
    }

    if (!waveformWindowFillsEveryColumnAndTracksTime())
    {
        std::cerr << "Waveform history test failed\n";
        return 1;
    }

    if (!waveformWindowPreservesItsAbsoluteHistoryCoordinates())
    {
        std::cerr << "Waveform absolute-coordinate test failed\n";
        return 1;
    }

    if (!publishedGrainTelemetryMatchesTheAudioEngineExactly())
    {
        std::cerr << "Grain telemetry truth test failed\n";
        return 1;
    }

    if (!grainSnapshotExchangePublishesOnlyCoherentLatestFrames())
    {
        std::cerr << "Grain snapshot coherence test failed\n";
        return 1;
    }

    if (!grainSnapshotExchangeStaysCoherentAcrossThreads())
    {
        std::cerr << "Grain snapshot threaded-coherence test failed\n";
        return 1;
    }

    if (!sourceCoordinateMappingSurvivesWaveformAndFrameSkew())
    {
        std::cerr << "Grain source-coordinate mapping test failed\n";
        return 1;
    }

    if (!internalHistoryDisplayTapIncludesTheFeedbackReturn())
    {
        std::cerr << "Internal-history feedback display test failed\n";
        return 1;
    }

    if (!transportResetClearsDisplayedGrainsAndAdvancesGeneration())
    {
        std::cerr << "Display transport-generation test failed\n";
        return 1;
    }

    if (!processorDryPathPassesAudioAndFeedsDisplay())
    {
        std::cerr << "Processor pass-through test failed\n";
        return 1;
    }

    if (!processorGainStagesControlSignalLevel())
    {
        std::cerr << "Processor gain-stage test failed\n";
        return 1;
    }

    if (!outputLimiterContainsTheRestoredMaximumGainAndFeedbackState())
    {
        std::cerr << "Output safety limiter stress test failed\n";
        return 1;
    }

    if (!outputLimiterSilencesNonFiniteSamples())
    {
        std::cerr << "Output safety limiter non-finite test failed\n";
        return 1;
    }

    if (!maximumFeedbackDecaysInsteadOfLatchingTheLimiter())
    {
        std::cerr << "Maximum-feedback decay test failed\n";
        return 1;
    }

    if (!collectiveTrajectoryIsIndependentOfHostBlockSize())
    {
        std::cerr << "Host block-size invariance test failed\n";
        return 1;
    }

    if (!deterministicGrainChoicesRepeatExactly())
    {
        std::cerr << "Deterministic grain test failed\n";
        return 1;
    }

    if (!shapeMorphChangesTheGrainEnvelope())
    {
        std::cerr << "Shape envelope test failed\n";
        return 1;
    }

    if (!spreadMovesStereoContentAcrossTheField())
    {
        std::cerr << "Stereo spread test failed\n";
        return 1;
    }

    if (!feedbackRegeneratesTheWetTailAndStaysBounded())
    {
        std::cerr << "Feedback regeneration test failed\n";
        return 1;
    }

    if (!processorStateRestoresGranularControls())
    {
        std::cerr << "Processor state test failed\n";
        return 1;
    }

    std::cout << "TIDES DSP tests passed\n";
    return 0;
}
