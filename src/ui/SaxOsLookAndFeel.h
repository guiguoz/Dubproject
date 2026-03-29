#pragma once

#include <JuceHeader.h>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SaxOsLookAndFeel
//
// Neon dark LookAndFeel for the SAX-OS / SONIC MONOLITH design language.
// Visual style: near-black surfaces, neon primary (#4CDFa8 green), per-effect
// accent glows, subtle grain texture overlays, rounded minimal shapes.
//
// Inheritance: LookAndFeel_V4
// ─────────────────────────────────────────────────────────────────────────────
class SaxOsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SaxOsLookAndFeel();

    // ── Knobs (RotarySlider) ────────────────────────────────────────────────
    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override;

    // ── Buttons ─────────────────────────────────────────────────────────────
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool isHighlighted,
                              bool isButtonDown) override;

    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool isHighlighted,
                        bool isButtonDown) override;

    // ── Toggle buttons ─────────────────────────────────────────────────────
    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool isMouseOverButton,
                          bool isButtonDown) override;

    // ── Popup menus ────────────────────────────────────────────────────────
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

    // ── Accent colour (per-instance, like SaxFXLookAndFeel) ─────────────────
    void         setAccentColour(juce::Colour c) noexcept { accent_ = c; }
    juce::Colour accentColour()              const noexcept { return accent_; }

private:
    juce::Colour accent_ { 0xFF4CDFA8 }; // primary neon green
};

} // namespace ui
