#include "PluginProcessor.h"

#include "ParameterIDs.h"
#include "PluginEditor.h"

#include <cstdlib>

namespace
{
void applySmoothedGain(juce::AudioBuffer<float>& buffer,
                       juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& smoother,
                       const float targetGain) noexcept
{
    smoother.setTargetValue(targetGain);
    for (auto sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto gain = smoother.getNextValue();
        for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.getWritePointer(channel)[sample] *= gain;
    }
}
}

TideGrainsAudioProcessor::TideGrainsAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
    inputGainParameter = parameters.getRawParameterValue(tide::parameter::inputGain);
    timeParameter = parameters.getRawParameterValue(tide::parameter::time);
    tideParameter = parameters.getRawParameterValue(tide::parameter::tide);
    sizeParameter = parameters.getRawParameterValue(tide::parameter::size);
    densityParameter = parameters.getRawParameterValue(tide::parameter::density);
    shapeParameter = parameters.getRawParameterValue(tide::parameter::shape);
    spreadParameter = parameters.getRawParameterValue(tide::parameter::spread);
    driftParameter = parameters.getRawParameterValue(tide::parameter::drift);
    feedbackParameter = parameters.getRawParameterValue(tide::parameter::feedback);
    mixParameter = parameters.getRawParameterValue(tide::parameter::mix);
    outputGainParameter = parameters.getRawParameterValue(tide::parameter::outputGain);

    jassert(inputGainParameter != nullptr);
    jassert(timeParameter != nullptr);
    jassert(tideParameter != nullptr);
    jassert(sizeParameter != nullptr);
    jassert(densityParameter != nullptr);
    jassert(shapeParameter != nullptr);
    jassert(spreadParameter != nullptr);
    jassert(driftParameter != nullptr);
    jassert(feedbackParameter != nullptr);
    jassert(mixParameter != nullptr);
    jassert(outputGainParameter != nullptr);

    constexpr auto defaultSeed = static_cast<juce::int64>(0x4d595df4d0f33173ULL);
    parameters.state.setProperty("randomSeed", defaultSeed, nullptr);
    granulator.setRandomSeed(static_cast<std::uint64_t>(defaultSeed));
}

void TideGrainsAudioProcessor::prepareToPlay(const double sampleRate,
                                             const int samplesPerBlock)
{
    waveformHistory.prepare(sampleRate);
    displayGeneration = 1;
    waveformHistory.reset(displayGeneration);
    internalHistoryTap.resize(static_cast<size_t>(juce::jmax(1, samplesPerBlock)));
    inputGainSmoother.reset(sampleRate, 0.02);
    inputGainSmoother.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(inputGainParameter->load()));
    outputGainSmoother.reset(sampleRate, 0.02);
    outputGainSmoother.setCurrentAndTargetValue(
        juce::Decibels::decibelsToGain(outputGainParameter->load()));
    outputSafetyLimiter.prepare(sampleRate);
    inputLevel.store(0.0f, std::memory_order_relaxed);
    outputLevel.store(0.0f, std::memory_order_relaxed);
    limiterActivity.store(0.0f, std::memory_order_relaxed);
    granulator.prepare(sampleRate,
                       samplesPerBlock,
                       getTotalNumOutputChannels(),
                       mixParameter->load(),
                       feedbackParameter->load(),
                       tideParameter->load(),
                       driftParameter->load());
    granulator.getVisualFrame(grainFrameScratch);
    grainFrameScratch.sequence = ++grainFrameSequence;
    grainFrameScratch.generation = displayGeneration;
    grainSnapshots.publish(grainFrameScratch);
    expectedHostSample.reset();
    hostWasPlaying = false;
}

void TideGrainsAudioProcessor::releaseResources()
{
    granulator.reset();
    ++displayGeneration;
    waveformHistory.reset(displayGeneration);
    granulator.getVisualFrame(grainFrameScratch);
    grainFrameScratch.sequence = ++grainFrameSequence;
    grainFrameScratch.generation = displayGeneration;
    grainSnapshots.publish(grainFrameScratch);
    outputSafetyLimiter.reset();
    limiterActivity.store(0.0f, std::memory_order_relaxed);
}

bool TideGrainsAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    return input == output
        && (output == juce::AudioChannelSet::mono()
            || output == juce::AudioChannelSet::stereo());
}

void TideGrainsAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    for (auto channel = getTotalNumInputChannels();
         channel < getTotalNumOutputChannels();
         ++channel)
    {
        buffer.clear(channel, 0, buffer.getNumSamples());
    }

    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            const auto isPlaying = position->getIsPlaying();
            const auto hostSample = position->getTimeInSamples();
            auto discontinuity = ! hostWasPlaying && isPlaying;
            if (isPlaying && hostSample.hasValue() && expectedHostSample.has_value())
                discontinuity = discontinuity
                    || std::llabs(*hostSample - *expectedHostSample) > 2;

            if (discontinuity)
            {
                granulator.resetHistory(hostSample.orFallback(0));
                ++displayGeneration;
                waveformHistory.reset(displayGeneration);
            }

            // While stopped the host reports a frozen position, which must not
            // register as an endless seek once playback resumes.
            if (isPlaying && hostSample.hasValue())
                expectedHostSample = *hostSample + buffer.getNumSamples();
            else if (! isPlaying)
                expectedHostSample.reset();
            hostWasPlaying = isPlaying;
        }
    }

    const tide::GranulatorEngine::Controls controls {
        timeParameter->load(),
        sizeParameter->load(),
        mixParameter->load(),
        densityParameter->load(),
        shapeParameter->load(),
        spreadParameter->load(),
        feedbackParameter->load(),
        tideParameter->load(),
        driftParameter->load()
    };

    applySmoothedGain(buffer,
                      inputGainSmoother,
                      juce::Decibels::decibelsToGain(inputGainParameter->load()));
    inputLevel.store(buffer.getMagnitude(0, buffer.getNumSamples()),
                     std::memory_order_relaxed);
    const auto canCaptureHistory = internalHistoryTap.size()
        >= static_cast<size_t>(buffer.getNumSamples());
    granulator.process(buffer,
                       controls,
                       canCaptureHistory ? internalHistoryTap.data() : nullptr,
                       canCaptureHistory ? static_cast<int>(internalHistoryTap.size()) : 0);
    if (canCaptureHistory)
        waveformHistory.pushMono(internalHistoryTap.data(), buffer.getNumSamples());

    granulator.getVisualFrame(grainFrameScratch);
    grainFrameScratch.sequence = ++grainFrameSequence;
    grainFrameScratch.generation = displayGeneration;
    grainSnapshots.publish(grainFrameScratch);
    applySmoothedGain(buffer,
                      outputGainSmoother,
                      juce::Decibels::decibelsToGain(outputGainParameter->load()));
    outputSafetyLimiter.process(buffer);
    limiterActivity.store(outputSafetyLimiter.getGainReduction(),
                          std::memory_order_relaxed);
    outputLevel.store(buffer.getMagnitude(0, buffer.getNumSamples()),
                      std::memory_order_relaxed);
}

tide::WaveformHistory::WindowInfo TideGrainsAudioProcessor::readWaveform(
    std::vector<juce::Range<float>>& destination,
    const int columnCount,
    const float seconds) const
{
    return waveformHistory.readWindow(destination, columnCount, seconds);
}

bool TideGrainsAudioProcessor::readGrainFrame(
    tide::GranulatorEngine::VisualFrame& destination) noexcept
{
    return grainSnapshots.readLatest(destination);
}

float TideGrainsAudioProcessor::getInputLevel() const noexcept
{
    return inputLevel.load(std::memory_order_relaxed);
}

float TideGrainsAudioProcessor::getOutputLevel() const noexcept
{
    return outputLevel.load(std::memory_order_relaxed);
}

float TideGrainsAudioProcessor::getLimiterActivity() const noexcept
{
    return limiterActivity.load(std::memory_order_relaxed);
}

juce::AudioProcessorEditor* TideGrainsAudioProcessor::createEditor()
{
    return new TidesAudioProcessorEditor(*this);
}

bool TideGrainsAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String TideGrainsAudioProcessor::getName() const
{
    return "TIDES";
}

bool TideGrainsAudioProcessor::acceptsMidi() const
{
    return false;
}

bool TideGrainsAudioProcessor::producesMidi() const
{
    return false;
}

bool TideGrainsAudioProcessor::isMidiEffect() const
{
    return false;
}

double TideGrainsAudioProcessor::getTailLengthSeconds() const
{
    return 5.0;
}

int TideGrainsAudioProcessor::getNumPrograms()
{
    return 1;
}

int TideGrainsAudioProcessor::getCurrentProgram()
{
    return 0;
}

void TideGrainsAudioProcessor::setCurrentProgram(const int index)
{
    juce::ignoreUnused(index);
}

const juce::String TideGrainsAudioProcessor::getProgramName(const int index)
{
    juce::ignoreUnused(index);
    return {};
}

void TideGrainsAudioProcessor::changeProgramName(const int index,
                                                 const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void TideGrainsAudioProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    if (auto state = parameters.copyState(); state.isValid())
    {
        if (auto xml = state.createXml())
            copyXmlToBinary(*xml, destinationData);
    }
}

void TideGrainsAudioProcessor::setStateInformation(const void* data,
                                                   const int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (xml->hasTagName(parameters.state.getType()))
        {
            parameters.replaceState(juce::ValueTree::fromXml(*xml));

            const auto seed = parameters.state.getProperty("randomSeed");
            if (! seed.isVoid())
                granulator.setRandomSeed(static_cast<std::uint64_t>(static_cast<juce::int64>(seed)));
        }
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout
TideGrainsAudioProcessor::createParameterLayout()
{
    using Parameter = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto timeRange = Range { 0.02f, 5.0f, 0.001f };
    timeRange.setSkewForCentre(0.5f);

    auto sizeRange = Range { 5.0f, 1000.0f, 0.1f };
    sizeRange.setSkewForCentre(40.0f);

    auto densityRange = Range { 1.0f, 64.0f, 1.0f };
    densityRange.setSkewForCentre(16.0f);

    const auto gainRange = Range { -24.0f, 12.0f, 0.1f };

    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::inputGain, 1 }, "Input Gain", gainRange, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::time, 1 }, "Time", timeRange, 1.0f,
        juce::AudioParameterFloatAttributes().withLabel("s")));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::tide, 1 }, "Tide", Range { 0.0f, 1.0f }, 0.0f));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::size, 1 }, "Size", sizeRange, 40.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::density, 1 }, "Density", densityRange, 10.0f,
        juce::AudioParameterFloatAttributes().withLabel("grains")));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::shape, 1 }, "Shape", Range { 0.0f, 1.0f }, 0.5f));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::spread, 1 }, "Spread", Range { 0.0f, 1.0f }, 0.35f));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::drift, 1 }, "Drift", Range { 0.0f, 1.0f }, 0.2f));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::feedback, 1 }, "Feedback", Range { 0.0f, 0.95f }, 0.0f));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::mix, 1 }, "Mix", Range { 0.0f, 1.0f }, 0.5f));
    layout.add(std::make_unique<Parameter>(
        juce::ParameterID { tide::parameter::outputGain, 1 }, "Output Gain", gainRange, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    return layout;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TideGrainsAudioProcessor();
}
