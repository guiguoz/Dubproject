#pragma once
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <algorithm>
#include <functional>

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// MusicVisualizerComponent
//
// 3D terrain spectrum analyzer — frequency on X, time history on Z (oblique
// projection), amplitude on Y. Renders 48 log-spaced bands × 52 history frames
// via painter's algorithm at 30 Hz. Color: cyan (front) → violet (back).
//
// Data flow: DspPipeline writes audio to a ring buffer; GUI copies 1024 samples
// per frame, computes a 1024-point FFT (Hann windowed), maps to kBands.
//
// Transparent to mouse, z-ordered behind all sampler controls.
// ─────────────────────────────────────────────────────────────────────────────
class MusicVisualizerComponent : public juce::Component
{
public:
    static constexpr int   kFFTOrder  = 10;           // 1024-point FFT
    static constexpr int   kFFTSize   = 1 << kFFTOrder;
    static constexpr int   kBands     = 48;            // frequency bars
    static constexpr int   kHistory   = 52;            // ~1.7 s at 30 Hz
    static constexpr float kObliqueX  = 4.5f;          // px/frame horizontal depth
    static constexpr float kObliqueY  = 2.8f;          // px/frame vertical depth

    MusicVisualizerComponent()
    {
        setInterceptsMouseClicks(false, false);
        setOpaque(false);
        for (int i = 0; i < kFFTSize; ++i)
            hannWin_[i] = 0.5f * (1.f - std::cos(
                juce::MathConstants<float>::twoPi * i / (kFFTSize - 1)));
    }

    void setAudioProvider(std::function<void(float*, int)> fn)
    {
        audioProvider_ = std::move(fn);
    }

    // Called from StepSequencerPanel::timerCallback at 30 Hz (GUI thread)
    void update(const float levels[9], float /*inputRms*/, float /*bpm*/) noexcept
    {
        // 1. Pull audio samples from DspPipeline ring buffer
        float raw[kFFTSize] {};
        if (audioProvider_)
            audioProvider_(raw, kFFTSize);

        // 2. Hann window → FFT buffer (interleaved complex, imag = 0)
        std::fill(fftBuf_.begin(), fftBuf_.end(), 0.f);
        for (int i = 0; i < kFFTSize; ++i)
            fftBuf_[i] = raw[i] * hannWin_[i];

        // 3. In-place FFT → magnitudes in fftBuf_[0..kFFTSize/2-1]
        fft_.performFrequencyOnlyForwardTransform(fftBuf_.data());

        // 4. Log-spaced band mapping 20 Hz – 18 kHz
        constexpr float sr    = 44100.f;
        constexpr float fLow  = 20.f;
        constexpr float fHigh = 18000.f;
        float newBands[kBands] {};
        for (int b = 0; b < kBands; ++b)
        {
            const float t  = static_cast<float>(b) / (kBands - 1);
            const float fc = fLow * std::pow(fHigh / fLow, t);
            const int bin  = std::clamp(
                static_cast<int>(fc / sr * kFFTSize), 1, kFFTSize / 2 - 2);
            const float e  = (fftBuf_[bin - 1] + fftBuf_[bin] + fftBuf_[bin + 1]) / 3.f;
            // dB normalized: –80 dB floor → 0.0, 0 dBFS → ~1.0
            newBands[b] = std::clamp(
                (20.f * std::log10(e / kFFTSize + 1e-4f) + 80.f) / 80.f, 0.f, 1.f);
        }

        // 5. Asymmetric LERP: fast attack, slow decay
        for (int b = 0; b < kBands; ++b)
            smoothBands_[b] = (newBands[b] > smoothBands_[b])
                ? 0.50f * smoothBands_[b] + 0.50f * newBands[b]   // attack
                : 0.92f * smoothBands_[b] + 0.08f * newBands[b];  // decay

        // 6. Push frame into ring history
        historyHead_ = (historyHead_ + 1) % kHistory;
        for (int b = 0; b < kBands; ++b)
            terrain_[historyHead_][b] = smoothBands_[b];

        for (int i = 0; i < 9; ++i)
            smoothLevels_[i] = 0.8f * smoothLevels_[i] + 0.2f * levels[i];

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const float W = static_cast<float>(getWidth());
        const float H = static_cast<float>(getHeight());

        // Deep dark background
        g.setGradientFill(juce::ColourGradient(
            juce::Colour(0xFF0A0012), W * 0.5f, 0.f,
            juce::Colour(0xFF000008), W * 0.5f, H, false));
        g.fillAll();

        const float baseY   = H * 0.88f;
        const float maxBarH = H * 0.70f;
        // Each bin column covers (W + full oblique offset) / kBands
        const float binW    = (W + kObliqueX * kHistory) / kBands;

        // Painter's algorithm: draw from back to front
        for (int z = kHistory - 1; z >= 0; --z)
        {
            const int   fi    = (historyHead_ - z + kHistory * 2) % kHistory;
            const float depth = static_cast<float>(z);
            const float alpha = 0.18f + 0.82f * (1.f - depth / kHistory);

            const float y0 = baseY  - depth * kObliqueY;
            const float x0 = -depth * kObliqueX;

            juce::Path slice;
            slice.startNewSubPath(x0, y0);
            for (int band = 0; band < kBands; ++band)
            {
                const float amp = terrain_[fi][band];
                slice.lineTo(x0 + band * binW,
                             y0 - amp * maxBarH * (0.35f + 0.65f * alpha));
            }
            slice.lineTo(x0 + kBands * binW, y0);
            slice.closeSubPath();

            // Hue: front = cyan (0.55), back = violet (0.73)
            const float hue = 0.55f + 0.18f * (depth / kHistory);
            const juce::Colour col = juce::Colour::fromHSV(hue, 0.85f, 1.0f, alpha * 0.88f);

            // Vertical gradient: bright at crest, near-transparent at base
            const float cx = x0 + kBands * binW * 0.5f;
            g.setGradientFill(juce::ColourGradient(
                col,                          cx, y0 - maxBarH,
                col.withAlpha(0.04f * alpha), cx, y0, false));
            g.fillPath(slice);

            // Thin crest line
            g.setColour(col.withAlpha(alpha * 0.55f));
            g.strokePath(slice, juce::PathStrokeType(0.8f));
        }
    }

private:
    juce::dsp::FFT                  fft_       { kFFTOrder };
    std::array<float, kFFTSize * 2> fftBuf_    {};
    std::array<float, kFFTSize>     hannWin_   {};

    float terrain_[kHistory][kBands] {};
    float smoothBands_[kBands]        {};
    int   historyHead_                { 0 };

    std::function<void(float*, int)> audioProvider_;
    float smoothLevels_[9] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MusicVisualizerComponent)
};

} // namespace ui
