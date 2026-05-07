#pragma once

#include "FeatureExtractor.h"
#include <atomic>
#include <cmath>

namespace dsp {

// Feed-forward peak compressor — one instance per sampler slot.
// Parameters are preset-driven by content type; no user controls.
// Thread model: setPreset() from worker thread (atomics); all other methods audio thread only.
class SlotDynamics
{
public:
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
        threshold_.store(p.tDb,       std::memory_order_relaxed);
        ratioInv_ .store(1.f / p.ratio, std::memory_order_relaxed);
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
        envGainDb_ = 0.f;
    }

    void prepare(double sr) noexcept { sampleRate_ = sr; }

    // Load atomics once per slot before the per-sample inner loop.
    void beginBlock() noexcept
    {
        cActive_   = active_.load(std::memory_order_acquire);
        cThreshDb_ = threshold_.load(std::memory_order_relaxed);
        cRatioInv_ = ratioInv_ .load(std::memory_order_relaxed);
        cAtt_      = attCoeff_ .load(std::memory_order_relaxed);
        cRel_      = relCoeff_ .load(std::memory_order_relaxed);
    }

    // Mono sample — called after sidechain gain, before pan split.
    void processSample(float& s) noexcept
    {
        if (!cActive_) return;
        const float xDb  = 20.f * std::log10(std::max(std::abs(s), 1e-6f));
        const float over = xDb - cThreshDb_;
        const float gDb  = (over > 0.f) ? -over * (1.f - cRatioInv_) : 0.f;
        envGainDb_ = (gDb < envGainDb_)
            ? cAtt_ * envGainDb_ + (1.f - cAtt_) * gDb   // attack
            : cRel_ * envGainDb_ + (1.f - cRel_) * gDb;  // release
        s *= std::pow(10.f, envGainDb_ / 20.f);
    }

    // Store GR meter after the per-sample loop.
    void endBlock() noexcept
    {
        grDb_.store(cActive_ ? envGainDb_ : 0.f, std::memory_order_relaxed);
    }

    // GUI thread: read current gain reduction in dB (0 = no reduction).
    float getGainReductionDb() const noexcept
    {
        return grDb_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool>  active_    { false };
    std::atomic<float> threshold_ { -18.f };
    std::atomic<float> ratioInv_  { 0.333f };
    std::atomic<float> attCoeff_  { 0.995f };
    std::atomic<float> relCoeff_  { 0.9995f };
    std::atomic<float> grDb_      { 0.f };

    double sampleRate_ { 44100.0 };

    // Cached per block — audio thread only.
    bool  cActive_   { false };
    float cThreshDb_ { -18.f };
    float cRatioInv_ { 0.333f };
    float cAtt_      { 0.995f };
    float cRel_      { 0.9995f };
    float envGainDb_ { 0.f };
};

} // namespace dsp
