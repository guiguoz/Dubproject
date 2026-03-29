#pragma once

#include "SaxFXLookAndFeel.h"

#include <JuceHeader.h>

#include <functional>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// RotaryKnob
//
// 64 × 80 px component:
//   ┌────────────────────────────┐  ← 64 px wide
//   │  rotary slider (64 × 64)  │  270° arc, accent-coloured value arc
//   │      param value label    │  16 px, centred below knob
//   └────────────────────────────┘
//
// - Each knob owns its own SaxFXLookAndFeel instance (per-effect accent colour).
// - An AI-managed badge (small green diamond "◆") is drawn in the top-right
//   corner when setAiManaged(true) is called by EffectChainOptimizer.
// - onValueChange fires whenever the slider is moved by the user.
// ─────────────────────────────────────────────────────────────────────────────
class RotaryKnob : public juce::Component,
                   private juce::Slider::Listener
{
public:
    /// label      : parameter name shown below the knob
    /// accentColour: arc / highlight colour (set per effect type)
    explicit RotaryKnob(const juce::String& label,
                        juce::Colour        accentColour = juce::Colour(0xFF1ABC9C));

    ~RotaryKnob() override;

    // ── Configuration ─────────────────────────────────────────────────────────

    /// Set the parameter range.  defaultVal is the initial value.
    void   setRange(double min, double max, double defaultVal);

    void   setValue(double v, juce::NotificationType = juce::sendNotification);
    double getValue() const;

    /// Change the accent colour (arc + badge highlight).
    void setAccentColour(juce::Colour c);

    /// Show / hide the AI-managed badge "◆" in the top-right corner.
    void setAiManaged(bool managed);
    bool isAiManaged() const noexcept { return aiManaged_; }

    // ── Callback ───────────────────────────────────────────────────────────────
    /// Called with the new value whenever the slider is moved by the user.
    std::function<void(double)> onValueChange;

    // ── juce::Component ────────────────────────────────────────────────────────
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    // juce::Slider::Listener
    void sliderValueChanged(juce::Slider* slider) override;

    SaxFXLookAndFeel laf_;
    juce::Slider     slider_;
    juce::Label      nameLabel_;
    bool             aiManaged_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RotaryKnob)
};

} // namespace ui
