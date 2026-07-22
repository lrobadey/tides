#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

#include <array>
#include <memory>

class TidesLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    TidesLookAndFeel();

    void drawRotarySlider(juce::Graphics&,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPosition,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider&) override;
};

class TideParameter final : public juce::Component,
                            private juce::Slider::Listener,
                            private juce::Timer
{
public:
    TideParameter(juce::AudioProcessorValueTreeState&,
                  const juce::String& parameterID,
                  const juce::String& displayName,
                  bool featureControl = false);
    ~TideParameter() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void sliderValueChanged(juce::Slider*) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void timerCallback() override;
    void setGlowTarget(float target);

    juce::String name;
    juce::Slider slider;
    bool featured = false;
    float glow = 0.0f;
    float glowTarget = 0.0f;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class TideField final : public juce::Component,
                        private juce::Timer
{
public:
    explicit TideField(TideGrainsAudioProcessor&);
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;
    float parameterValue(const char* parameterID) const;

    TideGrainsAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    float signalEnergy = 0.0f;
    float signalPeak = 0.0f;
    std::vector<juce::Range<float>> waveform;
    tide::WaveformHistory::WindowInfo waveformInfo;
    tide::GranulatorEngine::VisualFrame grainFrame;
};

class TideGainBar final : public juce::Component,
                          private juce::Slider::Listener,
                          private juce::Timer
{
public:
    TideGainBar(TideGrainsAudioProcessor&,
                const juce::String& parameterID,
                const juce::String& displayName,
                bool inputStage);
    ~TideGainBar() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void sliderValueChanged(juce::Slider*) override;
    void timerCallback() override;

    TideGrainsAudioProcessor& processor;
    juce::String name;
    juce::Slider slider;
    bool readsInput = false;
    float displayedLevel = 0.0f;
    float displayedLimiterActivity = 0.0f;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class TidesAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TidesAudioProcessorEditor(TideGrainsAudioProcessor&);
    ~TidesAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    TidesLookAndFeel lookAndFeel;
    TideField tideField;

    TideGainBar inputGain;
    TideGainBar outputGain;
    TideParameter time;
    TideParameter tide;
    TideParameter mix;
    TideParameter size;
    TideParameter density;
    TideParameter shape;
    TideParameter spread;
    TideParameter drift;
    TideParameter feedback;

    std::array<TideParameter*, 6> lowerControls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TidesAudioProcessorEditor)
};
