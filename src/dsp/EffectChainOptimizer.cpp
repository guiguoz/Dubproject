#include "EffectChainOptimizer.h"

#include <algorithm>
#include <cmath>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// Param indices — mirrors the constexpr arrays in each *Effect.cpp
// ─────────────────────────────────────────────────────────────────────────────
namespace idx {
    // ReverbEffect  (roomSize, damping, width, mix)
    enum Reverb    { R_roomSize = 0, R_damping, R_width, R_mix };
    // DelayEffect   (time_ms, feedback, mix)
    enum Delay     { D_timeMs = 0, D_feedback, D_mix };
    // FlangerEffect (rate, depth, feedback, mix)
    enum Flanger   { F_rate = 0, F_depth, F_feedback, F_mix };
    // HarmonizerEffect (voice0, voice1, mix)
    enum Harm      { H_voice0 = 0, H_voice1, H_mix };
    // EnvelopeFilterEffect (sensitivity, attack_ms, release_ms, resonance, mix)
    enum Env       { E_sensitivity = 0, E_attackMs, E_releaseMs, E_resonance, E_mix };
    // OctaverEffect (oct1, oct2, dry)
    enum Octaver   { O_oct1 = 0, O_oct2, O_dry };
    // AutoPitchCorrect (strength, refHz)
    enum AutoPitch { A_strength = 0, A_refHz };
    // SlicerEffect  (rate_hz, depth)
    enum Slicer    { S_rateHz = 0, S_depth };
} // namespace idx

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static float clamp(float v, float lo, float hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Choose BPM-synced delay time in ms based on tempo and style heuristic
static float bpmDelayMs(float bpm) noexcept
{
    if (bpm <= 0.0f) return 350.0f;                        // no BPM → 350 ms jazz echo

    const float quarter = 60000.0f / bpm;
    if      (bpm <= 80.0f)  return quarter;                // quarter note
    else if (bpm <= 120.0f) return quarter * 0.75f;        // dotted 8th (funk/pop)
    else                    return quarter * 0.5f;          // 8th note (fast)
}

bool EffectChainOptimizer::hasActiveType(EffectChain& chain, EffectType t) noexcept
{
    for (int i = 0; i < chain.effectCount(); ++i)
    {
        IEffect* fx = chain.getEffect(i);
        if (fx && fx->type() == t && fx->enabled.load(std::memory_order_acquire))
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// State save / restore
// ─────────────────────────────────────────────────────────────────────────────
void EffectChainOptimizer::saveState(EffectChain& chain) noexcept
{
    saved_.count = chain.effectCount();
    for (int i = 0; i < saved_.count; ++i)
    {
        IEffect* fx = chain.getEffect(i);
        EffectSnapshot& snap = saved_.effects[i];
        snap.paramCount = fx ? fx->paramCount() : 0;
        snap.enabled    = fx ? fx->enabled.load(std::memory_order_acquire) : true;
        for (int p = 0; p < snap.paramCount && p < kMaxParams; ++p)
            snap.params[p] = fx ? fx->getParam(p) : 0.0f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-effect rule functions
// ─────────────────────────────────────────────────────────────────────────────

void EffectChainOptimizer::applyReverb(IEffect& fx,
                                        bool delayPresent,
                                        int  saxType) noexcept
{
    // roomSize: concert-hall (0.55) or tighter if delay is washing the mix
    float room = delayPresent ? 0.45f : 0.55f;
    // Soprano sounds nasal in large reverbs → reduce
    if (saxType == 0) room = std::min(room, 0.40f);

    float mix = delayPresent ? 0.20f : 0.22f;   // 15-25% standard; less with delay

    fx.setParam(idx::R_roomSize, room);
    fx.setParam(idx::R_damping,  0.45f);          // damp high freqs naturally
    fx.setParam(idx::R_width,    0.80f);
    fx.setParam(idx::R_mix,      mix);
}

void EffectChainOptimizer::applyDelay(IEffect& fx, float bpm) noexcept
{
    fx.setParam(idx::D_timeMs,   clamp(bpmDelayMs(bpm), 10.0f, 2000.0f));
    fx.setParam(idx::D_feedback, 0.35f);   // ~2 natural repeats
    fx.setParam(idx::D_mix,      0.25f);   // 20-30% standard
}

void EffectChainOptimizer::applyFlanger(IEffect& fx,
                                         bool harmonizerPresent) noexcept
{
    fx.setParam(idx::F_rate,     0.35f);   // slow jet — natural
    // Harmonizer already adds phase modulation → reduce flanger depth
    fx.setParam(idx::F_depth,    harmonizerPresent ? 0.25f : 0.40f);
    fx.setParam(idx::F_feedback, 0.40f);
    fx.setParam(idx::F_mix,      0.30f);
}

void EffectChainOptimizer::applyHarmonizer(IEffect& fx,
                                            bool octaverPresent) noexcept
{
    fx.setParam(idx::H_voice0, 4.0f);     // major third — most consonant
    fx.setParam(idx::H_voice1, 7.0f);     // perfect fifth
    // Reduce mix if octaver is also adding levels
    fx.setParam(idx::H_mix, octaverPresent ? 0.35f : 0.45f);
}

void EffectChainOptimizer::applyEnvFilter(IEffect& fx,
                                           float rmsLevel,
                                           bool  octaverSubPresent) noexcept
{
    // Calibrate sensitivity to actual playing dynamics
    const float sens = clamp(rmsLevel * 8.0f, 0.8f, 7.0f);
    fx.setParam(idx::E_sensitivity, sens);
    fx.setParam(idx::E_attackMs,    8.0f);    // 8ms funk-standard rapid response
    fx.setParam(idx::E_releaseMs,   50.0f);   // short release for tight feel
    // Q ~5 (resonance param range: 0.5..10) — classic funk quack
    fx.setParam(idx::E_resonance,   5.0f);
    fx.setParam(idx::E_mix,         0.70f);

    // If octaver sub is also active, we don't have a cutoffHz param here
    // (EnvFilter starts at its natural frequency) — this is just a note for the UI
    (void)octaverSubPresent;
}

void EffectChainOptimizer::applyOctaver(IEffect& fx,
                                         bool harmonizerPresent) noexcept
{
    // Reduce sub-octave level if harmonizer is adding voices (gain staging)
    fx.setParam(idx::O_oct1, harmonizerPresent ? 0.35f : 0.45f);
    fx.setParam(idx::O_oct2, 0.0f);    // 2-octave sub is too extreme for live use
    fx.setParam(idx::O_dry,  0.70f);
}

void EffectChainOptimizer::applyAutoPitch(IEffect& fx,
                                           bool harmonizerPresent) noexcept
{
    // Harmonizer + full strength AutoPitch = robotic sound (retune speed too fast)
    // Reduce to 0.5 when harmonizer is active — "natural" setting
    fx.setParam(idx::A_strength, harmonizerPresent ? 0.50f : 1.00f);
    // Keep reference A=440 Hz (do not change user's tuning preference)
}

void EffectChainOptimizer::applySlicer(IEffect& fx, float bpm) noexcept
{
    // Sync slicer to detected BPM (16th notes = BPM/60 × 4)
    if (bpm > 0.0f)
        fx.setParam(idx::S_rateHz, clamp(bpm / 60.0f * 4.0f, 0.1f, 20.0f));
    fx.setParam(idx::S_depth, 0.90f);
}

// ─────────────────────────────────────────────────────────────────────────────
// optimize — main entry point
// ─────────────────────────────────────────────────────────────────────────────
void EffectChainOptimizer::optimize(EffectChain& chain,
                                     const Context& ctx) noexcept
{
    saveState(chain);
    active_ = true;

    const bool hasHarm    = hasActiveType(chain, EffectType::Harmonizer);
    const bool hasDelay   = hasActiveType(chain, EffectType::Delay);
    const bool hasOctaver = hasActiveType(chain, EffectType::Octaver);

    for (int i = 0; i < chain.effectCount(); ++i)
    {
        IEffect* fx = chain.getEffect(i);
        if (!fx) continue;

        switch (fx->type())
        {
        case EffectType::Reverb:
            applyReverb(*fx, hasDelay, ctx.saxType);
            break;
        case EffectType::Delay:
            applyDelay(*fx, ctx.bpm);
            break;
        case EffectType::Flanger:
            applyFlanger(*fx, hasHarm);
            break;
        case EffectType::Harmonizer:
            applyHarmonizer(*fx, hasOctaver);
            break;
        case EffectType::EnvelopeFilter:
            applyEnvFilter(*fx, ctx.rmsLevel, hasOctaver);
            break;
        case EffectType::Octaver:
            applyOctaver(*fx, hasHarm);
            break;
        case EffectType::AutoPitchCorrect:
            applyAutoPitch(*fx, hasHarm);
            break;
        case EffectType::Slicer:
            applySlicer(*fx, ctx.bpm);
            break;
        case EffectType::PitchFork:
        case EffectType::Whammy:
        case EffectType::Tuner:
        case EffectType::Synth:
            // No AI rules for these — leave params untouched
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// restore
// ─────────────────────────────────────────────────────────────────────────────
void EffectChainOptimizer::restore(EffectChain& chain) noexcept
{
    if (!active_) return;

    const int count = std::min(saved_.count, chain.effectCount());
    for (int i = 0; i < count; ++i)
    {
        IEffect* fx = chain.getEffect(i);
        if (!fx) continue;

        const EffectSnapshot& snap = saved_.effects[i];
        const int pc = std::min(snap.paramCount, fx->paramCount());
        for (int p = 0; p < pc; ++p)
            fx->setParam(p, snap.params[p]);
        fx->enabled.store(snap.enabled, std::memory_order_release);
    }

    active_ = false;
}

} // namespace dsp
