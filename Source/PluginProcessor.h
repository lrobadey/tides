#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "GranulatorEngine.h"
#include "GrainTelemetry.h"
#include "OutputSafetyLimiter.h"
#include "WaveformHistory.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

class TideGrainsAudioProcessor final : public juce::AudioProcessor
{
public:
    TideGrainsAudioProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    tide::WaveformHistory::WindowInfo readWaveform(
        std::vector<juce::Range<float>>& destination,
        int columnCount,
        float seconds) const;
    bool readGrainFrame(tide::GranulatorEngine::VisualFrame& destination) noexcept;
    float getInputLevel() const noexcept;
    float getOutputLevel() const noexcept;
    float getLimiterActivity() const noexcept;

    juce::AudioProcessorValueTreeState parameters;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    tide::GranulatorEngine granulator;
    tide::GrainSnapshotExchange grainSnapshots;
    tide::OutputSafetyLimiter outputSafetyLimiter;
    tide::WaveformHistory waveformHistory;
    tide::GranulatorEngine::VisualFrame grainFrameScratch;
    std::vector<float> internalHistoryTap;
    std::atomic<float>* inputGainParameter = nullptr;
    std::atomic<float>* timeParameter = nullptr;
    std::atomic<float>* tideParameter = nullptr;
    std::atomic<float>* sizeParameter = nullptr;
    std::atomic<float>* syncParameter = nullptr;
    std::atomic<float>* syncDivisionParameter = nullptr;
    std::atomic<float>* gridEndParameter = nullptr;
    std::atomic<float>* densityParameter = nullptr;
    std::atomic<float>* windParameter = nullptr;
    std::atomic<float>* shapeParameter = nullptr;
    std::atomic<float>* spreadParameter = nullptr;
    std::atomic<float>* driftParameter = nullptr;
    std::atomic<float>* feedbackParameter = nullptr;
    std::atomic<float>* mixParameter = nullptr;
    std::atomic<float>* outputGainParameter = nullptr;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGainSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmoother;
    std::atomic<float> inputLevel { 0.0f };
    std::atomic<float> outputLevel { 0.0f };
    std::atomic<float> limiterActivity { 0.0f };
    std::optional<std::int64_t> expectedHostSample;
    std::uint64_t grainFrameSequence = 0;
    std::uint64_t displayGeneration = 1;
    bool hostWasPlaying = false;
    double lastPlayingBpm = 0.0;
    std::atomic<double> synchronizedTailSeconds { 5.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TideGrainsAudioProcessor)
};
