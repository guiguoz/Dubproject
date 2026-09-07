#pragma once
#include <atomic>
#include <cmath>
#include <algorithm>

namespace engine::fx {

// ─── WarmthProcessor — saturation parallèle ──────────────────────────────────
// Mélange le signal sec avec une version saturée (tanh normalisé).
//
// Paramètres :
//   drive  0.0 → 2.0  — intensité de la saturation sur la branche wet
//   mix    0.0 → 1.0  — proportion de signal saturé (0 = dry pur, 1 = full sat)
//
// Formule wet : y = tanh(drive × x) / tanh(drive)
// Compensation : le gain bas-niveau de la chaîne parallèle est G = (1-mix) + mix·d/tanh(d) > 1.
// On divise la sortie par G → gain unitaire exact à bas niveau pour un A/B sans biais de volume.
// Sortie     : out = [dry·(1-mix) + wet·mix] / G
//
// Les transitoires (kick) restent intacts car le signal sec est préservé.
// La chaleur vient des harmoniques paires/impaires sur les mediums soutenus.

class WarmthProcessor {
public:
    void setDrive(float drive) noexcept {
        drive_.store(std::clamp(drive, 0.f, 2.f), std::memory_order_relaxed);
    }
    float getDrive() const noexcept {
        return drive_.load(std::memory_order_relaxed);
    }

    void setMix(float mix) noexcept {
        mix_.store(std::clamp(mix, 0.f, 1.f), std::memory_order_relaxed);
    }
    float getMix() const noexcept {
        return mix_.load(std::memory_order_relaxed);
    }

    void process(float* L, float* R, int numSamples) noexcept {
        const float d   = drive_.load(std::memory_order_relaxed);
        const float wet = mix_.load(std::memory_order_relaxed);
        const float dry = 1.f - wet;

        if (wet < 0.001f || d < 0.001f) return;  // bypass exact

        const float invTanhD = 1.f / std::tanh(d);
        // Gain bas-niveau de la chaîne parallèle : G = dry + wet·(d/tanh(d))
        // invG ramène ce gain à 1 → bypass et saturation au même volume perçu
        const float invG = 1.f / (dry + wet * (d * invTanhD));

        for (int i = 0; i < numSamples; ++i) {
            const float dryL = L[i];
            const float dryR = R[i];
            const float satL = std::tanh(d * dryL) * invTanhD;
            const float satR = std::tanh(d * dryR) * invTanhD;
            L[i] = (dryL * dry + satL * wet) * invG;
            R[i] = (dryR * dry + satR * wet) * invG;
        }
    }

private:
    std::atomic<float> drive_ {1.0f};  // intensité saturation branche wet
    std::atomic<float> mix_   {0.3f};  // 0.3 = 30% saturé / 70% sec
};

} // namespace engine::fx
