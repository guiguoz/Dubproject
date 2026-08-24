#pragma once
#include <cmath>
#include <atomic>
#include "engine/SlotPlayer.h"   // SlotRole
#include "engine/Sequencer.h"   // kMaxSlots

namespace engine {

struct SlotFeatures {
    float rms       = 0.f;
    float peak      = 0.f;
    float energyLow = 0.f;  // biquad Linkwitz-Riley < 100 Hz
    float energyMid = 0.f;  // 100 Hz – 2.5 kHz
    float energyHigh= 0.f;  // > 2.5 kHz
};

struct MixTargets {
    float gainDb [kMaxSlots] = {};   // gain cible en dB
    float delayDb[kMaxSlots] = {};   // send delay en dB (0 = pas de send)
};

// AutoMix V2.0 — règles 1, 3, 4-simplifiée, 6.
// Thread audio : updateFeatures(), applySidechain(), advanceGainRamp/DelayRamp().
// Thread de mix (50 ms) : computeTargets().
class AutoMixDub {
public:
    void prepare(float sampleRate) noexcept;

    // Thread audio : accumule RMS/peak pour un slot depuis son bloc rendu.
    void updateFeatures(int slot,
                        const float* outL, const float* outR,
                        int numFrames) noexcept;

    // Thread de mix : recalcule les cibles de gain et de send.
    void computeTargets(const SlotRole roles[kMaxSlots]) noexcept;

    // Thread audio : applique le sidechain kick→BASS/PAD par échantillon.
    // kickSlot : index du slot kick (si −1, aucun sidechain).
    void applySidechain(int slot, SlotRole role,
                        float kickEnv,
                        float* outL, float* outR, int numFrames) noexcept;

    // Retourne l'enveloppe courante du kick (mis à jour via updateKickEnv).
    float kickEnv() const noexcept { return kickEnv_; }

    // Thread audio : avance l'enveloppe du kick d'un frame (attack/release).
    float advanceKickEnv(float kickSample) noexcept;

    // Thread audio : avance la rampe de gain (τ = 30 ms) vers la cible.
    // Retourne le gain courant linéaire.
    float advanceGainRamp(int slot, int numFrames) noexcept;

    // Thread audio : avance la rampe de send delay (τ = 120 ms).
    float advanceDelayRamp(int slot, int numFrames) noexcept;

    // Informe AutoMix de la réduction de gain du limiteur (Règle 6).
    void notifyLimiterReduction(float gainReductionDb) noexcept;

    const SlotFeatures& features(int slot) const noexcept { return features_[slot]; }
    const MixTargets&   targets()          const noexcept { return targets_; }

    float currentGainLinear(int slot)  const noexcept { return currentGainLin_[slot]; }
    float currentDelaySend(int slot)   const noexcept { return currentDelaySend_[slot]; }

    // Règles statiques par rôle (pures).
    static float roleTargetDb(SlotRole role) noexcept;
    static float roleStaticDelaySend(SlotRole role) noexcept;

private:
    float sampleRate_ = 44100.f;

    SlotFeatures features_[kMaxSlots];
    MixTargets   targets_;

    float currentGainLin_  [kMaxSlots] = {};
    float currentDelaySend_[kMaxSlots] = {};

    // Rampe exponentielle : coefficients (calculés dans prepare)
    float gainTauCoef_  = 0.f;   // exp(-1/(τ_gain * sr))
    float delayTauCoef_ = 0.f;   // exp(-1/(τ_delay * sr))

    // Sidechain (règle 3)
    float kickEnv_      = 0.f;
    float attackCoef_   = 0.f;   // exp(-1 / (attack_s * sr))
    float releaseCoef_  = 0.f;

    // Hystérésis ±1 dB (règle 1)
    float lastDecisionDb_[kMaxSlots] = {};

    // Règle 6 : protection limiteur
    float limiterAccumSec_  = 0.f;  // secondes d'accumulation > 3 dB GR
    float globalTrimDb_     = 0.f;

    // Accumulateur RMS (rolling)
    float rmsAccum_ [kMaxSlots] = {};
    int   rmsSamples_[kMaxSlots] = {};

    // Coefficients d'un pole-1 LP
    static float tauToCoef(float tauMs, float sr) noexcept {
        return std::exp(-1.f / (tauMs * 0.001f * sr));
    }
};

} // namespace engine
