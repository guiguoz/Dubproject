#pragma once

#include "Colours.h"

#include <JuceHeader.h>
#include <functional>
#include <cmath>

namespace ui
{

// ─────────────────────────────────────────────────────────────────────────────
// SamplerMagicButton
//
// Hexagonal neon button (accent or) pour le sampler — même style que MagicButton
// mais dédié au SmartSamplerEngine.
//
//   Clic gauche  → onAutoMix()               (gain staging + key sync)
//   Clic droit   → menu preset (Balanced / Punch / Lo-Fi)
//
// Pulse ambré quand le contexte musical est disponible.
// Affiche une progress bar pendant le traitement (setProgress 0–1).
// ─────────────────────────────────────────────────────────────────────────────
class SamplerMagicButton : public juce::Component,
                           private juce::Timer
{
public:
    enum class Preset { Balanced, Punch, LoFi };

    std::function<void()>          onAutoMix;
    std::function<void(Preset)>    onPreset;

    SamplerMagicButton()
    {
        startTimerHz(30);
    }

    ~SamplerMagicButton() override { stopTimer(); }

    void setContextReady(bool ready)
    {
        hasContext_ = ready;
        repaint();
    }

    /// Call periodically during processing (0 = idle, 0..1 = in progress, 1 = done).
    void setProgress(float p)
    {
        progress_    = juce::jlimit(0.f, 1.f, p);
        processing_  = (progress_ > 0.f && progress_ < 1.f);
        repaint();
    }

    void setIdle()
    {
        processing_ = false;
        progress_   = 0.f;
        repaint();
    }

    // ── Component ─────────────────────────────────────────────────────────────

    void paint(juce::Graphics& g) override
    {
        const float W  = static_cast<float>(getWidth());
        const float H  = static_cast<float>(getHeight());
        const float cx = W * 0.5f;
        const float cy = H * 0.45f;   // slightly above centre (label below)
        const float R  = std::min(W * 0.46f, H * 0.40f);

        const juce::Colour accent = kGold;

        // ── Hexagon path (flat-top orientation) ──────────────────────────────
        auto makeHex = [&](float r) -> juce::Path
        {
            juce::Path hex;
            for (int i = 0; i < 6; ++i)
            {
                // +pi/2 → flat top & flat bottom, no tip artefact at glow rings
                const float angle = juce::MathConstants<float>::pi / 3.f * static_cast<float>(i)
                                    + juce::MathConstants<float>::pi / 2.f;
                const float px = cx + r * std::cos(angle);
                const float py = cy + r * std::sin(angle);
                if (i == 0) hex.startNewSubPath(px, py);
                else        hex.lineTo(px, py);
            }
            hex.closeSubPath();
            return hex;
        };

        const juce::Path hex = makeHex(R);

        // Glow amount
        float glowAlpha = 0.10f;
        if (hasContext_ && !processing_)
            glowAlpha = 0.08f + 0.22f * (0.5f + 0.5f * std::cos(glowPhase_));
        if (processing_)
            glowAlpha = 0.30f;   // bright while working
        if (flashAlpha_ > 0.f)
            glowAlpha = std::max(glowAlpha, flashAlpha_);

        // Outer glow rings
        for (int ring = 3; ring >= 1; --ring)
        {
            const float expand = static_cast<float>(ring) * 4.f;
            g.setColour(accent.withAlpha(glowAlpha / static_cast<float>(ring)));
            g.strokePath(makeHex(R + expand), juce::PathStrokeType(1.5f));
        }

        // Hex body
        {
            juce::ColourGradient bg(juce::Colour(0xFF2C2A1A), cx, cy - R,
                                    juce::Colour(0xFF111008), cx, cy + R, false);
            g.setGradientFill(bg);
            g.fillPath(hex);
        }

        // Inner accent wash when context ready
        if (hasContext_)
        {
            juce::ColourGradient wash(accent.withAlpha(0.18f), cx, cy - R,
                                     accent.withAlpha(0.0f),  cx, cy + R, false);
            g.setGradientFill(wash);
            g.fillPath(hex);
        }

        // Hex border
        g.setColour(hasContext_ ? accent.withAlpha(0.85f) : juce::Colour(0xFF444422));
        g.strokePath(hex, juce::PathStrokeType(1.5f));

        // ── Symbol ─────────────────────────────────────────────────────────────
        // ⚡ lightning bolt — font proportional to hex radius, not component height
        g.setColour(hasContext_ ? accent : accent.withAlpha(0.45f));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(R * 0.95f)));
        g.drawText(juce::CharPointer_UTF8("\xe2\x9a\xa1"), // ⚡
                   juce::Rectangle<int>(0, static_cast<int>(cy - R * 0.55f),
                                        static_cast<int>(W), static_cast<int>(R * 1.1f)),
                   juce::Justification::centred);

        // ── Progress arc (processing indicator) ───────────────────────────────
        if (processing_ && progress_ > 0.f)
        {
            const float startAngle = -juce::MathConstants<float>::pi * 0.5f;
            const float endAngle   = startAngle + juce::MathConstants<float>::twoPi * progress_;
            juce::Path arc;
            arc.addArc(cx - R + 3.f, cy - R + 3.f,
                       (R - 3.f) * 2.f, (R - 3.f) * 2.f,
                       startAngle, endAngle, true);
            g.setColour(accent.withAlpha(0.85f));
            g.strokePath(arc, juce::PathStrokeType(2.5f));
        }

        // ── Label below hex ────────────────────────────────────────────────────
        g.setColour(hasContext_ ? accent.withAlpha(0.80f) : juce::Colour(0xFF666644));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.5f).withStyle("Bold")));
        g.drawText(processing_ ? "..." : "AUTO",
                   0, static_cast<int>(H * 0.82f), static_cast<int>(W), 14,
                   juce::Justification::centred);

        // Flash overlay
        if (flashAlpha_ > 0.f)
        {
            g.setColour(accent.withAlpha(flashAlpha_ * 0.4f));
            g.fillPath(hex);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (processing_) return;   // ignore clicks while working

        if (e.mods.isRightButtonDown())
        {
            showPresetMenu();
        }
        else
        {
            flashAlpha_ = 1.0f;
            if (onAutoMix) onAutoMix();
        }
    }

    void resized() override {}

private:
    void timerCallback() override
    {
        glowPhase_ += 0.10f;
        if (glowPhase_ > juce::MathConstants<float>::twoPi)
            glowPhase_ -= juce::MathConstants<float>::twoPi;

        if (flashAlpha_ > 0.f)
        {
            flashAlpha_ -= 0.08f;
            if (flashAlpha_ < 0.f) flashAlpha_ = 0.f;
        }

        repaint();
    }

    void showPresetMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Balanced (recommandé)");
        menu.addItem(2, "Punch (gain staging agressif)");
        menu.addItem(3, "Lo-Fi (gain réduit)");

        menu.showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(this),
            [this](int result)
            {
                if (result == 0) return;
                Preset p = Preset::Balanced;
                if (result == 2) p = Preset::Punch;
                if (result == 3) p = Preset::LoFi;
                flashAlpha_ = 1.0f;
                if (onPreset) onPreset(p);
                if (onAutoMix) onAutoMix();
            });
    }

    inline static const juce::Colour kGold { 0xFFFFCC44 };

    bool  hasContext_ = false;
    bool  processing_ = false;
    float progress_   = 0.f;
    float glowPhase_  = 0.f;
    float flashAlpha_ = 0.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerMagicButton)
};

} // namespace ui
