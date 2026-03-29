#pragma once

#include "EffectChain.h"
#include "IEffect.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// MusicContext
//
// Analysis result from the master sample selector.
// Passed to SmartMixEngine to drive intelligent parameter decisions.
// ─────────────────────────────────────────────────────────────────────────────
struct MusicContext
{
    float bpm     = 120.f;
    int   keyRoot = 0;     // 0=C … 11=B ; -1 = unknown
    bool  isMajor = true;

    enum class Style { None, Jazz, Funk, Rock, Electro };
    Style style = Style::None;
};

// ─────────────────────────────────────────────────────────────────────────────
// SmartMixEngine
//
// Stateless "sound engineer" logic.  Header-only, no JUCE GUI dependency.
//
// Canonical signal chain order for saxophone:
//   Tuner → Whammy → PitchFork → Octaver → Harmonizer →
//   EnvelopeFilter → Flanger → Slicer → Delay → Reverb
//
// Usage (GUI thread):
//   auto order = SmartMixEngine::computeOptimalOrder(chain);
//   SmartMixEngine::reorderChain(chain, order);
//   SmartMixEngine::applySmartDefaults(chain, ctx);
// ─────────────────────────────────────────────────────────────────────────────
class SmartMixEngine
{
public:
    // ── Reordering ────────────────────────────────────────────────────────────

    /// Returns sorted permutation: result[newPos] = original index of the effect
    /// that should appear at that position in the optimal signal chain.
    static std::vector<int> computeOptimalOrder(EffectChain& chain)
    {
        const int n = chain.effectCount();
        std::vector<int> indices(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            indices[static_cast<std::size_t>(i)] = i;

        std::stable_sort(indices.begin(), indices.end(),
            [&chain](int a, int b)
            {
                auto* fa = chain.getEffect(a);
                auto* fb = chain.getEffect(b);
                if (!fa || !fb) return false;
                return effectPriority(fa->type()) < effectPriority(fb->type());
            });

        return indices;
    }

    /// Applies the permutation to the chain using chain.moveEffect().
    /// Uses selection-sort style: O(n²) moves, each move is O(n).
    static void reorderChain(EffectChain& chain, const std::vector<int>& targetOrder)
    {
        const int n = static_cast<int>(targetOrder.size());

        // current[orig] = current position of the effect originally at index orig
        std::vector<int> current(static_cast<std::size_t>(n));
        // inverse[pos]  = original index of the effect currently at position pos
        std::vector<int> inverse(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            current[static_cast<std::size_t>(i)] = i;
            inverse[static_cast<std::size_t>(i)] = i;
        }

        for (int i = 0; i < n; ++i)
        {
            const int wantedOrig = targetOrder[static_cast<std::size_t>(i)];
            const int curPos     = current[static_cast<std::size_t>(wantedOrig)];
            if (curPos == i) continue;

            chain.moveEffect(curPos, i);

            // Update bookkeeping
            const int displaced = inverse[static_cast<std::size_t>(i)];
            current[static_cast<std::size_t>(displaced)]   = curPos;
            current[static_cast<std::size_t>(wantedOrig)]  = i;
            inverse[static_cast<std::size_t>(curPos)]      = displaced;
            inverse[static_cast<std::size_t>(i)]           = wantedOrig;
        }
    }

    // ── Smart Defaults ────────────────────────────────────────────────────────

    /// Applies saxophone-oriented smart defaults to every effect in the chain.
    static void applySmartDefaults(EffectChain& chain, const MusicContext& ctx)
    {
        for (int i = 0; i < chain.effectCount(); ++i)
        {
            auto* fx = chain.getEffect(i);
            if (fx) applyEffectRules(*fx, ctx);
        }
    }

private:
    // ── Signal chain priority (lower = earlier in the chain) ─────────────────

    static int effectPriority(EffectType t) noexcept
    {
        switch (t)
        {
        case EffectType::Synth:          return -1; // Synth first (pitch tracking needs clean input)
        case EffectType::Tuner:          return 0;
        case EffectType::Whammy:         return 1;
        case EffectType::PitchFork:      return 2;
        case EffectType::Octaver:        return 3;
        case EffectType::Harmonizer:     return 4;
        case EffectType::EnvelopeFilter: return 5;
        case EffectType::Flanger:        return 6;
        case EffectType::Slicer:         return 7;
        case EffectType::Delay:          return 8;
        case EffectType::Reverb:         return 9;
        default:                         return 10;
        }
    }

    // ── Per-effect smart rules ────────────────────────────────────────────────

    static void applyEffectRules(IEffect& fx, const MusicContext& ctx)
    {
        const float safeClamp = [](float v, float lo, float hi)
            { return v < lo ? lo : (v > hi ? hi : v); }(ctx.bpm, 20.f, 300.f);
        (void)safeClamp; // computed below per-effect

        switch (fx.type())
        {
        // Delay: dotted-eighth note synced to BPM
        case EffectType::Delay:
        {
            const float dotEighthMs = 60000.f / ctx.bpm * 0.75f;
            const float clamped     = dotEighthMs < 0.f ? 0.f
                                    : dotEighthMs > 2000.f ? 2000.f : dotEighthMs;
            fx.setParam(0, clamped); // time ms
            fx.setParam(1, 0.35f);   // feedback
            fx.setParam(2, 0.30f);   // mix
            break;
        }

        // Harmonizer: 3rd + 5th, mode-aware
        case EffectType::Harmonizer:
        {
            const float third = ctx.isMajor ? 4.f : 3.f; // maj or min 3rd
            fx.setParam(0, third); // voice 0
            fx.setParam(1, 7.f);   // voice 1 = perfect 5th
            fx.setParam(2, 0.40f); // mix
            break;
        }

        // Flanger: slow & lush for saxophone
        case EffectType::Flanger:
        {
            fx.setParam(0, 0.30f); // rate Hz
            fx.setParam(1, 0.40f); // depth
            fx.setParam(2, 0.30f); // feedback
            fx.setParam(3, 0.35f); // mix
            break;
        }

        // Reverb: natural medium room
        case EffectType::Reverb:
        {
            fx.setParam(0, 0.60f); // roomSize
            fx.setParam(1, 0.50f); // damping
            fx.setParam(2, 1.00f); // width
            fx.setParam(3, 0.25f); // mix
            break;
        }

        // Octaver: subtle -1 octave only
        case EffectType::Octaver:
        {
            fx.setParam(0, 0.40f); // oct1 (-1 oct)
            fx.setParam(1, 0.00f); // oct2 (-2 oct)
            fx.setParam(2, 1.00f); // dry
            break;
        }

        // Slicer: 1/8-note gate synced to BPM
        case EffectType::Slicer:
        {
            const float eighthHz = ctx.bpm / 60.f * 2.f;
            const float clamped  = eighthHz < 0.1f ? 0.1f
                                 : eighthHz > 20.f  ? 20.f : eighthHz;
            fx.setParam(0, clamped); // rate
            fx.setParam(1, 0.80f);   // depth
            break;
        }

        // EnvelopeFilter: responsive auto-wah for saxophone
        case EffectType::EnvelopeFilter:
        {
            fx.setParam(0, 3.00f);  // sensitivity
            fx.setParam(1, 5.00f);  // attack ms
            fx.setParam(2, 200.f);  // release ms
            fx.setParam(3, 1.50f);  // resonance
            fx.setParam(4, 0.60f);  // mix
            break;
        }

        // PitchFork: unison default (user drives with expression)
        case EffectType::PitchFork:
        {
            fx.setParam(0, 0.0f);  // semitones
            fx.setParam(1, 0.5f);  // mix
            break;
        }

        // Whammy: +1 oct range, heel at unison
        case EffectType::Whammy:
        {
            fx.setParam(0, 0.0f);  // expression (heel pos)
            fx.setParam(1, 12.f);  // toe pitch (+1 oct)
            fx.setParam(2, 0.0f);  // heel pitch (unison)
            fx.setParam(3, 1.0f);  // mix
            break;
        }

        default: break; // Tuner, AutoPitchCorrect: no smart defaults
        }

        // Apply genre style overrides on top of base defaults
        applyStyleOverrides(fx, ctx.style, ctx.bpm);
    }

    // ── Genre-style parameter overrides ──────────────────────────────────────

    static void applyStyleOverrides(IEffect& fx, MusicContext::Style style, float bpm)
    {
        switch (style)
        {
        // ── Jazz ──────────────────────────────────────────────────────────────
        case MusicContext::Style::Jazz:
            switch (fx.type())
            {
            case EffectType::Delay:
            {
                const float quarterMs = 60000.f / bpm;
                fx.setParam(0, quarterMs < 2000.f ? quarterMs : 2000.f);
                fx.setParam(1, 0.20f); // subtle feedback
                break;
            }
            case EffectType::Reverb:
                fx.setParam(0, 0.70f); // large jazz room
                fx.setParam(3, 0.30f);
                break;
            case EffectType::Harmonizer:
                fx.setParam(0, 4.f);   // major 3rd only (jazz voicing)
                fx.setParam(1, -12.f); // octave below instead of 5th
                fx.setParam(2, 0.30f);
                break;
            case EffectType::Flanger:
                fx.setParam(3, 0.00f); // off for jazz
                break;
            default: break;
            }
            break;

        // ── Funk ──────────────────────────────────────────────────────────────
        case MusicContext::Style::Funk:
            switch (fx.type())
            {
            case EffectType::EnvelopeFilter:
                fx.setParam(0, 6.00f); // hyper-sensitive
                fx.setParam(1, 3.00f); // snappy attack
                fx.setParam(4, 0.80f); // heavy mix
                break;
            case EffectType::Slicer:
            {
                const float sixteenthHz = bpm / 60.f * 4.f;
                fx.setParam(0, sixteenthHz < 20.f ? sixteenthHz : 20.f);
                fx.setParam(1, 1.0f); // full depth
                break;
            }
            case EffectType::Delay:
            {
                const float sixteenthMs = 60000.f / bpm * 0.25f;
                fx.setParam(0, sixteenthMs < 2000.f ? sixteenthMs : 2000.f);
                fx.setParam(1, 0.15f); // quick echo
                break;
            }
            case EffectType::Reverb:
                fx.setParam(0, 0.30f); // tight room
                fx.setParam(3, 0.10f); // almost dry
                break;
            default: break;
            }
            break;

        // ── Rock ──────────────────────────────────────────────────────────────
        case MusicContext::Style::Rock:
            switch (fx.type())
            {
            case EffectType::Octaver:
                fx.setParam(0, 0.70f); // heavier -1 oct
                fx.setParam(1, 0.20f); // some -2 oct
                break;
            case EffectType::Delay:
                fx.setParam(1, 0.50f); // slapback echo
                fx.setParam(2, 0.35f);
                break;
            case EffectType::Reverb:
                fx.setParam(0, 0.80f); // arena room
                fx.setParam(3, 0.35f);
                break;
            case EffectType::Whammy:
                fx.setParam(3, 1.0f); // whammy full mix
                break;
            default: break;
            }
            break;

        // ── Electro ───────────────────────────────────────────────────────────
        case MusicContext::Style::Electro:
            switch (fx.type())
            {
            case EffectType::Flanger:
                fx.setParam(0, 2.0f);  // fast LFO
                fx.setParam(1, 0.8f);  // deep mod
                fx.setParam(3, 0.6f);  // heavy mix
                break;
            case EffectType::Slicer:
            {
                const float eighthHz = bpm / 60.f * 2.f;
                fx.setParam(0, eighthHz < 20.f ? eighthHz : 20.f);
                fx.setParam(1, 1.0f);
                break;
            }
            case EffectType::Delay:
            {
                const float dotEighthMs = 60000.f / bpm * 0.75f;
                fx.setParam(0, dotEighthMs < 2000.f ? dotEighthMs : 2000.f);
                fx.setParam(1, 0.55f); // ping-pong feel
                fx.setParam(2, 0.40f);
                break;
            }
            case EffectType::Reverb:
                fx.setParam(0, 0.85f); // huge space
                fx.setParam(3, 0.45f);
                break;
            default: break;
            }
            break;

        default: break; // Style::None — no overrides
        }
    }
};

} // namespace dsp
