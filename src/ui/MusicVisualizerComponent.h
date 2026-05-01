#pragma once
#include "Colours.h"
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <algorithm>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// MusicVisualizerComponent
//
// Animated plasma background for StepSequencerPanel.
// Rendered into a 96×48 pixel buffer and upscaled with high-quality resampling
// to give a smooth liquid/abstract look.
//
// Always animated — audio just modulates speed and palette drift.
// Transparent to mouse, z-ordered behind all sampler controls.
// ─────────────────────────────────────────────────────────────────────────────
class MusicVisualizerComponent : public juce::Component
{
public:
    MusicVisualizerComponent()
        : pixelBuf_(juce::Image::ARGB, kPixW, kPixH, false)
    {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
    }

    // GUI thread, 30 Hz
    void update(const float levels[9], float inputRms, float /*bpm*/) noexcept
    {
        const float maxLevel = *std::max_element(levels, levels + 9);
        // Base speed always advances — audio only accelerates it
        t_          += 0.012f + maxLevel * 0.05f + inputRms * 0.02f;
        paletteOff_  = std::fmod(paletteOff_ + 0.004f + maxLevel * 0.008f, 1.f);

        for (int i = 0; i < 9; ++i)
            smoothLevels_[i] += (levels[i] - smoothLevels_[i]) * 0.25f;
        inputRms_ = inputRms;

        renderPixels();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const float W = static_cast<float>(getWidth());
        const float H = static_cast<float>(getHeight());

        // Plasma upscaled with bilinear interpolation — smooth liquid look
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(pixelBuf_,
                    0, 0, getWidth(), getHeight(),
                    0, 0, kPixW, kPixH, false);

        // Per-track radial glow reacting to levels — metaball feel
        for (int t = 0; t < 9; ++t)
        {
            if (smoothLevels_[t] < 0.02f) continue;

            const float cx = (static_cast<float>(t) + 0.5f) / 9.f * W;
            const float cy = H * (0.5f + 0.28f * std::sin(t_ * 0.4f + static_cast<float>(t) * 0.73f));
            const float r  = 15.f + smoothLevels_[t] * 90.f;

            const juce::Colour col = trackColour(t);
            juce::ColourGradient grad(
                col.withAlpha(0.30f * smoothLevels_[t]), cx, cy,
                col.withAlpha(0.f), cx + r, cy, true);
            g.setGradientFill(grad);
            g.fillEllipse(cx - r, cy - r, r * 2.f, r * 2.f);
        }
    }

private:
    void renderPixels()
    {
        juce::Image::BitmapData data(pixelBuf_, juce::Image::BitmapData::readWrite);

        for (int y = 0; y < kPixH; ++y)
        {
            const float py = static_cast<float>(y) / static_cast<float>(kPixH - 1);
            for (int x = 0; x < kPixW; ++x)
            {
                const float px = static_cast<float>(x) / static_cast<float>(kPixW - 1);

                // 4 interference waves at different angles / speeds → liquid plasma
                float v  = std::sin(px * 7.0f + t_);
                v       += std::sin(py * 5.0f - t_ * 0.71f);
                v       += std::sin((px + py) * 4.5f + t_ * 1.13f);
                const float dx = px * 2.f - 1.f;
                const float dy = py * 2.f - 1.f;
                v       += std::sin(std::sqrt(dx * dx + dy * dy) * 6.f - t_ * 0.83f);
                // v ∈ [-4, 4] → normalise + palette offset → [0, 1]
                const float norm = std::fmod((v + 4.f) / 8.f + paletteOff_, 1.f);
                data.setPixelColour(x, y, palette(norm));
            }
        }
    }

    // Dark dub neon palette: mostly black, with cyan → blue → violet accents
    static juce::Colour palette(float t) noexcept
    {
        const float hue = t;                                        // full colour wheel
        const float ss  = t * t * (3.f - 2.f * t);             // smoothstep
        const float val = 0.04f + 0.52f * ss;                   // mostly dark, vivid peaks
        return juce::Colour::fromHSV(hue, 0.90f, val, 1.f);
    }

    static juce::Colour trackColour(int t) noexcept
    {
        static const juce::Colour kColours[9] = {
            juce::Colour { 0xFF4CDFA8 }, juce::Colour { 0xFF06B6D4 },
            juce::Colour { 0xFFC8C7C7 }, juce::Colour { 0xFF8B5CF6 },
            juce::Colour { 0xFFF97316 }, juce::Colour { 0xFFF43F5E },
            juce::Colour { 0xFFEAB308 }, juce::Colour { 0xFF38BDF8 },
            juce::Colour { 0xFFFF6B35 },
        };
        return kColours[t % 9];
    }

    static constexpr int kPixW = 96;
    static constexpr int kPixH = 48;

    juce::Image          pixelBuf_;
    float                t_          { 0.f };
    float                paletteOff_ { 0.f };
    std::array<float, 9> smoothLevels_ {};
    float                inputRms_   { 0.f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MusicVisualizerComponent)
};

} // namespace ui
