#pragma once

#include "Colours.h"

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// SpatialVisualization
//
// 2D stereo field display for the 8 sampler slots + live saxophone.
//
// X axis : pan  (−1.0 = full left … +1.0 = full right)
// Y axis : depth (0.0 = front … 1.0 = back)
//
// Each slot is drawn as a filled circle:
//   • Radius  = 7 + width * 18  (px)
//   • Colour  = content-type accent colour (passed from SmartSamplerEngine)
//   • Dimmed  when slot is not active (not loaded or not playing)
//
// The live saxophone is always drawn as a red dot at (pan ≈ +0.2, depth = 0).
//
// Thread safety: setSlotState() and setSaxActive() are called from the
// GUI thread (message thread) via juce::MessageManager::callAsync.
// paint() is also called from the GUI thread. No audio-thread access.
// ─────────────────────────────────────────────────────────────────────────────
class SpatialVisualization : public juce::Component
{
public:
    // ── Per-slot state ────────────────────────────────────────────────────────
    struct SlotState
    {
        float        pan   { 0.f };
        float        width { 0.f };
        float        depth { 0.f };
        bool         active{ false };
        juce::Colour colour{ juce::Colour(0xFF4CDFA8) };  // SAX-OS neon green default
    };

    // Update state for one slot (0-7). Call from the GUI thread only.
    void setSlotState(int slot, float pan, float width, float depth,
                      bool active, juce::Colour colour)
    {
        if (slot < 0 || slot >= 9) return;
        slots_[static_cast<std::size_t>(slot)] = { pan, width, depth, active, colour };
        repaint();
    }

    // Show/hide the live saxophone marker.
    void setSaxActive(bool active)
    {
        saxActive_ = active;
        repaint();
    }

    // Reset all slots to centred/inactive defaults.
    void resetAll()
    {
        for (auto& s : slots_) s = {};
        saxActive_ = false;
        repaint();
    }

    // ── JUCE Component ────────────────────────────────────────────────────────
    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float w = bounds.getWidth();
        const float h = bounds.getHeight();

        // Background
        g.fillAll(SaxFXColours::background);

        // Grid lines
        g.setColour(SaxFXColours::cardBorder);
        // Centre vertical (pan = 0)
        g.drawLine(w * 0.5f, 0.f, w * 0.5f, h, 1.f);
        // Front line (depth = 0 = top)
        g.drawLine(0.f, 4.f, w, 4.f, 1.f);

        // L / R labels
        g.setColour(SaxFXColours::textSecondary);
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(10.f)));
        g.drawText("L", juce::Rectangle<float>(2.f, 2.f, 14.f, 12.f),
                   juce::Justification::centred, false);
        g.drawText("R", juce::Rectangle<float>(w - 16.f, 2.f, 14.f, 12.f),
                   juce::Justification::centred, false);

        // ── Draw slot circles (back-to-front by depth so front slots are on top)
        // Sort indices by descending depth (paint deepest first)
        int order[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
        std::sort(order, order + 9, [this](int a, int b) {
            return slots_[static_cast<std::size_t>(a)].depth
                 > slots_[static_cast<std::size_t>(b)].depth;
        });

        for (int idx : order)
        {
            const auto& s = slots_[static_cast<std::size_t>(idx)];
            const float cx = panToX(s.pan, w);
            const float cy = depthToY(s.depth, h);
            const float r  = 7.f + s.width * 18.f;

            juce::Colour c = s.colour;
            if (!s.active) c = c.withAlpha(0.25f);

            // Glow ring for wide/active slots
            if (s.active && s.width > 0.2f)
            {
                g.setColour(c.withAlpha(0.15f));
                g.fillEllipse(cx - r - 4.f, cy - r - 4.f,
                              (r + 4.f) * 2.f, (r + 4.f) * 2.f);
            }

            g.setColour(c);
            g.fillEllipse(cx - r, cy - r, r * 2.f, r * 2.f);

            // Slot number label
            g.setColour(SaxFXColours::background.withAlpha(0.85f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(9.f).withStyle("Bold")));
            g.drawText(juce::String(idx + 1),
                       juce::Rectangle<float>(cx - r, cy - r, r * 2.f, r * 2.f),
                       juce::Justification::centred, false);
        }

        // ── Saxophone marker (red dot, always in front)
        if (saxActive_)
        {
            constexpr float kSaxPan   = 0.2f;   // slightly right
            constexpr float kSaxDepth = 0.0f;   // always front
            const float cx = panToX(kSaxPan, w);
            const float cy = depthToY(kSaxDepth, h) + 6.f; // slightly below front line
            constexpr float kR = 6.f;

            g.setColour(juce::Colour(0xFFFF4444).withAlpha(0.3f));
            g.fillEllipse(cx - kR - 3.f, cy - kR - 3.f,
                          (kR + 3.f) * 2.f, (kR + 3.f) * 2.f);
            g.setColour(juce::Colour(0xFFFF4444));
            g.fillEllipse(cx - kR, cy - kR, kR * 2.f, kR * 2.f);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions{}.withHeight(8.f).withStyle("Bold")));
            g.drawText("SAX",
                       juce::Rectangle<float>(cx - kR, cy - kR, kR * 2.f, kR * 2.f),
                       juce::Justification::centred, false);
        }

        // Border
        g.setColour(SaxFXColours::cardBorder);
        g.drawRect(bounds, 1.f);
    }

private:
    std::array<SlotState, 9> slots_ {};
    bool                     saxActive_ { false };

    static float panToX(float pan, float w) noexcept
    {
        return w * 0.5f + std::clamp(pan, -1.f, 1.f) * (w * 0.46f);
    }

    static float depthToY(float depth, float h) noexcept
    {
        // depth 0 = top (front), depth 1 = bottom (back)
        return 12.f + std::clamp(depth, 0.f, 1.f) * (h - 16.f);
    }
};

} // namespace ui
