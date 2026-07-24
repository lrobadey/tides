#include "GranulatorEngine.h"

#include <algorithm>
#include <cmath>

namespace tide
{
namespace
{
constexpr float baseSafetyMarginSeconds = 0.010f;
constexpr float minimumGrainSeconds = 0.005f;
constexpr float maximumGrainSeconds = 1.000f;
constexpr float minimumTimeSeconds = 0.020f;
constexpr float maximumTimeSeconds = 5.0f;
constexpr float maximumSpeedDeviation = 0.01f;
constexpr float maximumLifetimeVariation = 0.15f;
constexpr float maximumEventTimingJitter = 0.15f;
constexpr float maximumLaneRateSpread = 0.50f;
constexpr float laneRateWalkStep = 0.035f;
constexpr float minimumTrailProportion = 0.015f;
constexpr float maximumTrailProportion = 0.55f;
constexpr float maximumSourceDriftSeconds = 0.020f;
constexpr float maximumSourceDriftWindowProportion = 0.02f;
constexpr float goldenRatioConjugate = 0.61803398875f;
constexpr double schedulerEpsilon = 1.0e-9;
constexpr double ppqEpsilon = 1.0e-10;

float smoothStep(const float value) noexcept
{
    const auto clamped = juce::jlimit(0.0f, 1.0f, value);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

// sin(pi * phase) for phase in [0, 1], precomputed once. This term is evaluated
// once per active grain per sample in the audio loop; the table trades a libm
// transcendental for a lerp'd lookup with error below 5e-7 versus std::sin.
struct HalfSineTable
{
    static constexpr int resolution = 2048;
    std::array<float, resolution + 1> values {};

    HalfSineTable() noexcept
    {
        for (auto index = 0; index <= resolution; ++index)
            values[static_cast<size_t>(index)] = std::sin(
                juce::MathConstants<float>::pi
                * (static_cast<float>(index) / static_cast<float>(resolution)));
    }
};

const HalfSineTable halfSineTable;

float halfSine(const float phase) noexcept
{
    const auto scaled = juce::jlimit(0.0f, 1.0f, phase)
        * static_cast<float>(HalfSineTable::resolution);
    const auto lowerIndex = juce::jmin(static_cast<int>(scaled),
                                       HalfSineTable::resolution - 1);
    const auto fraction = scaled - static_cast<float>(lowerIndex);
    const auto lower = halfSineTable.values[static_cast<size_t>(lowerIndex)];
    const auto upper = halfSineTable.values[static_cast<size_t>(lowerIndex) + 1];
    return lower + fraction * (upper - lower);
}
}

void GranulatorEngine::prepare(const double sampleRate,
                               const int maximumBlockSize,
                               const int channelCount,
                               const float initialMix,
                               const float initialFeedback,
                               const float initialTide,
                               const float initialDrift)
{
    juce::ignoreUnused(maximumBlockSize);

    currentSampleRate = juce::jmax(1.0, sampleRate);
    currentChannelCount = std::clamp(channelCount, 1, 2);
    historyLength = static_cast<int>(std::ceil(5.0 * currentSampleRate)) + 2;

    history.setSize(currentChannelCount, historyLength, false, true, false);
    mixSmoother.reset(currentSampleRate, 0.02);
    mixSmoother.setCurrentAndTargetValue(juce::jlimit(0.0f, 1.0f, initialMix));
    feedbackSmoother.reset(currentSampleRate, 0.02);
    feedbackSmoother.setCurrentAndTargetValue(juce::jlimit(0.0f, 0.95f, initialFeedback));
    tideSmoother.reset(currentSampleRate, 0.08);
    tideSmoother.setCurrentAndTargetValue(juce::jlimit(0.0f, 1.0f, initialTide));
    driftSmoother.reset(currentSampleRate, 0.08);
    driftSmoother.setCurrentAndTargetValue(juce::jlimit(0.0f, 1.0f, initialDrift));

    normalizationAttack = static_cast<float>(
        1.0 - std::exp(-1.0 / (currentSampleRate * 0.020)));
    normalizationRelease = static_cast<float>(
        1.0 - std::exp(-1.0 / (currentSampleRate * 0.180)));

    reset();
}

void GranulatorEngine::reset() noexcept
{
    history.clear();
    resetHistory(0);
}

void GranulatorEngine::resetHistory(const std::int64_t newTimelineSample) noexcept
{
    resetPlayback(newTimelineSample);
    writePosition = 0;
    totalSamplesWritten = 0;
}

void GranulatorEngine::resetPlayback(const std::int64_t newTimelineSample) noexcept
{
    for (auto& grain : grains)
        grain = {};

    timelineSample = newTimelineSample;
    nominalWetNormalization = 1.0f;
    activeGrainCount = 0;
    birthOrdinal = 0;
    freeState = {};
    syncState = {};
}

void GranulatorEngine::setRandomSeed(const std::uint64_t seed) noexcept
{
    randomSeed = seed;
}

void GranulatorEngine::process(juce::AudioBuffer<float>& buffer,
                               const Controls& controls,
                               const Timing& timing,
                               float* internalHistoryTap,
                               const int internalHistoryTapCapacity) noexcept
{
    jassert(currentSampleRate > 0.0);
    jassert(historyLength > 0);

    const auto channelsToProcess = std::min(buffer.getNumChannels(), currentChannelCount);
    if (channelsToProcess <= 0 || buffer.getNumSamples() <= 0)
        return;

    if (internalHistoryTapCapacity < buffer.getNumSamples())
        internalHistoryTap = nullptr;

    std::array<float*, 2> outputChannels {};
    std::array<float*, 2> historyChannels {};
    for (auto channel = 0; channel < channelsToProcess; ++channel)
    {
        outputChannels[static_cast<size_t>(channel)] = buffer.getWritePointer(channel);
        historyChannels[static_cast<size_t>(channel)] = history.getWritePointer(channel);
    }

    mixSmoother.setTargetValue(juce::jlimit(0.0f, 1.0f, controls.mix));
    feedbackSmoother.setTargetValue(juce::jlimit(0.0f, 0.95f, controls.feedback));
    tideSmoother.setTargetValue(juce::jlimit(0.0f, 1.0f, controls.tide));
    driftSmoother.setTargetValue(juce::jlimit(0.0f, 1.0f, controls.drift));
    const auto validClock = timing.clockValid && std::isfinite(timing.bpm)
        && timing.bpm > 0.0 && std::isfinite(timing.ppqPosition);
    const auto ppqPerSample = validClock
        ? timing.bpm / (60.0 * currentSampleRate)
        : 0.0;

    if (timing.discontinuity)
        syncState = {};

    // A boundary commit is unreachable without a running host clock, so a
    // disable request must hand back to the Free scheduler immediately rather
    // than stranding it behind a boundary that never arrives.
    if (syncState.active && ! controls.sync && (! timing.playing || ! validClock))
    {
        releaseAllSyncVoices();
        syncState = {};
        freeState = {};
    }

    if (! controls.sync && ! syncState.active)
        reconcileFreeLanes(controls);

    if (controls.sync && timing.playing && ! validClock && syncState.clockWasValid)
        releaseAllSyncVoices();

    if (controls.sync && timing.playing && validClock && syncState.active
        && ! syncState.clockWasValid && ! timing.discontinuity)
    {
        const auto duration = divisionPpq(controls.syncDivision);
        syncState.awaitingBoundary = true;
        syncState.nextBoundaryPpq = (std::floor(timing.ppqPosition / duration) + 1.0)
            * duration;
        for (auto& lane : syncState.lanes)
            lane.onsetPending = false;
    }

    if (syncState.active)
        syncState.disabling = ! controls.sync;

    for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        std::array<float, 2> dry {};
        std::array<float, 2> wet {};
        auto envelopeAmplitude = 0.0f;
        auto expectedEnvelopeMass = 0.0f;
        auto averageBirthTide = 0.0f;
        auto readablePopulation = 0;

        for (auto channel = 0; channel < channelsToProcess; ++channel)
        {
            dry[static_cast<size_t>(channel)] =
                outputChannels[static_cast<size_t>(channel)][sample];
            historyChannels[static_cast<size_t>(channel)][writePosition] =
                dry[static_cast<size_t>(channel)];
        }

        const auto tide = tideSmoother.getNextValue();
        const auto drift = driftSmoother.getNextValue();
        const auto currentPpq = validClock
            ? timing.ppqPosition + static_cast<double>(sample) * ppqPerSample
            : timing.ppqPosition;
        if (controls.sync || syncState.active)
            processSyncEvents(controls, timing, currentPpq);
        else
            processFreeEvents(controls, tide, drift);

        for (auto& grain : grains)
        {
            if (! grain.active)
                continue;

            const auto phase = grain.syncVoice
                ? static_cast<float>(grain.lifeProgress)
                : static_cast<float>(grain.ageInSamples) * grain.phaseScale;
            const auto envelope = grainEnvelope(phase, grain.shapeAtBirth);
            const auto releaseGain = grain.releaseSamplesRemaining > 0
                ? static_cast<float>(grain.releaseSamplesRemaining)
                    / static_cast<float>(juce::jmax(1, grain.releaseSamplesTotal))
                : 1.0f;
            const auto renderedEnvelope = envelope * releaseGain;

            if (grain.hasReadableSource)
            {
                envelopeAmplitude += renderedEnvelope;
                expectedEnvelopeMass += grain.envelopeMassAtBirth;
                averageBirthTide += grain.tideAtBirth;
                ++readablePopulation;
            }

            if (grain.hasReadableSource && channelsToProcess == 1)
            {
                grain.lastOutputLeft =
                    readHistorySample(historyChannels[0], grain.readPosition) * renderedEnvelope;
                grain.lastOutputRight = 0.0f;
                wet[0] += grain.lastOutputLeft;
            }
            else if (grain.hasReadableSource)
            {
                const auto left = readHistorySample(historyChannels[0], grain.readPosition);
                const auto right = readHistorySample(historyChannels[1], grain.readPosition);
                const auto centredImage = (left + right) * 0.70710678118f;
                if (grain.pan < 0.0f)
                {
                    grain.lastOutputLeft = (left * (1.0f - grain.panAmount)
                               + centredImage * grain.panAmount) * renderedEnvelope;
                    grain.lastOutputRight = right * (1.0f - grain.panAmount) * renderedEnvelope;
                }
                else
                {
                    grain.lastOutputLeft = left * (1.0f - grain.panAmount) * renderedEnvelope;
                    grain.lastOutputRight = (right * (1.0f - grain.panAmount)
                               + centredImage * grain.panAmount) * renderedEnvelope;
                }
                wet[0] += grain.lastOutputLeft;
                wet[1] += grain.lastOutputRight;
            }

            if (grain.hasReadableSource)
            {
                grain.readPosition += grain.playbackSpeed;
                while (grain.readPosition >= static_cast<double>(historyLength))
                    grain.readPosition -= static_cast<double>(historyLength);
            }

            ++grain.ageInSamples;
            if (grain.syncVoice)
            {
                const auto envelopePpqPerSample = validClock && timing.playing
                    ? ppqPerSample
                    : (! timing.playing && timing.bpm > 0.0
                        ? timing.bpm / (60.0 * currentSampleRate) : 0.0);
                if (grain.lifetimePpq > 0.0)
                    grain.lifeProgress += envelopePpqPerSample / grain.lifetimePpq;
            }
            if (grain.releaseSamplesRemaining > 0)
                --grain.releaseSamplesRemaining;
            if ((grain.syncVoice && grain.lifeProgress >= 1.0)
                || (! grain.syncVoice && grain.ageInSamples >= grain.lengthInSamples)
                || (grain.releaseSamplesTotal > 0 && grain.releaseSamplesRemaining <= 0))
            {
                grain.active = false;
                grain.hasReadableSource = false;
                --activeGrainCount;
            }
        }

        if (syncState.activeCutTails > 0)
        {
            for (auto& tail : syncState.cutTails)
            {
                if (tail.samplesRemaining <= 0)
                    continue;
                const auto tailGain = static_cast<float>(tail.samplesRemaining)
                    / static_cast<float>(juce::jmax(1, tail.samplesTotal));
                wet[0] += tail.levelLeft * tailGain;
                wet[1] += tail.levelRight * tailGain;
                if (--tail.samplesRemaining <= 0)
                    --syncState.activeCutTails;
            }
        }

        // Independent one-shots should overlap into a mostly steady field. Low
        // Tide remains source-coherent and uses conservative linear gain, while
        // high Tide moves toward energy normalisation as birth anchors spread.
        const auto meanBirthTide = readablePopulation > 0
            ? averageBirthTide / static_cast<float>(readablePopulation)
            : tide;
        const auto linearNormalization = juce::jmax(1.0f, expectedEnvelopeMass);
        const auto energyNormalization = std::sqrt(linearNormalization);
        const auto targetNormalization = linearNormalization
            + (energyNormalization - linearNormalization) * smoothStep(meanBirthTide);
        const auto normalizationFollow = targetNormalization > nominalWetNormalization
            ? normalizationAttack
            : normalizationRelease;
        nominalWetNormalization += (targetNormalization - nominalWetNormalization)
            * normalizationFollow;

        const auto feedbackCompensation = 1.0f / juce::jmax(1.0f, envelopeAmplitude);
        const auto mix = mixSmoother.getNextValue();
        const auto feedback = feedbackSmoother.getNextValue();
        const auto dryGain = std::cos(juce::MathConstants<float>::halfPi * mix);
        const auto wetGain = std::sin(juce::MathConstants<float>::halfPi * mix);
        auto internalHistoryMono = 0.0f;
        for (auto channel = 0; channel < channelsToProcess; ++channel)
        {
            const auto audibleWet = wet[static_cast<size_t>(channel)]
                / juce::jmax(1.0f, nominalWetNormalization);
            const auto feedbackSignal = juce::jlimit(
                -1.0f,
                1.0f,
                wet[static_cast<size_t>(channel)] * feedbackCompensation);
            const auto storedSource = dry[static_cast<size_t>(channel)]
                + feedbackSignal * feedback;
            historyChannels[static_cast<size_t>(channel)][writePosition] = storedSource;
            internalHistoryMono += storedSource;
            outputChannels[static_cast<size_t>(channel)][sample] =
                dry[static_cast<size_t>(channel)] * dryGain
                + audibleWet * wetGain;
        }

        if (internalHistoryTap != nullptr)
            internalHistoryTap[sample] = internalHistoryMono
                / static_cast<float>(channelsToProcess);

        if (++writePosition >= historyLength)
            writePosition = 0;
        ++totalSamplesWritten;
        ++timelineSample;
    }

    syncState.clockWasValid = validClock;
    syncState.wasPlaying = timing.playing;
}

void GranulatorEngine::getVisualFrame(VisualFrame& destination) const noexcept
{
    destination.totalSamplesWritten = totalSamplesWritten;
    destination.sampleRate = currentSampleRate;
    destination.grainCount = 0;

    for (const auto& grain : grains)
    {
        if (! grain.active || ! grain.hasReadableSource)
            continue;

        auto sourceDelay = static_cast<double>(writePosition) - grain.readPosition;
        while (sourceDelay < 0.0)
            sourceDelay += static_cast<double>(historyLength);
        while (sourceDelay >= static_cast<double>(historyLength))
            sourceDelay -= static_cast<double>(historyLength);

        const auto denominator = juce::jmax(1, grain.lengthInSamples - 1);
        const auto phase = grain.syncVoice
            ? static_cast<float>(grain.lifeProgress)
            : static_cast<float>(grain.ageInSamples) / static_cast<float>(denominator);
        destination.grains[static_cast<size_t>(destination.grainCount++)] = {
            grain.eventId,
            sourceDelay,
            grain.playbackSpeed,
            grainEnvelope(phase, grain.shapeAtBirth),
            grain.pan,
            grain.ageInSamples,
            grain.lengthInSamples
            , juce::jlimit(0.0f, 1.0f, phase)
        };
    }
}

double GranulatorEngine::divisionPpq(const int division) noexcept
{
    constexpr std::array<int, 9> ticks { 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    return static_cast<double>(ticks[static_cast<size_t>(juce::jlimit(0, 8, division))]) / 16.0;
}

void GranulatorEngine::releaseAllSyncVoices() noexcept
{
    const auto release = juce::jmax(1, static_cast<int>(std::round(currentSampleRate * 0.001)));
    for (auto& grain : grains)
    {
        if (grain.active && grain.syncVoice)
        {
            grain.releaseSamplesRemaining = release;
            grain.releaseSamplesTotal = release;
        }
    }
}

void GranulatorEngine::processSyncEvents(const Controls& controls,
                                         const Timing& timing,
                                         const double currentPpq) noexcept
{
    if (! timing.playing || ! timing.clockValid)
        return;

    const auto requestedDuration = divisionPpq(controls.syncDivision);
    if (! syncState.active)
    {
        syncState.active = true;
        syncState.awaitingBoundary = true;
        syncState.durationPpq = requestedDuration;
        syncState.division = juce::jlimit(0, 8, controls.syncDivision);
        syncState.nextBoundaryPpq = (std::floor(currentPpq / requestedDuration) + 1.0)
            * requestedDuration;
        const auto samplesToBoundary = juce::jmax(1, static_cast<int>(std::ceil(
            (syncState.nextBoundaryPpq - currentPpq) * 60.0 * currentSampleRate / timing.bpm)));
        for (auto& grain : grains)
        {
            if (grain.active && ! grain.syncVoice)
            {
                grain.releaseSamplesRemaining = samplesToBoundary;
                grain.releaseSamplesTotal = samplesToBoundary;
            }
        }
        return;
    }

    syncState.disabling = ! controls.sync;

    if (syncState.awaitingBoundary && syncState.disabling)
    {
        for (auto& grain : grains)
        {
            if (grain.active && ! grain.syncVoice)
            {
                grain.releaseSamplesRemaining = 0;
                grain.releaseSamplesTotal = 0;
            }
        }
        syncState = {};
        freeState = {};
        reconcileFreeLanes(controls);
        return;
    }

    if (syncState.disabling && currentPpq + ppqEpsilon >= syncState.nextBoundaryPpq)
    {
        releaseAllSyncVoices();
        syncState = {};
        freeState = {};
        reconcileFreeLanes(controls);
        return;
    }

    if (syncState.awaitingBoundary && currentPpq + ppqEpsilon >= syncState.nextBoundaryPpq)
    {
        syncState.awaitingBoundary = false;
        commitSyncBoundary(controls, syncState.nextBoundaryPpq, timing.bpm);
    }
    else if (! syncState.awaitingBoundary
             && currentPpq + ppqEpsilon >= syncState.nextBoundaryPpq)
    {
        commitSyncBoundary(controls, syncState.nextBoundaryPpq, timing.bpm);
    }

    for (auto lane = 0; lane < syncState.density; ++lane)
    {
        auto& state = syncState.lanes[static_cast<size_t>(lane)];
        if (state.onsetPending && currentPpq + ppqEpsilon >= state.onsetPpq)
        {
            beginSyncLane(lane, controls, timing.bpm);
            state.onsetPending = false;
        }
    }
}

void GranulatorEngine::commitSyncBoundary(const Controls& controls,
                                           const double boundaryPpq,
                                           const double bpm) noexcept
{
    const auto newDivision = juce::jlimit(0, 8, controls.syncDivision);
    const auto duration = divisionPpq(newDivision);
    const auto density = juce::jlimit(1, maximumLanes,
                                     static_cast<int>(std::round(controls.density)));
    const auto drift = juce::jlimit(0.0f, 1.0f, controls.drift);

    if (! syncState.gridEnd && controls.gridEnd)
        releaseAllSyncVoices();

    syncState.phaseOriginPpq = boundaryPpq;
    syncState.durationPpq = duration;
    syncState.division = newDivision;
    syncState.density = density;
    syncState.drift = drift;
    syncState.gridEnd = controls.gridEnd;
    syncState.nextBoundaryPpq = boundaryPpq + duration;

    const auto latestOffsetPpq = duration * 0.20 * static_cast<double>(drift);
    const auto latestOffsetSamples = static_cast<std::int64_t>(std::ceil(
        latestOffsetPpq * 60.0 * currentSampleRate / juce::jmax(1.0, bpm)));
    const auto requestedWindow = static_cast<std::int64_t>(std::round(
        juce::jlimit(minimumTimeSeconds, maximumTimeSeconds, controls.timeSeconds)
        * currentSampleRate));
    const auto available = juce::jmin<std::int64_t>(requestedWindow, totalSamplesWritten);
    const auto safety = static_cast<std::int64_t>(std::round(baseSafetyMarginSeconds
                                                             * currentSampleRate));
    const auto safeSpan = available - latestOffsetSamples - safety;

    for (auto lane = 0; lane < maximumLanes; ++lane)
    {
        auto& state = syncState.lanes[static_cast<size_t>(lane)];
        state = {};
        if (lane >= density || safeSpan <= safety)
            continue;
        const auto rank = density > 1
            ? static_cast<double>(lane) / static_cast<double>(density - 1) : 0.0;
        state.offsetPpq = latestOffsetPpq * rank;
        state.onsetPpq = boundaryPpq + state.offsetPpq;
        const auto tideWidth = 0.015 + 0.535 * smoothStep(juce::jlimit(0.0f, 1.0f, controls.tide));
        const auto sequence = wrappedUnit(static_cast<float>(
            (static_cast<double>(lane) + 0.5) * goldenRatioConjugate
            + static_cast<double>(syncState.boundaryOrdinal) * 0.38196601125
            + randomUnit(0, 31ULL)));
        const auto delay = static_cast<std::int64_t>(std::round(
            static_cast<double>(safety) + static_cast<double>(safeSpan - safety)
                * (0.12 + tideWidth * sequence)));
        state.sourceAnchorSample = totalSamplesWritten - delay;
        state.anchorValid = state.sourceAnchorSample >= 0;
        state.onsetPending = state.anchorValid;
    }
    ++syncState.boundaryOrdinal;
}

bool GranulatorEngine::beginSyncLane(const int laneIndex,
                                     const Controls& controls,
                                     const double bpm) noexcept
{
    auto& lane = syncState.lanes[static_cast<size_t>(laneIndex)];
    auto& grain = grains[static_cast<size_t>(laneIndex)];
    if (! lane.anchorValid || lane.sourceAnchorSample < totalSamplesWritten - historyLength + 2)
        return false;

    if (grain.active)
    {
        // Every forced cut decays through the per-lane one-millisecond tail
        // rather than truncating the running voice at a nonzero level.
        auto& tail = syncState.cutTails[static_cast<size_t>(laneIndex)];
        if (tail.samplesRemaining <= 0)
            ++syncState.activeCutTails;
        tail.levelLeft = grain.lastOutputLeft;
        tail.levelRight = grain.lastOutputRight;
        tail.samplesTotal = juce::jmax(1,
            static_cast<int>(std::round(currentSampleRate * 0.001)));
        tail.samplesRemaining = tail.samplesTotal;
        grain.active = false;
        --activeGrainCount;
    }
    const auto lifetime = syncState.gridEnd
        ? juce::jmax(1.0e-6, syncState.durationPpq - lane.offsetPpq)
        : syncState.durationPpq;
    const auto delay = static_cast<double>(totalSamplesWritten - lane.sourceAnchorSample);
    grain = {};
    grain.active = true;
    grain.syncVoice = true;
    grain.hasReadableSource = true;
    grain.birthSample = timelineSample;
    grain.eventId = birthOrdinal++;
    grain.lifetimePpq = lifetime;
    grain.lifeProgress = 0.0;
    grain.lengthInSamples = juce::jmax(2, static_cast<int>(std::round(
        lifetime * 60.0 * currentSampleRate / juce::jmax(1.0, bpm))));
    grain.phaseScale = 1.0f / static_cast<float>(grain.lengthInSamples - 1);
    grain.shapeAtBirth = juce::jlimit(0.0f, 1.0f, controls.shape);
    grain.envelopeMassAtBirth = 0.5f
        + grain.shapeAtBirth * (2.0f / juce::MathConstants<float>::pi - 0.5f);
    grain.tideAtBirth = juce::jlimit(0.0f, 1.0f, controls.tide);
    grain.playbackSpeed = 1.0;
    grain.sourceDelaySamples = delay;
    grain.readPosition = static_cast<double>(writePosition) - delay;
    while (grain.readPosition < 0.0)
        grain.readPosition += static_cast<double>(historyLength);
    const auto panIdentity = randomUnit(static_cast<std::int64_t>(syncState.boundaryOrdinal),
                                        static_cast<std::uint64_t>(laneIndex) + 401ULL)
        * 2.0f - 1.0f;
    grain.pan = panIdentity * juce::jlimit(0.0f, 1.0f, controls.spread);
    grain.panAmount = std::sin(juce::MathConstants<float>::halfPi * std::abs(grain.pan));
    ++activeGrainCount;
    return true;
}

void GranulatorEngine::reconcileFreeLanes(const Controls& controls) noexcept
{
    const auto requestedCount = juce::jlimit(
        1, maximumLanes, static_cast<int>(std::round(controls.density)));

    if (requestedCount == freeState.activeLaneCount)
        return;

    const auto nominalLength = requestedLengthInSamples(controls);
    const auto wind = juce::jlimit(0.0f, 1.0f, controls.activity);
    const auto nominalRest = static_cast<double>(nominalLength)
        * 4.0 * static_cast<double>((1.0f - wind) * (1.0f - wind));
    const auto nominalCycle = static_cast<double>(nominalLength) + nominalRest;
    for (auto laneIndex = 0; laneIndex < maximumLanes; ++laneIndex)
    {
        auto& lane = freeState.lanes[static_cast<size_t>(laneIndex)];
        if (laneIndex < requestedCount && ! lane.active)
        {
            lane = {};
            lane.active = true;
            const auto phase = laneIndex == 0 ? 0.0
                : static_cast<double>(wrappedUnit((static_cast<float>(laneIndex) + 0.5f)
                                                   * goldenRatioConjugate));
            lane.nextOnsetSample = static_cast<double>(timelineSample) + nominalCycle * phase;
        }
        else if (laneIndex >= requestedCount)
        {
            // Voices already emitted by a removed lane finish naturally.
            lane.active = false;
        }
    }
    freeState.activeLaneCount = requestedCount;
}

void GranulatorEngine::processFreeEvents(const Controls& controls,
                                         const float tide,
                                         const float drift) noexcept
{
    for (auto laneIndex = 0; laneIndex < freeState.activeLaneCount; ++laneIndex)
    {
        auto& lane = freeState.lanes[static_cast<size_t>(laneIndex)];
        if (! lane.active
            || lane.nextOnsetSample > static_cast<double>(timelineSample) + schedulerEpsilon)
            continue;

        // Free lanes are strictly monophonic: lane N permanently owns voice N.
        // Wind only controls the rest after that voice's Size-defined lifetime.
        auto& grain = grains[static_cast<size_t>(laneIndex)];
        if (grain.active)
            continue;

        if (! beginOneShot(grain, laneIndex, controls, tide, drift))
        {
            // History is not readable yet. Retry without consuming the event.
            lane.nextOnsetSample = static_cast<double>(timelineSample + 1);
            continue;
        }
        ++activeGrainCount;
        const auto rest = laneRestSamples(lane,
                                          laneIndex,
                                          grain.lengthInSamples,
                                          controls.activity,
                                          drift);
        lane.nextOnsetSample = static_cast<double>(timelineSample)
            + static_cast<double>(grain.lengthInSamples) + rest;
        ++lane.articulationOrdinal;
    }
}

bool GranulatorEngine::beginOneShot(Grain& grain,
                                    const int laneIndex,
                                    const Controls& controls,
                                    const float tide,
                                    const float drift) noexcept
{
    const auto eventId = birthOrdinal;
    const auto eventStream = eventId * 17ULL;
    const auto clampedDrift = juce::jlimit(0.0f, 1.0f, drift);
    const auto nominalLength = requestedLengthInSamples(controls);
    const auto lifetimeOffset = (randomUnit(timelineSample, eventStream + 1ULL) * 2.0f - 1.0f)
        * maximumLifetimeVariation * clampedDrift;
    const auto minimumLength = juce::jmax(
        2,
        static_cast<int>(std::round(minimumGrainSeconds * currentSampleRate)));
    const auto maximumLength = juce::jmax(
        minimumLength,
        static_cast<int>(std::round(maximumGrainSeconds * currentSampleRate)));
    const auto length = juce::jlimit(
        minimumLength,
        maximumLength,
        static_cast<int>(std::round(static_cast<float>(nominalLength)
                                    * (1.0f + lifetimeOffset))));

    const auto playbackOffset = (randomUnit(timelineSample, eventStream + 2ULL) * 2.0f - 1.0f)
        * maximumSpeedDeviation * clampedDrift;
    const auto playbackSpeed = 1.0 + static_cast<double>(playbackOffset);
    const auto activeWindowSeconds = juce::jlimit(minimumTimeSeconds,
                                                  maximumTimeSeconds,
                                                  controls.timeSeconds);
    const auto activeWindowSamples = juce::jmax<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::round(activeWindowSeconds * currentSampleRate)));
    const auto availableWindow = juce::jmin<std::int64_t>(activeWindowSamples,
                                                          totalSamplesWritten);
    const auto baseSafetySamples = static_cast<double>(std::round(baseSafetyMarginSeconds
                                                                   * currentSampleRate));
    const auto requiredSafety = juce::jmax(baseSafetySamples,
                                           (playbackSpeed - 1.0)
                                               * static_cast<double>(length)
                                               + baseSafetySamples);
    if (availableWindow <= static_cast<std::int64_t>(std::ceil(requiredSafety)))
        return false;

    const auto minimumDelay = static_cast<float>(std::ceil(requiredSafety));
    const auto safeSpan = juce::jmax(
        1.0f,
        static_cast<float>(availableWindow) - minimumDelay);
    const auto elapsedSeconds = static_cast<double>(timelineSample) / currentSampleRate;

    // The centroid is a moving point in source memory, not an amplitude clock.
    // New births sample behind it; existing grains never have their anchor moved.
    const auto centroidFraction = juce::jlimit(
        0.06f,
        0.22f,
        0.12f
            + 0.055f * std::sin(static_cast<float>(elapsedSeconds * 0.37))
            + 0.025f * std::sin(static_cast<float>(elapsedSeconds * 0.113 + 1.7)));
    const auto centroidDelay = minimumDelay + safeSpan * centroidFraction;
    const auto trailRank = wrappedUnit(
        (static_cast<float>(eventId) + 0.5f) * goldenRatioConjugate
        + randomUnit(0, 31ULL));
    const auto trailProportion = minimumTrailProportion
        + (maximumTrailProportion - minimumTrailProportion) * smoothStep(tide);
    const auto trailDelay = safeSpan * trailProportion * trailRank;
    const auto maximumSourceDrift = juce::jmin(
        static_cast<float>(currentSampleRate * maximumSourceDriftSeconds),
        safeSpan * maximumSourceDriftWindowProportion);
    const auto sourceDrift = (randomUnit(timelineSample, eventStream + 3ULL) * 2.0f - 1.0f)
        * maximumSourceDrift * clampedDrift;
    const auto delayInSamples = juce::jlimit(
        static_cast<double>(minimumDelay),
        static_cast<double>(availableWindow),
        static_cast<double>(centroidDelay + trailDelay + sourceDrift));

    grain = {};
    grain.active = true;
    grain.hasReadableSource = true;
    grain.sourceLane = laneIndex;
    grain.birthSample = timelineSample;
    grain.eventId = eventId;
    grain.lengthInSamples = length;
    grain.phaseScale = 1.0f / static_cast<float>(juce::jmax(1, length - 1));
    grain.shapeAtBirth = juce::jlimit(0.0f, 1.0f, controls.shape);
    grain.envelopeMassAtBirth = 0.5f
        + grain.shapeAtBirth * (2.0f / juce::MathConstants<float>::pi - 0.5f);
    grain.tideAtBirth = juce::jlimit(0.0f, 1.0f, tide);
    grain.playbackSpeed = playbackSpeed;
    grain.sourceDelaySamples = delayInSamples;
    grain.readPosition = static_cast<double>(writePosition) - delayInSamples;
    while (grain.readPosition < 0.0)
        grain.readPosition += static_cast<double>(historyLength);

    const auto panIdentity = randomUnit(timelineSample, eventStream + 4ULL) * 2.0f - 1.0f;
    const auto panDrift = (randomUnit(timelineSample, eventStream + 5ULL) * 2.0f - 1.0f)
        * clampedDrift * 0.2f;
    grain.pan = juce::jlimit(-1.0f, 1.0f, panIdentity * 0.8f + panDrift)
        * juce::jlimit(0.0f, 1.0f, controls.spread);
    grain.panAmount = std::sin(juce::MathConstants<float>::halfPi
                              * std::abs(grain.pan));

    ++birthOrdinal;
    return true;
}

double GranulatorEngine::laneRestSamples(FreeState::Lane& lane,
                                         const int laneIndex,
                                         const int grainLengthSamples,
                                         const float wind,
                                         const float drift) noexcept
{
    const auto amount = juce::jlimit(0.0f, 1.0f, drift);
    const auto stream = static_cast<std::uint64_t>(laneIndex) * 4099ULL
        + lane.articulationOrdinal * 17ULL;
    const auto walk = (randomUnit(timelineSample, stream + 701ULL) * 2.0f - 1.0f)
        * laneRateWalkStep * amount;
    lane.rateTendency = juce::jlimit(1.0f - maximumLaneRateSpread * amount,
                                    1.0f + maximumLaneRateSpread * amount,
                                    lane.rateTendency + walk);
    const auto jitter = (randomUnit(timelineSample, stream + 702ULL) * 2.0f - 1.0f)
        * maximumEventTimingJitter * amount;
    // Wind describes space between articulations, relative to Size: calm waits
    // four grain lengths, halfway waits one, and full Wind is back-to-back.
    const auto inverseWind = 1.0f - juce::jlimit(0.0f, 1.0f, wind);
    const auto nominalRest = static_cast<double>(grainLengthSamples)
        * 4.0 * static_cast<double>(inverseWind * inverseWind);
    return juce::jmax(0.0, nominalRest
        * static_cast<double>(lane.rateTendency * (1.0f + jitter)));
}

int GranulatorEngine::requestedLengthInSamples(const Controls& controls) const noexcept
{
    const auto lengthSeconds = juce::jlimit(minimumGrainSeconds,
                                            maximumGrainSeconds,
                                            controls.grainSizeMilliseconds * 0.001f);
    return juce::jmax(2,
                      static_cast<int>(std::round(lengthSeconds * currentSampleRate)));
}

float GranulatorEngine::randomUnit(const std::int64_t eventSample,
                                   const std::uint64_t stream) const noexcept
{
    const auto mixed = hash(randomSeed
                            ^ static_cast<std::uint64_t>(eventSample)
                            ^ (stream * 0x9e3779b97f4a7c15ULL));
    return static_cast<float>((mixed >> 40) & 0xFFFFFFULL) / static_cast<float>(0x1000000ULL);
}

std::uint64_t GranulatorEngine::hash(const std::uint64_t value) noexcept
{
    auto result = value + 0x9e3779b97f4a7c15ULL;
    result = (result ^ (result >> 30)) * 0xbf58476d1ce4e5b9ULL;
    result = (result ^ (result >> 27)) * 0x94d049bb133111ebULL;
    return result ^ (result >> 31);
}

float GranulatorEngine::wrappedUnit(const float value) noexcept
{
    return value - std::floor(value);
}

float GranulatorEngine::roundedSawEnvelope(const float phase) noexcept
{
    constexpr auto attackEnd = 0.08f;
    if (phase < attackEnd)
    {
        const auto attackPhase = juce::jlimit(0.0f, 1.0f, phase / attackEnd);
        return attackPhase * attackPhase * (3.0f - 2.0f * attackPhase);
    }

    const auto decayPhase = juce::jlimit(0.0f, 1.0f, (phase - attackEnd) / (1.0f - attackEnd));
    const auto remaining = 1.0f - decayPhase;
    return remaining * remaining * (3.0f - 2.0f * remaining);
}

float GranulatorEngine::grainEnvelope(const float phase, const float shape) noexcept
{
    const auto clampedPhase = juce::jlimit(0.0f, 1.0f, phase);
    const auto saw = roundedSawEnvelope(clampedPhase);
    const auto sine = halfSine(clampedPhase);
    return saw + (sine - saw) * juce::jlimit(0.0f, 1.0f, shape);
}

float GranulatorEngine::readHistorySample(const float* const channelData,
                                          const double position) const noexcept
{
    auto wrappedPosition = position;
    while (wrappedPosition < 0.0)
        wrappedPosition += static_cast<double>(historyLength);
    while (wrappedPosition >= static_cast<double>(historyLength))
        wrappedPosition -= static_cast<double>(historyLength);

    const auto firstIndex = static_cast<int>(wrappedPosition);
    // firstIndex is in [0, historyLength - 1], so the successor wraps at most
    // once. A conditional subtract avoids an integer divide in the audio loop.
    auto secondIndex = firstIndex + 1;
    if (secondIndex >= historyLength)
        secondIndex = 0;
    const auto fraction = static_cast<float>(wrappedPosition - static_cast<double>(firstIndex));

    const auto first = channelData[firstIndex];
    const auto second = channelData[secondIndex];
    return first + fraction * (second - first);
}
} // namespace tide
