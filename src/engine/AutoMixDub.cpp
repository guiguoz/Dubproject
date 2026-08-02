#include "engine/AutoMixDub.h"
#include <cstring>
#include <algorithm>

namespace engine {

// ─── Cibles de gain par rôle (dB, relatif au kick = 0) ──────────────────────
float AutoMixDub::roleTargetDb(SlotRole role) noexcept
{
    switch (role) {
        case SlotRole::Kick:    return  0.f;
        case SlotRole::Bass:    return -2.f;
        case SlotRole::Snare:   return -4.f;
        case SlotRole::Perc:    return -4.f;
        case SlotRole::Melodic: return -8.f;
        case SlotRole::Pad:     return -12.f;
        case SlotRole::Fx:      return -10.f;
        case SlotRole::Loop:    return -6.f;
        case SlotRole::Drum:    return -4.f;
        case SlotRole::Unknown: return  0.f;   // neutre
        default:                return  0.f;
    }
}

// ─── Sends statiques par rôle (règle 4 simplifiée) ──────────────────────────
float AutoMixDub::roleStaticDelaySend(SlotRole role) noexcept
{
    switch (role) {
        case SlotRole::Snare:   return 0.25f;
        case SlotRole::Perc:    return 0.25f;
        case SlotRole::Melodic: return 0.15f;
        default:                return 0.f;
    }
}

// ─── prepare ─────────────────────────────────────────────────────────────────
void AutoMixDub::prepare(float sampleRate) noexcept
{
    sampleRate_  = sampleRate;

    // Constante de temps : 30 ms gains, 120 ms sends
    gainTauCoef_  = tauToCoef(30.f,  sampleRate);
    delayTauCoef_ = tauToCoef(120.f, sampleRate);

    // Sidechain : attack 5 ms, release 120 ms
    attackCoef_   = tauToCoef(5.f,   sampleRate);
    releaseCoef_  = tauToCoef(120.f, sampleRate);

    kickEnv_        = 0.f;
    globalTrimDb_   = 0.f;
    limiterAccumSec_= 0.f;

    for (int s = 0; s < kMaxSlots; ++s) {
        features_[s]          = {};
        currentGainLin_[s]    = 1.f;
        currentDelaySend_[s]  = 0.f;
        lastDecisionDb_[s]    = 0.f;
        rmsAccum_[s]          = 0.f;
        rmsSamples_[s]        = 0;
    }
    targets_ = {};
}

// ─── updateFeatures ──────────────────────────────────────────────────────────
void AutoMixDub::updateFeatures(int slot,
                                 const float* outL, const float* outR,
                                 int numFrames) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;

    float sumSq = 0.f, peak = 0.f;
    for (int i = 0; i < numFrames; ++i) {
        const float s = (outL[i] + outR[i]) * 0.5f;
        sumSq += s * s;
        const float abs = s < 0.f ? -s : s;
        if (abs > peak) peak = abs;
    }

    // Accumulation rolling RMS sur ~400 ms
    const int kWindow = static_cast<int>(sampleRate_ * 0.4f);
    rmsAccum_[slot]   += sumSq;
    rmsSamples_[slot] += numFrames;
    if (rmsSamples_[slot] >= kWindow) {
        features_[slot].rms  = std::sqrt(rmsAccum_[slot] / static_cast<float>(rmsSamples_[slot]));
        rmsAccum_[slot]   = 0.f;
        rmsSamples_[slot] = 0;
    }
    features_[slot].peak = peak;
}

// ─── computeTargets (thread de mix, 50 ms) ────────────────────────────────
void AutoMixDub::computeTargets(const SlotRole roles[kMaxSlots]) noexcept
{
    // Chercher le kick pour la référence 0 dB
    float kickRms = -1.f;
    for (int s = 0; s < kMaxSlots; ++s)
        if (roles[s] == SlotRole::Kick && features_[s].rms > 0.f)
            kickRms = features_[s].rms;

    for (int s = 0; s < kMaxSlots; ++s) {
        // ─ Règle 1 : gain staging
        float targetDb = roleTargetDb(roles[s]) + globalTrimDb_;

        if (kickRms > 0.f && features_[s].rms > 0.f) {
            // Correction pour atteindre la cible relative
            const float currentRelDb = 20.f * std::log10(features_[s].rms / kickRms);
            const float error        = targetDb - currentRelDb;
            // Borner ±9 dB
            const float corrDb = std::clamp(error, -9.f, 9.f);

            // Hystérésis ±1 dB : ne changer que si l'écart dépasse 1 dB
            if (std::abs(corrDb - lastDecisionDb_[s]) > 1.f) {
                targets_.gainDb[s]  = corrDb;
                lastDecisionDb_[s]  = corrDb;
            }
            // sinon on garde la décision précédente (hystérésis)
        } else {
            // Silence : retour neutre (0 dB correction)
            targets_.gainDb[s]  = 0.f;
            lastDecisionDb_[s]  = 0.f;
        }

        // ─ Règle 4 simplifiée : sends statiques
        const float sendLinear = roleStaticDelaySend(roles[s]);
        targets_.delayDb[s] = (sendLinear > 0.f)
            ? 20.f * std::log10(sendLinear)
            : -120.f;
    }
}

// ─── advanceKickEnv ──────────────────────────────────────────────────────────
float AutoMixDub::advanceKickEnv(float kickSample) noexcept
{
    const float abs = kickSample < 0.f ? -kickSample : kickSample;
    if (abs > kickEnv_)
        kickEnv_ = attackCoef_  * kickEnv_ + (1.f - attackCoef_)  * abs;
    else
        kickEnv_ = releaseCoef_ * kickEnv_ + (1.f - releaseCoef_) * abs;
    return kickEnv_;
}

// ─── applySidechain ──────────────────────────────────────────────────────────
void AutoMixDub::applySidechain(int slot, SlotRole role,
                                 float kickEnv,
                                 float* outL, float* outR,
                                 int numFrames) noexcept
{
    // Uniquement BASS et PAD sont affectés (règle 3)
    if (role != SlotRole::Bass && role != SlotRole::Pad) return;

    // Réduction max : −4 dB = facteur 0.631
    static constexpr float kMaxReductionDb = 4.f;
    // gain = 1 - kickEnv * (1 - 10^(-kMaxReductionDb/20))
    const float minGain = std::pow(10.f, -kMaxReductionDb / 20.f);  // ≈ 0.631
    const float gain    = 1.f - kickEnv * (1.f - minGain);

    for (int i = 0; i < numFrames; ++i) {
        outL[i] *= gain;
        outR[i] *= gain;
    }
}

// ─── advanceGainRamp ─────────────────────────────────────────────────────────
float AutoMixDub::advanceGainRamp(int slot, int numFrames) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 1.f;

    const float targetLin = std::pow(10.f, targets_.gainDb[slot] / 20.f);
    // Rampe exponentielle : coef^numFrames pour avancer d'un bloc
    const float alpha = std::pow(gainTauCoef_, static_cast<float>(numFrames));
    currentGainLin_[slot] = alpha * currentGainLin_[slot]
                          + (1.f - alpha) * targetLin;
    return currentGainLin_[slot];
}

// ─── advanceDelayRamp ────────────────────────────────────────────────────────
float AutoMixDub::advanceDelayRamp(int slot, int numFrames) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return 0.f;

    const float targetLin = (targets_.delayDb[slot] > -110.f)
        ? std::pow(10.f, targets_.delayDb[slot] / 20.f)
        : 0.f;
    const float alpha = std::pow(delayTauCoef_, static_cast<float>(numFrames));
    currentDelaySend_[slot] = alpha * currentDelaySend_[slot]
                            + (1.f - alpha) * targetLin;
    return currentDelaySend_[slot];
}

// ─── notifyLimiterReduction (règle 6) ────────────────────────────────────────
void AutoMixDub::notifyLimiterReduction(float gainReductionDb) noexcept
{
    const float kFrameSec = static_cast<float>(512) / sampleRate_;  // ≈ bloc

    if (gainReductionDb > 3.f) {
        limiterAccumSec_ += kFrameSec;
        if (limiterAccumSec_ > 2.f) {
            globalTrimDb_    -= 1.f;
            limiterAccumSec_  = 0.f;
        }
    } else {
        limiterAccumSec_ = 0.f;
    }
    // Limiter le trim global (pas de sur-compensation)
    globalTrimDb_ = std::clamp(globalTrimDb_, -12.f, 0.f);
}

} // namespace engine
