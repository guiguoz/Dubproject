#pragma once

#include "Colours.h"
#include "dsp/SmartMixEngine.h"

#include <JuceHeader.h>
#include <functional>
#include <cmath>

namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// MagicButton
//
// Hexagonal neon button that triggers the smart mix engine.
//
//   Left click  → onAutoMix()          (reorder + smart defaults)
//   Right click → style preset popup   → onStylePreset(Style)
//
// The button pulses gently when a MusicContext has been loaded
// (bpm > 0 and keyRoot >= 0), and flashes on click.
// ─────────────────────────────────────────────────────────────────────────────
class MagicButton : public juce::Component,
                    private juce::Timer
{
public:
    std::function<void()>                         onAutoMix;
    std::function<void(::dsp::MusicContext::Style)> onStylePreset;

    MagicButton()
    {
        startTimerHz(30); // gentle glow pulse
    }

    ~MagicButton() override { stopTimer(); }

    void setContext(const ::dsp::MusicContext& ctx)
    {
        ctx_        = ctx;
        hasContext_ = (ctx.keyRoot >= 0 && ctx.bpm > 0.f);
        repaint();
    }

    // ── juce::Component ──────────────────────────────────────────────────────

    void paint(juce::Graphics& g) override
    {
        const float W = static_cast<float>(getWidth());
        const float H = static_cast<float>(getHeight());
        const float cx = W * 0.5f;
        const float cy = H * 0.5f;
        const float R  = std::min(W, H) * 0.46f;

        // ── Hexagon path ──────────────────────────────────────────────────────
        juce::Path hex;
        for (int i = 0; i < 6; ++i)
        {
            const float angle = juce::MathConstants<float>::pi / 3.f * static_cast<float>(i)
                                - juce::MathConstants<float>::pi / 6.f;
            const float px = cx + R * std::cos(angle);
            const float py = cy + R * std::sin(angle);
            if (i == 0) hex.startNewSubPath(px, py);
            else        hex.lineTo(px, py);
        }
        hex.closeSubPath();

        // Glow amount: pulse when context is loaded, static when not
        float glowAlpha = 0.12f;
        if (hasContext_)
        {
            // Smooth cosine pulse 0.08 → 0.30
            glowAlpha = 0.08f + 0.22f * (0.5f + 0.5f * std::cos(glowPhase_));
        }
        if (flashAlpha_ > 0.f)
            glowAlpha = std::max(glowAlpha, flashAlpha_);

        // Outer glow rings
        const juce::Colour accent = SaxFXColours::aiBadge; // vivid green
        for (int ring = 3; ring >= 1; --ring)
        {
            const float expand = static_cast<float>(ring) * 4.f;
            g.setColour(accent.withAlpha(glowAlpha / static_cast<float>(ring)));
            juce::Path expanded;
            for (int i = 0; i < 6; ++i)
            {
                const float angle = juce::MathConstants<float>::pi / 3.f * static_cast<float>(i)
                                    - juce::MathConstants<float>::pi / 6.f;
                const float px = cx + (R + expand) * std::cos(angle);
                const float py = cy + (R + expand) * std::sin(angle);
                if (i == 0) expanded.startNewSubPath(px, py);
                else        expanded.lineTo(px, py);
            }
            expanded.closeSubPath();
            g.strokePath(expanded, juce::PathStrokeType(1.5f));
        }

        // Hex body gradient — dark metal
        {
            juce::ColourGradient bg(juce::Colour(0xFF2C2C2C), cx, cy - R,
                                    juce::Colour(0xFF111111), cx, cy + R, false);
            g.setGradientFill(bg);
            g.fillPath(hex);
        }

        // Inner accent wash when context loaded
        if (hasContext_)
        {
            juce::ColourGradient wash(accent.withAlpha(0.18f), cx, cy - R,
                                     accent.withAlpha(0.0f),  cx, cy + R, false);
            g.setGradientFill(wash);
            g.fillPath(hex);
        }

        // Hex border
        g.setColour(hasContext_ ? accent.withAlpha(0.85f) : juce::Colour(0xFF444444));
        g.strokePath(hex, juce::PathStrokeType(1.5f));

        // ── Symbol ─────────────────────────────────────────────────────────────
        // Three-star / sparkle symbol drawn manually
        g.setColour(hasContext_ ? accent : juce::Colour(0xFF888888));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(H * 0.42f).withStyle("Bold")));
        g.drawText(juce::CharPointer_UTF8("\xe2\x9c\xa8"), // ✨
                   getLocalBounds(), juce::Justification::centred);

        // Style indicator dot (bottom)
        if (ctx_.style != ::dsp::MusicContext::Style::None)
        {
            g.setColour(styleColour(ctx_.style));
            g.fillEllipse(cx - 3.f, cy + R * 0.68f, 6.f, 6.f);
        }

        // Flash overlay
        if (flashAlpha_ > 0.f)
        {
            g.setColour(accent.withAlpha(flashAlpha_ * 0.4f));
            g.fillPath(hex);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown())
        {
            showStyleMenu();
        }
        else
        {
            // Flash animation
            flashAlpha_ = 1.0f;
            if (onAutoMix) onAutoMix();
        }
    }

    void resized() override {}

private:
    // ── Timer — pulse + flash decay ───────────────────────────────────────────

    void timerCallback() override
    {
        glowPhase_ += 0.12f; // ~0.6 Hz pulse at 30fps
        if (glowPhase_ > juce::MathConstants<float>::twoPi)
            glowPhase_ -= juce::MathConstants<float>::twoPi;

        if (flashAlpha_ > 0.f)
        {
            flashAlpha_ -= 0.08f;
            if (flashAlpha_ < 0.f) flashAlpha_ = 0.f;
        }

        repaint();
    }

    // ── Style preset popup ─────────────────────────────────────────────────────

    void showStyleMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Jazz");
        menu.addItem(2, "Funk");
        menu.addItem(3, "Rock");
        menu.addItem(4, "Electro");
        menu.addSeparator();
        menu.addItem(5, "Aucun style (defaults uniquement)");

        menu.showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(this),
            [this](int result)
            {
                using Style = ::dsp::MusicContext::Style;
                Style s = Style::None;
                switch (result)
                {
                case 1: s = Style::Jazz;    break;
                case 2: s = Style::Funk;    break;
                case 3: s = Style::Rock;    break;
                case 4: s = Style::Electro; break;
                default: break;
                }
                ctx_.style = s;
                if (onStylePreset) onStylePreset(s);
                flashAlpha_ = 1.0f;
                repaint();
            });
    }

    static juce::Colour styleColour(::dsp::MusicContext::Style s) noexcept
    {
        using Style = ::dsp::MusicContext::Style;
        switch (s)
        {
        case Style::Jazz:    return juce::Colour(0xFF00CCFF); // sky blue
        case Style::Funk:    return juce::Colour(0xFFFF1177); // neon rose
        case Style::Rock:    return juce::Colour(0xFFFF2244); // neon red
        case Style::Electro: return juce::Colour(0xFFCC44FF); // neon violet
        default:             return juce::Colours::transparentBlack;
        }
    }

    ::dsp::MusicContext ctx_;
    bool  hasContext_ = false;
    float glowPhase_  = 0.f;
    float flashAlpha_ = 0.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MagicButton)
};

} // namespace ui
