#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>

namespace engine::fx {

// ─── MasterLimiter ───────────────────────────────────────────────────────────
// Garde-fou anti-clipping transparent — peak limiter par bloc stéréo-couplé.
//
// Comportement :
//   • Si le pic max(|L|, |R|) ≤ threshold sur TOUT le bloc → identité exacte,
//     zéro coloration, zéro distorsion harmonique.
//   • Si un pic dépasse le seuil → atténuation multiplicative uniforme du bloc
//     entier (scale = threshold / peak). Pas de tanh, pas de gain boost.
//
// L'atténuation stéréo-couplée préserve l'image : L et R sont réduits du même
// facteur, l'équilibre du champ stéréo est inchangé.
//
// threshold par défaut : 0.98f ≈ −0.17 dBFS — n'agit que sur les vrais clips.

class MasterLimiter {
public:
    MasterLimiter() = default;

    void prepare(double /*sampleRate*/) noexcept {
        gainReductionDb_ = 0.f;
    }

    void setEnabled(bool e) noexcept {
        enabled_.store(e, std::memory_order_relaxed);
    }
    bool isEnabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    // Seuil en dBFS (ex : −0.2 dBFS = setThreshold(-0.2f)).
    void setThreshold(float threshDb) noexcept {
        threshold_.store(std::pow(10.f, threshDb / 20.f), std::memory_order_relaxed);
    }

    // Traitement in-place stéréo-couplé. Identité exacte sous le seuil.
    void process(float* L, float* R, int numSamples) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) {
            gainReductionDb_ = 0.f;
            return;
        }

        const float thr = threshold_.load(std::memory_order_relaxed);

        // 1. Trouver le pic max sur le bloc (stéréo-couplé).
        float peak = 0.f;
        for (int i = 0; i < numSamples; ++i) {
            const float p = std::max(std::abs(L[i]), std::abs(R[i]));
            if (p > peak) peak = p;
        }

        // 2. Si le pic dépasse le seuil : atténuation uniforme du bloc.
        if (peak > thr) {
            const float scale = thr / peak;
            for (int i = 0; i < numSamples; ++i) {
                L[i] *= scale;
                R[i] *= scale;
            }
            gainReductionDb_ = -20.f * std::log10(scale);  // ≥ 0
        } else {
            gainReductionDb_ = 0.f;
        }
    }

    // Réduction de gain du dernier bloc (dB ≥ 0). 0 = aucune action.
    float getGainReductionDb() const noexcept { return gainReductionDb_; }

private:
    std::atomic<float> threshold_ {0.98f};  // ≈ −0.17 dBFS
    std::atomic<bool>  enabled_   {true};
    float              gainReductionDb_ = 0.f;
};

} // namespace engine::fx
