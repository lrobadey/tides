#include "PluginEditor.h"

#include "ParameterIDs.h"

#include <cmath>

namespace
{
const auto ink = juce::Colour::fromRGB(5, 18, 24);
const auto deepWater = juce::Colour::fromRGB(8, 37, 47);
const auto midWater = juce::Colour::fromRGB(13, 70, 78);
const auto tideBlue = juce::Colour::fromRGB(74, 205, 196);
const auto foam = juce::Colour::fromRGB(226, 246, 239);
const auto coral = juce::Colour::fromRGB(255, 119, 93);
const auto muted = juce::Colour::fromRGB(126, 161, 160);

juce::Font font(const float height, const bool bold = false)
{
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

void drawTrackedText(juce::Graphics& g,
                     const juce::String& text,
                     const juce::Rectangle<float> area,
                     const float characterSpacing,
                     const juce::Justification justification)
{
    auto type = font(10.0f, true);
    type.setExtraKerningFactor(characterSpacing);
    g.setFont(type);
    g.drawText(text, area, justification, false);
}

// Frosted "glass" card: translucent fill, top sheen, and a light-catching
// border that fades from bright (top-left) to dark (bottom-right).
void drawGlassPanel(juce::Graphics& g,
                    const juce::Rectangle<float> bounds,
                    const float cornerRadius,
                    const float strength = 1.0f)
{
    g.setColour(juce::Colours::black.withAlpha(0.22f * strength));
    g.fillRoundedRectangle(bounds.translated(0.0f, 3.0f), cornerRadius);

    juce::ColourGradient fill(foam.withAlpha(0.065f * strength),
                              bounds.getX(), bounds.getY(),
                              midWater.withAlpha(0.10f * strength),
                              bounds.getX(), bounds.getBottom(),
                              false);
    fill.addColour(0.35, foam.withAlpha(0.028f * strength));
    g.setGradientFill(fill);
    g.fillRoundedRectangle(bounds, cornerRadius);

    juce::Path sheen;
    sheen.addRoundedRectangle(bounds.withHeight(bounds.getHeight() * 0.42f),
                              cornerRadius);
    juce::ColourGradient sheenFill(foam.withAlpha(0.05f * strength),
                                   bounds.getX(), bounds.getY(),
                                   foam.withAlpha(0.0f),
                                   bounds.getX(),
                                   bounds.getY() + bounds.getHeight() * 0.42f,
                                   false);
    g.setGradientFill(sheenFill);
    g.fillPath(sheen);

    juce::ColourGradient edge(foam.withAlpha(0.30f * strength),
                              bounds.getX(), bounds.getY(),
                              tideBlue.withAlpha(0.06f * strength),
                              bounds.getRight(), bounds.getBottom(),
                              false);
    g.setGradientFill(edge);
    g.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);
}

// Soft radial "bioluminescent" glow.
void drawGlow(juce::Graphics& g,
              const juce::Point<float> centre,
              const float radius,
              const juce::Colour colour,
              const float alpha)
{
    juce::ColourGradient glow(colour.withAlpha(alpha), centre.x, centre.y,
                              colour.withAlpha(0.0f),
                              centre.x + radius, centre.y, true);
    g.setGradientFill(glow);
    g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
}

float previewPeakPosition(const float shape, const float envelopePhase) noexcept
{
    auto peakPosition = 0.0f;
    auto peakValue = -1.0f;
    for (auto point = 0; point <= 96; ++point)
    {
        const auto phase = static_cast<float>(point) / 96.0f;
        const auto value = tide::GranulatorEngine::evaluateEnvelope(
            phase, shape, envelopePhase);
        if (value > peakValue)
        {
            peakValue = value;
            peakPosition = phase;
        }
    }
    return peakPosition;
}
} // namespace

TidesLookAndFeel::TidesLookAndFeel()
{
    setColour(juce::Slider::rotarySliderFillColourId, tideBlue);
    setColour(juce::Slider::rotarySliderOutlineColourId, muted.withAlpha(0.28f));
    setColour(juce::Slider::thumbColourId, coral);
}

void TidesLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                        const int x,
                                        const int y,
                                        const int width,
                                        const int height,
                                        const float sliderPosition,
                                        const float rotaryStartAngle,
                                        const float rotaryEndAngle,
                                        juce::Slider& slider)
{
    const auto featured = static_cast<bool>(slider.getProperties()["featured"]);
    const auto glowAmount = static_cast<float>(slider.getProperties()
                                                   .getWithDefault("glow", 0.0));
    const auto available = juce::Rectangle<float>(static_cast<float>(x),
                                                   static_cast<float>(y),
                                                   static_cast<float>(width),
                                                   static_cast<float>(height))
                               .reduced(featured ? 8.0f : 6.0f);
    const auto diameter = juce::jmin(available.getWidth(), available.getHeight());
    const auto bounds = juce::Rectangle<float>(diameter, diameter)
                            .withCentre(available.getCentre());
    const auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = rotaryStartAngle + sliderPosition * (rotaryEndAngle - rotaryStartAngle);
    const auto ringRadius = radius - 5.0f;

    if (glowAmount > 0.01f)
        drawGlow(g, centre, radius * 1.55f, tideBlue, 0.20f * glowAmount);

    g.setColour(juce::Colours::black.withAlpha(0.24f));
    g.fillEllipse(bounds.translated(0.0f, 4.0f));

    juce::ColourGradient face(deepWater.brighter(0.16f),
                              centre.x - radius * 0.35f,
                              centre.y - radius * 0.5f,
                              ink,
                              centre.x + radius * 0.45f,
                              centre.y + radius * 0.55f,
                              true);
    face.addColour(0.62, deepWater);
    g.setGradientFill(face);
    g.fillEllipse(bounds);

    // Glassy specular highlight across the upper-left of the face.
    {
        juce::Path highlight;
        highlight.addPieSegment(bounds.reduced(2.5f),
                                -juce::MathConstants<float>::pi * 0.92f,
                                -juce::MathConstants<float>::pi * 0.18f,
                                0.62f);
        juce::ColourGradient sheen(foam.withAlpha(0.11f + glowAmount * 0.05f),
                                   centre.x - radius * 0.5f,
                                   centre.y - radius * 0.7f,
                                   foam.withAlpha(0.0f),
                                   centre.x,
                                   centre.y + radius * 0.2f,
                                   false);
        g.setGradientFill(sheen);
        g.fillPath(highlight);
    }

    g.setColour(foam.withAlpha((featured ? 0.22f : 0.13f) + glowAmount * 0.14f));
    g.drawEllipse(bounds.reduced(1.0f), featured ? 1.5f : 1.0f);

    for (auto tick = 0; tick <= 14; ++tick)
    {
        const auto tickPosition = static_cast<float>(tick) / 14.0f;
        const auto tickAngle = rotaryStartAngle + tickPosition * (rotaryEndAngle - rotaryStartAngle);
        const auto outer = centre.getPointOnCircumference(ringRadius - 1.0f, tickAngle);
        const auto inner = centre.getPointOnCircumference(ringRadius - (tick % 7 == 0 ? 6.0f : 3.5f), tickAngle);
        g.setColour(foam.withAlpha(tickPosition <= sliderPosition ? 0.35f : 0.11f));
        g.drawLine({ inner, outer }, tick % 7 == 0 ? 1.4f : 0.8f);
    }

    juce::Path inactiveArc;
    inactiveArc.addCentredArc(centre.x,
                              centre.y,
                              ringRadius,
                              ringRadius,
                              0.0f,
                              rotaryStartAngle,
                              rotaryEndAngle,
                              true);
    g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId));
    g.strokePath(inactiveArc, juce::PathStrokeType(featured ? 3.2f : 2.2f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

    if (sliderPosition > 0.001f)
    {
        juce::Path activeArc;
        activeArc.addCentredArc(centre.x,
                                centre.y,
                                ringRadius,
                                ringRadius,
                                0.0f,
                                rotaryStartAngle,
                                angle,
                                true);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId)
                        .brighter(glowAmount * 0.35f));
        g.strokePath(activeArc, juce::PathStrokeType(featured ? 4.2f : 3.0f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    }

    const auto needleStart = centre.getPointOnCircumference(radius * 0.16f, angle);
    const auto needleEnd = centre.getPointOnCircumference(radius * 0.64f, angle);
    g.setColour(foam.withAlpha(0.88f));
    g.drawLine({ needleStart, needleEnd }, featured ? 2.3f : 1.7f);
    g.setColour(coral);
    g.fillEllipse(juce::Rectangle<float>(featured ? 9.0f : 7.0f,
                                         featured ? 9.0f : 7.0f)
                      .withCentre(needleEnd));

    const auto envelopePreview = static_cast<bool>(
        slider.getProperties().getWithDefault("envelopePreview", false));
    if (envelopePreview)
    {
        const auto shape = sliderPosition;
        const auto envelopePhase = static_cast<float>(
            slider.getProperties().getWithDefault("envelopePhase", 0.0));
        const auto graph = juce::Rectangle<float>(radius * 0.82f, radius * 0.48f)
                               .withCentre(centre);
        juce::Path curve;
        juce::Path fill;
        for (auto point = 0; point <= 72; ++point)
        {
            const auto p = static_cast<float>(point) / 72.0f;
            const auto value = tide::GranulatorEngine::evaluateEnvelope(
                p, shape, envelopePhase);
            const juce::Point<float> plotted { graph.getX() + p * graph.getWidth(),
                                               graph.getBottom() - value * graph.getHeight() };
            if (point == 0)
            {
                curve.startNewSubPath(plotted);
                fill.startNewSubPath(graph.getX(), graph.getBottom());
                fill.lineTo(plotted);
            }
            else
            {
                curve.lineTo(plotted);
                fill.lineTo(plotted);
            }
        }
        fill.lineTo(graph.getRight(), graph.getBottom());
        fill.closeSubPath();

        const auto neutralPeak = previewPeakPosition(shape, 0.0f);
        g.setColour(foam.withAlpha(0.12f));
        g.drawHorizontalLine(juce::roundToInt(graph.getBottom()),
                             graph.getX(), graph.getRight());
        g.drawVerticalLine(juce::roundToInt(graph.getX() + neutralPeak * graph.getWidth()),
                           graph.getY(), graph.getBottom());

        juce::ColourGradient envelopeFill(tideBlue.withAlpha(0.18f),
                                          graph.getCentreX(), graph.getY(),
                                          tideBlue.withAlpha(0.01f),
                                          graph.getCentreX(), graph.getBottom(),
                                          false);
        g.setGradientFill(envelopeFill);
        g.fillPath(fill);
        g.setColour(foam.withAlpha(0.86f));
        g.strokePath(curve, juce::PathStrokeType(1.25f,
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        const auto peakPhase = previewPeakPosition(shape, envelopePhase);
        const auto peakPoint = juce::Point<float> {
            graph.getX() + peakPhase * graph.getWidth(),
            graph.getBottom()
                - tide::GranulatorEngine::evaluateEnvelope(
                    peakPhase, shape, envelopePhase) * graph.getHeight()
        };
        g.setColour(coral.withAlpha(0.48f));
        g.drawVerticalLine(juce::roundToInt(peakPoint.x), peakPoint.y, graph.getBottom());
        g.setColour(coral.withAlpha(0.96f));
        g.fillEllipse(juce::Rectangle<float>(3.8f, 3.8f).withCentre(peakPoint));
    }
    else
    {
        g.setColour(tideBlue.withAlpha(0.22f));
        g.fillEllipse(juce::Rectangle<float>(radius * 0.38f, radius * 0.38f).withCentre(centre));
        g.setColour(foam.withAlpha(0.72f));
        g.drawEllipse(juce::Rectangle<float>(radius * 0.24f, radius * 0.24f).withCentre(centre), 1.0f);
    }
}

TideParameter::TideParameter(juce::AudioProcessorValueTreeState& state,
                             const juce::String& parameterID,
                             const juce::String& displayName,
                             const bool featureControl)
    : name(displayName.toUpperCase()), featured(featureControl),
      musicalDivision(parameterID == tide::parameter::syncDivision)
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.2f,
                               juce::MathConstants<float>::pi * 2.8f,
                               true);
    slider.getProperties().set("featured", featured);
    const auto shapeControl = parameterID == tide::parameter::shape;
    slider.getProperties().set("shapeControl", shapeControl);
    slider.getProperties().set("envelopePreview", shapeControl);
    slider.getProperties().set("envelopeShape", 0.5f);
    slider.getProperties().set("envelopePhase", 0.0f);
    slider.setName(displayName);
    slider.setTitle(displayName);
    const auto* parameter = state.getParameter(parameterID);
    jassert(parameter != nullptr);
    slider.setDoubleClickReturnValue(true,
                                     state.getParameterRange(parameterID)
                                         .convertFrom0to1(parameter->getDefaultValue()));
    slider.addListener(this);
    slider.addMouseListener(this, false);
    slider.getProperties().set("glow", 0.0);
    addAndMakeVisible(slider);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state,
                                                                                         parameterID,
                                                                                         slider);
}

TideParameter::~TideParameter()
{
    slider.removeMouseListener(this);
    slider.removeListener(this);
}

void TideParameter::setEnvelopePreview(const float shape, const float phase)
{
    const auto clampedShape = juce::jlimit(0.0f, 1.0f, shape);
    const auto clampedPhase = juce::jlimit(-1.0f, 1.0f, phase);
    const auto previousShape = static_cast<float>(
        slider.getProperties().getWithDefault("envelopeShape", 0.5));
    const auto previousPhase = static_cast<float>(
        slider.getProperties().getWithDefault("envelopePhase", 0.0));
    if (std::abs(previousShape - clampedShape) < 1.0e-6f
        && std::abs(previousPhase - clampedPhase) < 1.0e-6f)
        return;

    slider.getProperties().set("envelopeShape", clampedShape);
    slider.getProperties().set("envelopePhase", clampedPhase);
    slider.repaint();
}

void TideParameter::mouseEnter(const juce::MouseEvent&)
{
    setGlowTarget(slider.isMouseButtonDown() ? 1.0f : 0.55f);
}

void TideParameter::mouseExit(const juce::MouseEvent&)
{
    setGlowTarget(slider.isMouseButtonDown() ? 1.0f : 0.0f);
}

void TideParameter::mouseDown(const juce::MouseEvent&)
{
    setGlowTarget(1.0f);
}

void TideParameter::mouseUp(const juce::MouseEvent&)
{
    setGlowTarget(slider.isMouseOver() ? 0.55f : 0.0f);
}

void TideParameter::setGlowTarget(const float target)
{
    glowTarget = target;
    startTimerHz(60);
}

void TideParameter::timerCallback()
{
    glow += (glowTarget - glow) * 0.22f;
    if (std::abs(glowTarget - glow) < 0.01f)
    {
        glow = glowTarget;
        stopTimer();
    }
    slider.getProperties().set("glow", glow);
    repaint();
}

void TideParameter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    if (featured)
        drawGlassPanel(g, bounds.reduced(4.0f), 12.0f, 0.55f + glow * 0.65f);

    g.setColour(featured ? foam : muted);
    drawTrackedText(g,
                    name,
                    bounds.removeFromTop(featured ? 23.0f : 18.0f),
                    featured ? 0.24f : 0.18f,
                    juce::Justification::centred);

    juce::String value;
    if (name == "TIME")
        value = juce::String(slider.getValue(), slider.getValue() < 1.0 ? 2 : 1) + " s";
    else if (musicalDivision)
    {
        constexpr std::array<const char*, 9> labels {
            "1/32", "1/32.", "1/16", "1/16.", "1/8", "1/8.", "1/4", "1/4.", "1/2"
        };
        value = labels[static_cast<size_t>(juce::jlimit(0, 8,
            static_cast<int>(std::round(slider.getValue()))))];
    }
    else if (name == "SIZE")
        value = juce::String(slider.getValue(), 0) + " ms";
    else if (name == "DENSITY")
        value = juce::String(slider.getValue(), 0) + " lanes";
    else if (name.endsWith("GAIN"))
        value = (slider.getValue() >= 0.05 ? "+" : "")
            + juce::String(slider.getValue(), 1) + " dB";
    else if (name == "PHASE")
    {
        const auto degrees = juce::roundToInt(slider.getValue() * 180.0);
        value = (degrees > 0 ? "+" : "") + juce::String(degrees)
            + juce::String::fromUTF8("\xC2\xB0");
    }
    else
        value = juce::String(juce::roundToInt(slider.getValue() * 100.0)) + "%";

    g.setColour(featured ? foam.withAlpha(0.88f) : foam.withAlpha(0.74f));
    g.setFont(font(featured ? 13.0f : 11.0f, true));
    g.drawText(value,
               getLocalBounds().toFloat().removeFromBottom(featured ? 24.0f : 19.0f),
               juce::Justification::centred,
               false);
}

void TideParameter::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(featured ? 19 : 15);
    area.removeFromBottom(featured ? 21 : 16);
    slider.setBounds(area);
}

void TideParameter::sliderValueChanged(juce::Slider*)
{
    repaint();
}

TideGainBar::TideGainBar(TideGrainsAudioProcessor& owner,
                         const juce::String& parameterID,
                         const juce::String& displayName,
                         const bool inputStage)
    : processor(owner), name(displayName.toUpperCase()), readsInput(inputStage)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setColour(juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    slider.setColour(juce::Slider::trackColourId, juce::Colours::transparentBlack);
    slider.setColour(juce::Slider::thumbColourId, juce::Colours::transparentBlack);
    slider.setName(displayName);
    slider.setTitle(displayName);
    const auto* parameter = owner.parameters.getParameter(parameterID);
    jassert(parameter != nullptr);
    slider.setDoubleClickReturnValue(
        true,
        owner.parameters.getParameterRange(parameterID)
            .convertFrom0to1(parameter->getDefaultValue()));
    slider.addListener(this);
    addAndMakeVisible(slider);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        owner.parameters, parameterID, slider);
    startTimerHz(30);
}

TideGainBar::~TideGainBar()
{
    slider.removeListener(this);
}

void TideGainBar::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto gainValue = static_cast<float>(slider.getValue());
    const auto gainText = (gainValue >= 0.05f ? "+" : "")
        + juce::String(gainValue, 1) + " dB";

    g.setColour(muted);
    drawTrackedText(g,
                    name,
                    bounds.withHeight(16.0f),
                    0.16f,
                    juce::Justification::centredLeft);
    g.setColour(foam.withAlpha(0.86f));
    g.setFont(font(11.0f, true));
    g.drawText(gainText,
               bounds.withHeight(16.0f),
               juce::Justification::centredRight,
               false);

    const auto meter = juce::Rectangle<float>(bounds.getX(),
                                               bounds.getY() + 22.0f,
                                               bounds.getWidth(),
                                               11.0f);
    g.setColour(foam.withAlpha(0.075f));
    g.fillRoundedRectangle(meter, 2.5f);

    const auto levelDb = juce::Decibels::gainToDecibels(displayedLevel, -60.0f);
    const auto levelProportion = juce::jmap(levelDb, -60.0f, 0.0f, 0.0f, 1.0f);
    if (levelProportion > 0.0f)
    {
        const auto levelBounds = meter.withWidth(meter.getWidth() * levelProportion);
        juce::ColourGradient levelGradient(tideBlue.withAlpha(0.78f),
                                            meter.getX(), meter.getCentreY(),
                                            levelDb > -6.0f ? coral : tideBlue,
                                            meter.getRight(), meter.getCentreY(), false);
        g.setGradientFill(levelGradient);
        g.fillRoundedRectangle(levelBounds, 2.5f);
    }

    const auto gainMarker = juce::jmap(gainValue,
                                       static_cast<float>(slider.getMinimum()),
                                       static_cast<float>(slider.getMaximum()),
                                       meter.getX(),
                                       meter.getRight());
    g.setColour(foam.withAlpha(0.9f));
    g.drawVerticalLine(juce::roundToInt(gainMarker),
                       meter.getY() - 3.0f,
                       meter.getBottom() + 3.0f);
    g.setColour(coral);
    g.fillEllipse(gainMarker - 2.5f, meter.getCentreY() - 2.5f, 5.0f, 5.0f);

    const auto statusRow = bounds.withTrimmedTop(37.0f).withHeight(14.0f);
    g.setColour(muted.withAlpha(0.78f));
    drawTrackedText(g,
                    readsInput ? "LEVEL" : "LIMITER",
                    statusRow,
                    0.14f,
                    juce::Justification::centredLeft);
    if (! readsInput)
    {
        const auto activity = juce::jlimit(0.0f, 1.0f, displayedLimiterActivity);
        const auto iconCentre = juce::Point<float>(statusRow.getX() + 65.0f,
                                                   statusRow.getCentreY());
        g.setColour(foam.withAlpha(0.16f + activity * 0.22f));
        g.drawEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(iconCentre), 1.0f);
        g.setColour(coral.withAlpha(0.14f + activity * 0.86f));
        g.fillEllipse(juce::Rectangle<float>(4.0f + activity * 2.0f,
                                             4.0f + activity * 2.0f)
                          .withCentre(iconCentre));
    }
    g.setColour(foam.withAlpha(0.7f));
    g.setFont(font(10.0f, true));
    const auto levelText = displayedLevel > 0.001f
        ? juce::String(levelDb, 1) + " dB"
        : juce::String("-inf dB");
    g.drawText(levelText,
               bounds.withTrimmedTop(37.0f).withHeight(14.0f),
               juce::Justification::centredRight,
               false);
}

void TideGainBar::resized()
{
    slider.setBounds(getLocalBounds().withTrimmedTop(17).withTrimmedBottom(14));
}

void TideGainBar::sliderValueChanged(juce::Slider*)
{
    repaint();
}

void TideGainBar::timerCallback()
{
    const auto measuredLevel = readsInput
        ? processor.getInputLevel()
        : processor.getOutputLevel();
    const auto smoothing = measuredLevel > displayedLevel ? 0.28f : 0.08f;
    displayedLevel += (measuredLevel - displayedLevel) * smoothing;
    if (displayedLevel < 1.0e-5f)
        displayedLevel = 0.0f;
    if (! readsInput)
    {
        const auto limiterActivity = processor.getLimiterActivity();
        const auto limiterSmoothing = limiterActivity > displayedLimiterActivity ? 0.5f : 0.12f;
        displayedLimiterActivity += (limiterActivity - displayedLimiterActivity)
            * limiterSmoothing;
        if (displayedLimiterActivity < 1.0e-4f)
            displayedLimiterActivity = 0.0f;
    }
    repaint();
}

TideToggle::TideToggle(juce::AudioProcessorValueTreeState& state,
                       const juce::String& parameterID,
                       const juce::String& displayName)
    : name(displayName.toUpperCase())
{
    button.setClickingTogglesState(true);
    button.setButtonText({});
    button.setName(displayName);
    button.setTitle(displayName);
    button.setColour(juce::ToggleButton::tickColourId, juce::Colours::transparentBlack);
    button.setColour(juce::ToggleButton::tickDisabledColourId, juce::Colours::transparentBlack);
    button.onClick = [this] { repaint(); };
    addAndMakeVisible(button);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        state, parameterID, button);
}

void TideToggle::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(isEnabled() ? muted : muted.withAlpha(0.35f));
    drawTrackedText(g, name, bounds.removeFromTop(18.0f), 0.18f,
                    juce::Justification::centred);
    const auto track = juce::Rectangle<float>(38.0f, 18.0f)
        .withCentre({ getLocalBounds().toFloat().getCentreX(), bounds.getCentreY() });
    g.setColour((button.getToggleState() ? tideBlue : foam)
                    .withAlpha(isEnabled() ? (button.getToggleState() ? 0.65f : 0.12f) : 0.06f));
    g.fillRoundedRectangle(track, 9.0f);
    const auto knobX = button.getToggleState() ? track.getRight() - 9.0f : track.getX() + 9.0f;
    g.setColour((button.getToggleState() ? foam : muted).withAlpha(isEnabled() ? 0.9f : 0.3f));
    g.fillEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre({ knobX, track.getCentreY() }));
    g.setColour(foam.withAlpha(isEnabled() ? 0.72f : 0.25f));
    g.setFont(font(10.0f, true));
    g.drawText(button.getToggleState() ? "ON" : "OFF", bounds.removeFromBottom(18.0f),
               juce::Justification::centred, false);
}

void TideToggle::resized()
{
    button.setBounds(getLocalBounds());
}

void TideToggle::setTooltip(const juce::String& text)
{
    button.setTooltip(text);
}

SyncSizeControl::SyncSizeControl(juce::AudioProcessorValueTreeState& state)
    : milliseconds(state, tide::parameter::size, "Size"),
      division(state, tide::parameter::syncDivision, "Size")
{
    addAndMakeVisible(milliseconds);
    addChildComponent(division);
}

void SyncSizeControl::resized()
{
    milliseconds.setBounds(getLocalBounds());
    division.setBounds(getLocalBounds());
}

void SyncSizeControl::setSyncMode(const bool enabled)
{
    milliseconds.setVisible(! enabled);
    division.setVisible(enabled);
}

TideField::TideField(TideGrainsAudioProcessor& owner)
    : processor(owner), state(owner.parameters)
{
    setInterceptsMouseClicks(false, false);
    startTimerHz(60);
}

float TideField::parameterValue(const char* parameterID) const
{
    if (const auto* value = state.getRawParameterValue(parameterID))
        return value->load();

    return 0.0f;
}

void TideField::timerCallback()
{
    processor.readGrainFrame(grainFrame);
    waveformInfo = processor.readWaveform(
        waveform, 320, parameterValue(tide::parameter::time));

    tide::GranulatorEngine::VisualFrame newerFrame;
    if (processor.readGrainFrame(newerFrame)
        && newerFrame.sequence != grainFrame.sequence)
    {
        grainFrame = newerFrame;
        waveformInfo = processor.readWaveform(
            waveform, 320, parameterValue(tide::parameter::time));
    }

    auto averageMagnitude = 0.0f;
    auto peakMagnitude = 0.0f;
    for (const auto range : waveform)
    {
        const auto magnitude = juce::jmax(std::abs(range.getStart()),
                                          std::abs(range.getEnd()));
        averageMagnitude += magnitude;
        peakMagnitude = juce::jmax(peakMagnitude, magnitude);
    }

    if (!waveform.empty())
        averageMagnitude /= static_cast<float>(waveform.size());

    const auto energySmoothing = averageMagnitude > signalEnergy ? 0.32f : 0.09f;
    signalEnergy += (averageMagnitude - signalEnergy) * energySmoothing;
    const auto peakSmoothing = peakMagnitude > signalPeak ? 0.55f : 0.12f;
    signalPeak += (peakMagnitude - signalPeak) * peakSmoothing;
    repaint();
}

void TideField::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto tideAmount = parameterValue(tide::parameter::tide);

    juce::Path frame;
    frame.addRoundedRectangle(bounds, 14.0f);
    g.reduceClipRegion(frame);

    juce::ColourGradient water(ink.withAlpha(0.0f),
                               bounds.getCentreX(),
                               bounds.getY(),
                               midWater.withAlpha(0.65f),
                               bounds.getCentreX(),
                               bounds.getBottom(),
                               false);
    water.addColour(0.54, deepWater.withAlpha(0.58f));
    g.setGradientFill(water);
    g.fillRect(bounds);

    g.setColour(foam.withAlpha(0.055f));
    for (auto line = 0; line < 6; ++line)
    {
        const auto y = bounds.getY() + 34.0f + static_cast<float>(line) * bounds.getHeight() / 6.5f;
        g.drawHorizontalLine(juce::roundToInt(y), bounds.getX(), bounds.getRight());
    }

    const auto signalCentre = bounds.getY()
        + bounds.getHeight() * (0.66f - tideAmount * 0.12f);
    const auto maximumSignalHeight = bounds.getHeight() * (0.26f + tideAmount * 0.035f);
    const auto shapedSample = [](const float value)
    {
        return juce::jlimit(-1.0f, 1.0f, value);
    };

    juce::Path upperWake;
    juce::Path lowerWake;
    juce::Path waveformShape;
    if (!waveform.empty())
    {
        const auto lastIndex = waveform.size() - 1;
        for (size_t index = 0; index < waveform.size(); ++index)
        {
            const auto progress = static_cast<float>(index)
                / static_cast<float>(juce::jmax<size_t>(1, lastIndex));
            const auto x = bounds.getX() + progress * bounds.getWidth();
            const auto upper = signalCentre
                - shapedSample(waveform[index].getEnd()) * maximumSignalHeight;
            const auto lower = signalCentre
                - shapedSample(waveform[index].getStart()) * maximumSignalHeight;

            if (index == 0)
            {
                upperWake.startNewSubPath(x, upper);
                lowerWake.startNewSubPath(x, lower);
                waveformShape.startNewSubPath(x, upper);
            }
            else
            {
                upperWake.lineTo(x, upper);
                lowerWake.lineTo(x, lower);
                waveformShape.lineTo(x, upper);
            }
        }

        for (auto index = waveform.size(); index-- > 0;)
        {
            const auto progress = static_cast<float>(index)
                / static_cast<float>(juce::jmax<size_t>(1, lastIndex));
            const auto x = bounds.getX() + progress * bounds.getWidth();
            const auto lower = signalCentre
                - shapedSample(waveform[index].getStart()) * maximumSignalHeight;
            waveformShape.lineTo(x, lower);
        }
        waveformShape.closeSubPath();
    }

    auto wakeShape = waveformShape;
    wakeShape.applyTransform(juce::AffineTransform::scale(1.0f, 0.56f, bounds.getCentreX(), signalCentre)
                                 .translated(0.0f, 34.0f));
    g.setColour(tideBlue.withAlpha(0.07f + signalEnergy * 0.13f));
    g.fillPath(wakeShape);

    juce::ColourGradient waveformFill(tideBlue.withAlpha(0.22f + signalEnergy * 0.3f),
                                      bounds.getCentreX(),
                                      signalCentre - maximumSignalHeight,
                                      midWater.withAlpha(0.2f + signalEnergy * 0.16f),
                                      bounds.getCentreX(),
                                      signalCentre + maximumSignalHeight,
                                      false);
    waveformFill.addColour(0.5, tideBlue.withAlpha(0.09f + signalEnergy * 0.15f));
    g.setGradientFill(waveformFill);
    g.fillPath(waveformShape);
    g.setColour(tideBlue.withAlpha(0.18f));
    g.drawHorizontalLine(juce::roundToInt(signalCentre), bounds.getX(), bounds.getRight());
    g.setColour(foam.withAlpha(0.38f + signalPeak * 0.42f));
    g.strokePath(upperWake,
                 juce::PathStrokeType(1.35f + signalPeak * 1.2f,
                                      juce::PathStrokeType::curved,
                                      juce::PathStrokeType::rounded));
    g.setColour(tideBlue.withAlpha(0.28f + signalEnergy * 0.3f));
    g.strokePath(lowerWake,
                 juce::PathStrokeType(1.0f,
                                      juce::PathStrokeType::curved,
                                      juce::PathStrokeType::rounded));

    std::array<float, tide::GranulatorEngine::maximumGrains> visibleGrainX {};
    auto visibleGrainCount = 0;
    if (grainFrame.generation == waveformInfo.generation)
    {
        for (auto index = 0; index < grainFrame.grainCount; ++index)
        {
            const auto& grain = grainFrame.grains[static_cast<size_t>(index)];
            const auto progress = tide::sourceProgressInWindow(
                grainFrame,
                grain,
                waveformInfo.newestSampleSequence,
                waveformInfo.visibleSampleCount);
            if (progress < 0.0f || progress > 1.0f)
                continue;

            const auto x = bounds.getX() + progress * bounds.getWidth();
            const auto y = signalCentre
                + grain.pan * maximumSignalHeight * 0.72f;
            const auto envelope = juce::jlimit(0.0f, 1.0f, grain.envelope);
            const auto life = juce::jlimit(0.0f, 1.0f, grain.lifeProgress);
            const auto radius = 1.25f + envelope * 3.1f;
            const auto colour = tideBlue.interpolatedWith(
                coral, juce::jlimit(0.0f, 1.0f, grain.pan * 0.5f + 0.5f));

            drawGlow(g, { x, y }, radius * 3.1f, colour, 0.05f + envelope * 0.16f);
            g.setColour(colour.withAlpha(0.2f + envelope * 0.72f));
            g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);

            juce::Path lifeArc;
            lifeArc.addCentredArc(x,
                                  y,
                                  radius + 2.6f,
                                  radius + 2.6f,
                                  0.0f,
                                  -juce::MathConstants<float>::halfPi,
                                  -juce::MathConstants<float>::halfPi
                                      + juce::MathConstants<float>::twoPi * life,
                                  true);
            g.setColour(foam.withAlpha(0.2f + envelope * 0.52f));
            g.strokePath(lifeArc, juce::PathStrokeType(0.9f));

            const auto speedOffset = static_cast<float>(grain.playbackSpeed - 1.0);
            if (std::abs(speedOffset) > 0.0001f)
            {
                const auto direction = speedOffset > 0.0f ? 1.0f : -1.0f;
                g.setColour(colour.withAlpha(0.35f + envelope * 0.4f));
                g.drawLine(x + direction * (radius + 1.5f),
                           y,
                           x + direction * (radius + 4.5f),
                           y,
                           0.8f);
            }

            visibleGrainX[static_cast<size_t>(visibleGrainCount++)] = x;
        }
    }

    if (visibleGrainCount > 0)
    {
        auto centroidX = 0.0f;
        for (auto index = 0; index < visibleGrainCount; ++index)
            centroidX += visibleGrainX[static_cast<size_t>(index)];
        centroidX /= static_cast<float>(visibleGrainCount);

        g.setColour(coral.withAlpha(0.58f));
        g.drawVerticalLine(juce::roundToInt(centroidX),
                           bounds.getY() + 53.0f,
                           bounds.getY() + 68.0f);
    }

    const auto orbitY = bounds.getY() + 31.0f;
    const auto orbitLeft = bounds.getX() + 30.0f;
    const auto orbitRight = bounds.getRight() - 30.0f;
    g.setColour(muted.withAlpha(0.2f));
    g.drawLine(orbitLeft, orbitY, orbitRight, orbitY, 0.8f);
    const auto moonX = juce::jmap(tideAmount, 0.0f, 1.0f, orbitLeft, orbitRight);
    g.setColour(foam.withAlpha(0.92f));
    g.fillEllipse(moonX - 5.5f, orbitY - 5.5f, 11.0f, 11.0f);
    g.setColour(ink.withAlpha(0.72f));
    g.fillEllipse(moonX - 1.0f + tideAmount * 3.0f, orbitY - 5.5f, 11.0f, 11.0f);

    g.setColour(muted.withAlpha(0.76f));
    drawTrackedText(g,
                    "OLDEST / EBB",
                    { orbitLeft, orbitY + 8.0f, 94.0f, 15.0f },
                    0.18f,
                    juce::Justification::centredLeft);
    drawTrackedText(g,
                    "NOW / FLOOD",
                    { orbitRight - 94.0f, orbitY + 8.0f, 94.0f, 15.0f },
                    0.18f,
                    juce::Justification::centredRight);

    const auto inputActive = signalPeak > 0.012f;
    const auto statusWidth = 112.0f;
    const auto statusBounds = juce::Rectangle<float>(statusWidth, 18.0f)
                                  .withCentre({ bounds.getCentreX(), orbitY - 13.0f });
    g.setColour(ink.withAlpha(0.58f));
    g.fillRoundedRectangle(statusBounds, 9.0f);
    g.setColour((inputActive ? tideBlue : muted).withAlpha(inputActive ? 0.88f : 0.5f));
    g.fillEllipse(statusBounds.getX() + 9.0f, statusBounds.getCentreY() - 2.5f, 5.0f, 5.0f);
    drawTrackedText(g,
                    inputActive ? "SOURCE ACTIVE" : "SOURCE QUIET",
                    statusBounds.withTrimmedLeft(18.0f).withTrimmedRight(5.0f),
                    0.12f,
                    juce::Justification::centred);
}

TidesAudioProcessorEditor::TidesAudioProcessorEditor(TideGrainsAudioProcessor& owner)
    : AudioProcessorEditor(owner),
      tideField(owner),
      inputGain(owner, tide::parameter::inputGain, "Input Gain", true),
      outputGain(owner, tide::parameter::outputGain, "Output Gain", false),
      time(owner.parameters, tide::parameter::time, "Time", true),
      tide(owner.parameters, tide::parameter::tide, "Tide", true),
      mix(owner.parameters, tide::parameter::mix, "Mix", true),
      size(owner.parameters),
      density(owner.parameters, tide::parameter::density, "Density"),
      wind(owner.parameters, tide::parameter::wind, "Wind"),
      shape(owner.parameters, tide::parameter::shape, "Shape", true),
      phase(owner.parameters, tide::parameter::envelopePhase, "Phase"),
      spread(owner.parameters, tide::parameter::spread, "Spread"),
      drift(owner.parameters, tide::parameter::drift, "Drift"),
      sync(owner.parameters, tide::parameter::sync, "Sync"),
      gridEnd(owner.parameters, tide::parameter::gridEnd, "Grid End"),
      feedback(owner.parameters, tide::parameter::feedback, "Feedback"),
      lowerControls { &size, &density, &wind, &spread, &drift, &gridEnd, &feedback }
{
    setLookAndFeel(&lookAndFeel);
    setOpaque(true);

    addAndMakeVisible(tideField);
    addAndMakeVisible(inputGain);
    addAndMakeVisible(outputGain);
    addAndMakeVisible(time);
    addAndMakeVisible(tide);
    addAndMakeVisible(mix);
    addAndMakeVisible(sync);
    addAndMakeVisible(shape);
    addAndMakeVisible(phase);
    for (auto* control : lowerControls)
        addAndMakeVisible(*control);

    sync.setTooltip("Sync requires a valid playing host BPM and PPQ clock; no internal clock is used.");
    gridEnd.setTooltip("With Grid End off, Drift layout changes form on the following cycle.");

    applySyncMode(owner.parameters.getRawParameterValue(tide::parameter::sync)->load() >= 0.5f);
    updateEnvelopePreviews();
    startTimerHz(60);

    setResizable(true, true);
    setResizeLimits(780, 520, 1200, 800);
    getConstrainer()->setFixedAspectRatio(1.5);
    setSize(960, 640);
}

void TidesAudioProcessorEditor::applySyncMode(const bool enabled)
{
    syncMode = enabled;
    size.setSyncMode(enabled);
    gridEnd.setEnabled(enabled);
    gridEnd.setAlpha(enabled ? 1.0f : 0.42f);
    wind.setEnabled(! enabled);
    wind.setAlpha(enabled ? 0.42f : 1.0f);
    repaint();
}

void TidesAudioProcessorEditor::timerCallback()
{
    auto& processor = static_cast<TideGrainsAudioProcessor&>(*getAudioProcessor());
    const auto enabled = processor.parameters.getRawParameterValue(tide::parameter::sync)->load() >= 0.5f;
    if (enabled != syncMode)
        applySyncMode(enabled);
    else
        gridEnd.repaint();
    updateEnvelopePreviews();
    sync.repaint();
}

void TidesAudioProcessorEditor::updateEnvelopePreviews()
{
    auto& processor = static_cast<TideGrainsAudioProcessor&>(*getAudioProcessor());
    const auto shapeValue = processor.parameters
                                .getRawParameterValue(tide::parameter::shape)->load();
    const auto phaseValue = processor.parameters
                                .getRawParameterValue(tide::parameter::envelopePhase)->load();
    shape.setEnvelopePreview(shapeValue, phaseValue);
}

TidesAudioProcessorEditor::~TidesAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void TidesAudioProcessorEditor::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto width = bounds.getWidth();
    const auto height = bounds.getHeight();
    g.fillAll(ink);

    juce::ColourGradient ambient(tideBlue.withAlpha(0.13f),
                                 bounds.getWidth() * 0.52f,
                                 bounds.getHeight() * 0.18f,
                                 ink.withAlpha(0.0f),
                                 bounds.getWidth() * 0.12f,
                                 bounds.getHeight() * 0.86f,
                                 true);
    g.setGradientFill(ambient);
    g.fillRect(bounds);

    // Deep-sea bioluminescent pools drifting in the background.
    drawGlow(g, { width * 0.16f, height * 0.92f }, width * 0.30f, tideBlue, 0.07f);
    drawGlow(g, { width * 0.86f, height * 0.30f }, width * 0.24f, midWater.brighter(0.4f), 0.10f);
    drawGlow(g, { width * 0.62f, height * 0.78f }, width * 0.18f, coral, 0.035f);

    const auto scale = width / 960.0f;
    const auto headerHeight = 106.0f * scale;
    const auto lowerTop = height * 0.60f;

    // Light shafts filtering down from the surface.
    for (auto shaft = 0; shaft < 3; ++shaft)
    {
        const auto shaftX = width * (0.30f + static_cast<float>(shaft) * 0.22f);
        juce::Path ray;
        ray.addQuadrilateral(shaftX, 0.0f,
                             shaftX + 26.0f * scale, 0.0f,
                             shaftX + 120.0f * scale, height,
                             shaftX + 58.0f * scale, height);
        juce::ColourGradient rayFill(foam.withAlpha(0.028f), shaftX, 0.0f,
                                     foam.withAlpha(0.0f), shaftX + 60.0f * scale, height,
                                     false);
        g.setGradientFill(rayFill);
        g.fillPath(ray);
    }

    // Glass cards behind the three control zones.
    drawGlassPanel(g,
                   { 22.0f * scale, 12.0f * scale, width - 44.0f * scale, headerHeight - 22.0f * scale },
                   14.0f * scale,
                   0.85f);
    drawGlassPanel(g,
                   { 22.0f * scale, headerHeight, width - 44.0f * scale, lowerTop - headerHeight - 6.0f * scale },
                   14.0f * scale);
    drawGlassPanel(g,
                   { 22.0f * scale, lowerTop + 4.0f * scale, width - 44.0f * scale, height - lowerTop - 16.0f * scale },
                   14.0f * scale,
                   0.85f);

    g.setColour(foam);
    auto brandFont = font(52.0f * scale, true);
    brandFont.setExtraKerningFactor(0.02f);
    g.setFont(brandFont);
    g.drawText("TIDES",
               juce::Rectangle<float>(34.0f * scale, 18.0f * scale, 245.0f * scale, 58.0f * scale),
               juce::Justification::centredLeft,
               false);

    g.setColour(coral);
    g.fillRect(34.0f * scale, 78.0f * scale, 42.0f * scale, 2.0f * scale);
    g.setColour(muted);
    drawTrackedText(g,
                    "GRANULAR TIDAL PROCESSOR",
                    { 86.0f * scale, 69.0f * scale, 224.0f * scale, 19.0f * scale },
                    0.18f,
                    juce::Justification::centredLeft);

    const auto markCentre = juce::Point<float>(width - 52.0f * scale, 47.0f * scale);
    for (auto ring = 0; ring < 3; ++ring)
    {
        const auto radius = (10.0f + static_cast<float>(ring) * 5.5f) * scale;
        g.setColour((ring == 1 ? tideBlue : foam).withAlpha(0.24f + static_cast<float>(ring) * 0.12f));
        g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(markCentre),
                      (ring == 1 ? 1.8f : 0.8f) * scale);
    }
    g.setColour(coral);
    g.fillEllipse(juce::Rectangle<float>(5.0f * scale, 5.0f * scale)
                      .withCentre(markCentre.getPointOnCircumference(15.5f * scale,
                                                                    juce::MathConstants<float>::pi * 0.72f)));

    g.setColour(muted.withAlpha(0.66f));
    drawTrackedText(g,
                    "CURRENT BED  /  MICROSTRUCTURE",
                    { 35.0f * scale, lowerTop + 10.0f * scale, 260.0f * scale, 16.0f * scale },
                    0.18f,
                    juce::Justification::centredLeft);
    drawTrackedText(g,
                    "01 / TIDAL ENGINE",
                    { width - 205.0f * scale, lowerTop + 10.0f * scale, 170.0f * scale, 16.0f * scale },
                    0.18f,
                    juce::Justification::centredRight);

    g.setColour(foam.withAlpha(0.035f));
    for (auto x = 0; x < getWidth(); x += juce::jmax(3, juce::roundToInt(4.0f * scale)))
        g.fillRect(x, getHeight() - 2, 1, 1);
}

void TidesAudioProcessorEditor::resized()
{
    const auto width = static_cast<float>(getWidth());
    const auto height = static_cast<float>(getHeight());
    const auto scale = width / 960.0f;
    const auto headerHeight = 107.0f * scale;
    const auto lowerTop = height * 0.60f;

    const auto gainLeft = 318.0f * scale;
    const auto gainRight = width - 92.0f * scale;
    const auto gainGap = 18.0f * scale;
    const auto gainWidth = (gainRight - gainLeft - gainGap) * 0.5f;
    inputGain.setBounds(juce::roundToInt(gainLeft),
                        juce::roundToInt(25.0f * scale),
                        juce::roundToInt(gainWidth),
                        juce::roundToInt(62.0f * scale));
    outputGain.setBounds(juce::roundToInt(gainLeft + gainWidth + gainGap),
                         juce::roundToInt(25.0f * scale),
                         juce::roundToInt(gainWidth),
                         juce::roundToInt(62.0f * scale));
    sync.setBounds(juce::roundToInt(247.0f * scale),
                   juce::roundToInt(28.0f * scale),
                   juce::roundToInt(64.0f * scale),
                   juce::roundToInt(56.0f * scale));

    tideField.setBounds(juce::roundToInt(22.0f * scale),
                        juce::roundToInt(headerHeight),
                        juce::roundToInt(width - 44.0f * scale),
                        juce::roundToInt(lowerTop - headerHeight));

    const auto featuredTop = headerHeight + 51.0f * scale;
    const auto featuredHeight = lowerTop - featuredTop - 12.0f * scale;
    time.setBounds(juce::roundToInt(72.0f * scale),
                   juce::roundToInt(featuredTop + 16.0f * scale),
                   juce::roundToInt(178.0f * scale),
                   juce::roundToInt(featuredHeight - 18.0f * scale));
    tide.setBounds(juce::roundToInt(width * 0.5f - 111.0f * scale),
                   juce::roundToInt(featuredTop - 4.0f * scale),
                   juce::roundToInt(222.0f * scale),
                   juce::roundToInt(featuredHeight + 8.0f * scale));
    mix.setBounds(juce::roundToInt(width - 250.0f * scale),
                  juce::roundToInt(featuredTop + 16.0f * scale),
                  juce::roundToInt(178.0f * scale),
                  juce::roundToInt(featuredHeight - 18.0f * scale));

    const auto lowerY = lowerTop + 29.0f * scale;
    const auto lowerHeight = height - lowerY - 20.0f * scale;
    const auto availableWidth = width - 62.0f * scale;
    const auto controlWidth = availableWidth / 9.0f;
    constexpr std::array<int, 7> controlColumns { 0, 1, 2, 5, 6, 7, 8 };
    for (size_t index = 0; index < lowerControls.size(); ++index)
        lowerControls[index]->setBounds(
            juce::roundToInt(31.0f * scale
                             + controlWidth * static_cast<float>(controlColumns[index])),
            juce::roundToInt(lowerY),
            juce::roundToInt(controlWidth),
            juce::roundToInt(lowerHeight));

    const auto shapeLeft = 31.0f * scale + controlWidth * 3.0f;
    const auto shapeWidth = controlWidth * 2.0f;
    const auto phaseHeight = juce::jlimit(54.0f * scale,
                                         72.0f * scale,
                                         lowerHeight * 0.32f);
    const auto shapeHeight = lowerHeight - phaseHeight + 8.0f * scale;
    shape.setBounds(juce::roundToInt(shapeLeft),
                    juce::roundToInt(lowerY - 7.0f * scale),
                    juce::roundToInt(shapeWidth),
                    juce::roundToInt(shapeHeight));
    phase.setBounds(juce::roundToInt(shapeLeft + shapeWidth * 0.31f),
                    juce::roundToInt(lowerY + lowerHeight - phaseHeight),
                    juce::roundToInt(shapeWidth * 0.38f),
                    juce::roundToInt(phaseHeight));
}
