#pragma once

#include <JuceHeader.h>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SaxFXLookAndFeel
//
// Dark neon LookAndFeel_V4 for the SaxFX Live UI.
// Overrides:
//   - drawRotarySlider : dark ring + accent-coloured value arc + pointer dot
//   - drawButtonBackground / drawButtonText : flat dark rounded buttons
//   - drawPopupMenuBackground / drawPopupMenuItem : dark popup with accent ticks
//
// Each RotaryKnob owns its own instance of this LookAndFeel so that the accent
// colour (set via setAccentColour()) is per-knob rather than global.
// ─────────────────────────────────────────────────────────────────────────────
class SaxFXLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SaxFXLookAndFeel();

    // ── Knob ──────────────────────────────────────────────────────────────────
    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override;

    // ── Buttons ───────────────────────────────────────────────────────────────
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool isHighlighted,
                              bool isButtonDown) override;

    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool isHighlighted,
                        bool isButtonDown) override;

    // ── Popup menus ───────────────────────────────────────────────────────────
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;

    void drawPopupMenuItem(juce::Graphics& g,
                           const juce::Rectangle<int>& area,
                           bool isSeparator,
                           bool isActive,
                           bool isHighlighted,
                           bool isTicked,
                           bool hasSubMenu,
                           const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColour) override;

    // ── Accent colour (per-instance) ──────────────────────────────────────────
    void         setAccentColour(juce::Colour c) noexcept { accent_ = c; }
    juce::Colour accentColour()              const noexcept { return accent_; }

private:
    juce::Colour accent_ { 0xFF1ABC9C }; // default teal
};

} // namespace ui
