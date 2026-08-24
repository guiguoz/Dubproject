#pragma once
#include <algorithm>
#include <cmath>

namespace engine::fx {

// Soft-clipper/limiteur master — namespace engine::fx.
// Traite les deux canaux stéréo séparément, suit la réduction de gain
// pour la règle 6 de l'AutoMix.
class MasterLimiter {
public:
    MasterLimiter() = default;

    void prepare(double /*sampleRate*/) noexcept {
        gainReductionDb_ = 0.f;
    }

    void setEnabled(bool e)           noexcept { enabled_   = e; }
    bool isEnabled()            const noexcept { return enabled_; }

    void setThreshold(float threshDb) noexcept {
        threshold_ = std::pow(10.f, threshDb / 20.f);
    }

    // Traite un canal (in-place). Retourne la réduction de gain max (dB ≥ 0).
    void process(float* bufL, float* bufR, int numSamples) noexcept {
        if (!enabled_) { gainReductionDb_ = 0.f; return; }

        const float invT = 1.f / threshold_;
        float maxGR = 0.f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float rawL  = bufL[i];
            const float rawR  = bufR[i];
            bufL[i] = std::tanh(rawL * invT) * threshold_;
            bufR[i] = std::tanh(rawR * invT) * threshold_;

            // Réduction de gain : diff entre l'amplitude avant et après (en dB)
            const float absIn  = std::max(std::abs(rawL), std::abs(rawR));
            const float absOut = std::max(std::abs(bufL[i]), std::abs(bufR[i]));
            if (absIn > 1e-6f) {
                const float gr = 20.f * std::log10(absOut / absIn);
                if (gr < maxGR) maxGR = gr;  // GR est négatif
            }
        }
        gainReductionDb_ = -maxGR;  // positif = réduction effective en dB
    }

    // Réduction de gain du dernier appel process() en dB (≥ 0).
    float getGainReductionDb() const noexcept { return gainReductionDb_; }

private:
    float threshold_       = 0.95f;  // ≈ −0.4 dBFS
    bool  enabled_         = true;
    float gainReductionDb_ = 0.f;
};

} // namespace engine::fx
