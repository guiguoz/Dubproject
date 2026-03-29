#pragma once

#include "EffectChain.h"
#include "IEffect.h"

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// EffectChainOptimizer
//
// One-shot "AI sound engineer" rule engine.
//
// Usage (GUI thread only):
//   1. User clicks [IA] → call optimize(chain, ctx)
//      → saves current params, applies professional rules
//   2. User clicks [↩] or disables IA → call restore(chain)
//      → restores params saved in step 1, exactly
//   3. User may tweak knobs freely after optimize() — optimizer does not
//      re-apply rules until the next explicit optimize() call.
//
// Rules encoded from professional sources (Sound on Sound, iZotope, Sweetwater,
// Strymon, HornFX, Sax on the Web):
//   - Signal chain order suggestion
//   - Per-effect calibrated parameters (reverb, delay, flanger, harmonizer…)
//   - Interactions (flanger depth ↓ when harmonizer active, AutoPitch ↓ when
//     harmonizer active, octaver gain compensation, delay + reverb interaction)
//   - BPM-synced delay time and slicer rate
//   - Sensitivity calibrated to actual RMS level
//   - Per-sax-type rules (soprano/alto/tenor/baritone)
//
// Pure C++ — no JUCE dependency.
// ─────────────────────────────────────────────────────────────────────────────
class EffectChainOptimizer
{
public:
    // Context provided by DspPipeline / UI at the moment of optimization
    struct Context
    {
        float bpm      { 0.0f };   // detected BPM (0 = unknown)
        float keyRoot  { -1.0f };  // 0=C … 11=B, -1 = unknown
        float rmsLevel { 0.0f };   // current smoothed RMS [0..1]
        int   saxType  { 2 };      // 0=soprano, 1=alto, 2=tenor, 3=baritone
    };

    /// Apply professional rules to chain.  Saves current state first.
    /// GUI thread only.
    void optimize(EffectChain& chain, const Context& ctx) noexcept;

    /// Restore the param state saved before the last optimize() call.
    /// GUI thread only.
    void restore(EffectChain& chain) noexcept;

    bool isActive() const noexcept { return active_; }

    static constexpr int kMaxEffects = EffectChain::kMaxEffects;
    static constexpr int kMaxParams  = 8;

private:
    bool active_ { false };

    // ── Snapshot ─────────────────────────────────────────────────────────────
    struct EffectSnapshot
    {
        float params[kMaxParams] {};
        bool  enabled { true };
        int   paramCount { 0 };
    };
    struct ChainSnapshot
    {
        EffectSnapshot effects[kMaxEffects] {};
        int count { 0 };
    };
    ChainSnapshot saved_;

    // ── Private helpers ───────────────────────────────────────────────────────
    void saveState      (EffectChain& chain) noexcept;
    void applyReverb    (IEffect& fx, bool delayPresent, int saxType) noexcept;
    void applyDelay     (IEffect& fx, float bpm) noexcept;
    void applyFlanger   (IEffect& fx, bool harmonizerPresent) noexcept;
    void applyHarmonizer(IEffect& fx, bool octaverPresent) noexcept;
    void applyEnvFilter (IEffect& fx, float rmsLevel, bool octaverSubPresent) noexcept;
    void applyOctaver   (IEffect& fx, bool harmonizerPresent) noexcept;
    void applyAutoPitch (IEffect& fx, bool harmonizerPresent) noexcept;
    void applySlicer    (IEffect& fx, float bpm) noexcept;

    // Returns true if any effect of the given type is present and enabled
    static bool hasActiveType(EffectChain& chain,
                              EffectType t) noexcept;
};

} // namespace dsp
