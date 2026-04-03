#pragma once

#include "AnimatedValue.h"
#include "Colours.h"
#include "SaxFXLayout.h"
#include <JuceHeader.h>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// NeonButton — drop-in replacement for juce::TextButton
//
// Features:
//   - Glow effect (multi-layer concentric fills simulating blur)
//   - Smooth hover animation (150ms cubic-out via AnimatedValue + Timer)
//   - Toggle state: gradient fill + dark text
//   - Per-instance accent colour (setAccentColour)
//
// Thread safety: GUI thread only (standard JUCE component rules).
//
// Performance: Timer runs at 60fps ONLY during hover in/out animation;
//   it stops automatically when the animation completes.
// ─────────────────────────────────────────────────────────────────────────────
class NeonButton : public juce::TextButton, public juce::Timer
{
public:
    explicit NeonButton(const juce::String& text = {})
        : juce::TextButton(text) {}

    void setAccentColour(juce::Colour c) noexcept { accent_ = c; }
    void setGlowEnabled(bool e)          noexcept { glowEnabled_ = e; }
    void setFadeOutMs(int ms)            noexcept { fadeOutMs_ = ms; }

    juce::Colour accentColour() const noexcept { return accent_; }

    // ── Mouse events ──────────────────────────────────────────────────────────
    void mouseEnter(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter(e);
        hoverAnim_.setTarget(1.f, 80);
        startTimerHz(60);
    }

    void mouseExit(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseExit(e);
        hoverAnim_.setTarget(0.f, fadeOutMs_);
        startTimerHz(60);  // always restart — isActive() can be true yet timer stopped
    }

    void timerCallback() override
    {
        hoverAnim_.tick(17);  // ~60fps
        repaint();
        if (!hoverAnim_.isActive())
            stopTimer();
    }

    // ── Painting ──────────────────────────────────────────────────────────────
    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        juce::ignoreUnused(isMouseOver);

        auto         bounds   = getLocalBounds().toFloat().reduced(1.5f);
        const float  cr       = SaxFXLayout::radiusMd;
        const bool   toggled  = getClickingTogglesState() && getToggleState();
        const float  hover    = hoverAnim_.get();
        const float  gAlpha   = (toggled ? 0.80f : hover * 0.50f)
                                + (isButtonDown ? 0.20f : 0.f);

        // ── Glow (multi-pass concentric fills) ────────────────────────────
        if (glowEnabled_ && gAlpha > 0.01f)
        {
            g.setColour(accent_.withAlpha(gAlpha * 0.10f));
            g.fillRoundedRectangle(bounds.expanded(SaxFXLayout::glowSpread), cr + 4.f);
            g.setColour(accent_.withAlpha(gAlpha * 0.20f));
            g.fillRoundedRectangle(bounds.expanded(3.f), cr + 2.f);
            g.setColour(accent_.withAlpha(gAlpha * 0.30f));
            g.fillRoundedRectangle(bounds.expanded(1.5f), cr + 1.f);
        }

        // ── Background ────────────────────────────────────────────────────
        if (toggled || isButtonDown)
        {
            juce::ColourGradient bg(accent_.brighter(0.2f), bounds.getTopLeft(),
                                    accent_.darker(0.3f),   bounds.getBottomRight(), false);
            g.setGradientFill(bg);
        }
        else
        {
            const juce::Colour base = findColour(juce::TextButton::buttonColourId);
            g.setColour(base.interpolatedWith(accent_.withAlpha(0.10f), hover));
        }
        g.fillRoundedRectangle(bounds, cr);

        // ── Border ────────────────────────────────────────────────────────
        if (toggled)
            g.setColour(accent_);
        else if (hover > 0.01f)
            g.setColour(accent_.withAlpha(0.30f + hover * 0.40f));
        else
            g.setColour(accent_.withAlpha(0.20f));

        g.drawRoundedRectangle(bounds, cr,
                               toggled ? SaxFXLayout::borderMedium
                                       : SaxFXLayout::borderThin);

        // ── Text ──────────────────────────────────────────────────────────
        const juce::Colour textOff = findColour(juce::TextButton::textColourOffId);
        const juce::Colour textCol = toggled
            ? SaxFXColours::background
            : textOff.interpolatedWith(SaxFXColours::textPrimary, hover);

        g.setColour(textCol);
        const float fh = std::max(9.f, static_cast<float>(getHeight()) * 0.42f);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(fh).withStyle("Bold")));
        g.drawFittedText(getButtonText(), getLocalBounds(),
                         juce::Justification::centred, 1);
    }

private:
    juce::Colour  accent_      { SaxFXColours::aiBadge };
    bool          glowEnabled_ { true };
    int           fadeOutMs_   { 60 };
    AnimatedValue hoverAnim_   { 0.f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NeonButton)
};

} // namespace ui
