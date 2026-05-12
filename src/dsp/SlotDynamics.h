#pragma once

#include "FeatureExtractor.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace dsp {

// Feed-forward peak compressor — one instance per sampler slot.
// Parameters are preset-driven by content type; no user controls.
// Thread model: setPreset() from worker thread (atomics); all other methods audio thread only.
class SlotDynamics
{
public:
    SlotDynamics() noexcept : envGain_(1.f) {}

    void setPreset(ContentCategory type) noexcept
    {
        struct Preset { float tDb, ratio, attMs, relMs; };
        static constexpr Preset kPresets[] = {
            /* KICK  */ { -18.f, 3.f,  5.f,  80.f },
            /* SNARE */ { -16.f, 3.f,  5.f, 100.f },
            /* HIHAT */ { -12.f, 2.f,  1.f,  50.f },
            /* BASS  */ { -20.f, 3.f, 10.f, 200.f },
            /* SYNTH */ { -15.f, 2.f, 10.f, 150.f },
            /* PAD   */ { -18.f, 2.f, 30.f, 500.f },
            /* PERC  */ { -15.f, 3.f,  3.f,  80.f },
            /* OTHER */ { -18.f, 2.f,  5.f, 150.f },
        };
        const int idx = std::min(static_cast<int>(type),
                                 static_cast<int>(ContentCategory::OTHER));
        const auto& p = kPresets[idx];
        threshLin_.store(std::pow(10.f, p.tDb / 20.f), std::memory_order_relaxed);
        ratioInv_ .store(1.f / p.ratio,                 std::memory_order_relaxed);
        if (sampleRate_ > 0.0)
        {
            const float sr = static_cast<float>(sampleRate_);
            attCoeff_.store(std::exp(-2.2f / (p.attMs * 0.001f * sr)),
                            std::memory_order_relaxed);
            relCoeff_.store(std::exp(-2.2f / (p.relMs * 0.001f * sr)),
                            std::memory_order_relaxed);
        }
        active_.store(true, std::memory_order_release);
    }

    void reset() noexcept
    {
        active_.store(false, std::memory_order_relaxed);
        envGain_ = 1.f;
    }

    void prepare(double sr) noexcept { sampleRate_ = sr; }

    // Load atomics once per slot before the per-sample inner loop.
    void beginBlock() noexcept
    {
        cActive_      = active_.load(std::memory_order_acquire);
        cThreshLin_   = threshLin_.load(std::memory_order_relaxed);
        cRatioSlope_  = 1.f - ratioInv_.load(std::memory_order_relaxed); // (ratio-1)/ratio > 0
        cAtt_         = attCoeff_.load(std::memory_order_relaxed);
        cRel_         = relCoeff_.load(std::memory_order_relaxed);
    }

    // Mono sample — called after sidechain gain, before pan split.
    void processSample(float& s) noexcept
    {
        if (!cActive_) return;
        const float absS = std::abs(s);
        if (absS < 1e-6f) return;                            // gate -120 dBFS, avoids denormals
        const float over = absS / (cThreshLin_ + 1e-12f);
        const float gr   = (over > 1.f) ? std::pow(over, -cRatioSlope_) : 1.f;
        envGain_ = (gr < envGain_)
            ? cAtt_ * envGain_ + (1.f - cAtt_) * gr         // attack
            : cRel_ * envGain_ + (1.f - cRel_) * gr;        // release
        envGain_ = std::clamp(envGain_, 0.f, 1.f);
        s *= envGain_;
    }

    // Store GR meter after the per-sample loop — one log10 per block, not per sample.
    void endBlock() noexcept
    {
        grDb_.store(cActive_ ? 20.f * std::log10(std::max(envGain_, 1e-6f)) : 0.f,
                    std::memory_order_relaxed);
    }

    // GUI thread: read current gain reduction in dB (0 = no reduction).
    float getGainReductionDb() const noexcept
    {
        return grDb_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool>  active_    { false };
    std::atomic<float> threshLin_ { 0.126f };  // 10^(-18/20)
    std::atomic<float> ratioInv_  { 0.333f };  // 1/ratio
    std::atomic<float> attCoeff_  { 0.995f };
    std::atomic<float> relCoeff_  { 0.9995f };
    std::atomic<float> grDb_      { 0.f };

    double sampleRate_ { 44100.0 };

    // Cached per block — audio thread only.
    bool  cActive_     { false };
    float cThreshLin_  { 0.126f };
    float cRatioSlope_ { 0.667f };  // (ratio-1)/ratio
    float cAtt_        { 0.995f };
    float cRel_        { 0.9995f };
    float envGain_     { 1.f };     // linear gain envelope [0..1]
};

} // namespace dsp
