#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/mix/MixDecisions.h
//
// Décisions pures du magic mix (étape 2 / M9) — portées depuis la branche
// heuristique de SmartSamplerEngine::applyNeutronMix (phase 2 et duck Serum).
// Zéro dépendance JUCE / dsp/ : compilable par EngineTests (C++17).
//
// Contenu :
//   - densité de scène → scale de gain adaptatif
//   - présence de bass dans la scène (compensation EQ)
//   - type effectif (override utilisateur)
//   - calibration de gain (true-peak + densityScale + headroom sax)
//   - duck Serum (compensation de masquage PAD/SYNTH/LOOP)
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include "engine/mix/MixAlgorithms.h"

namespace engine::mix {

// ── Vue de scène (port de SmartSamplerEngine::SceneSnapshot) ────────────────

struct SceneSnapshot
{
    std::array<bool, 9>        slotActive {};
    std::array<MixContentType, 9> slotTypes  {};
    int  activeCount  { 0 };
    bool isBreakdown  { false };  // <= 2 pistes actives
    bool isDrop       { false };  // >= 6 pistes actives
};

/// Densité de la scène courante → scale de gain adaptatif (phase 2 heuristique).
inline float densityScale(const SceneSnapshot& snap) noexcept
{
    return snap.isDrop      ? 0.95f  // drop dense
         : snap.isBreakdown ? 1.15f  // breakdown : plus d'espace
         : (snap.activeCount >= 4) ? 1.05f  // build-up
         :                              1.10f;
}

/// Compensation d'absence de bass : vrai si un slot BASS est actif dans la scène.
inline bool hasActiveBass(const SceneSnapshot& snap) noexcept
{
    for (int i = 0; i < 9; ++i)
        if (snap.slotActive[static_cast<std::size_t>(i)]
            && snap.slotTypes[static_cast<std::size_t>(i)] == MixContentType::BASS)
            return true;
    return false;
}

/// Type effectif : override utilisateur si présent, sinon type détecté.
inline MixContentType effectiveType(MixContentType detected, bool hasOverride,
                                    MixContentType overrideType) noexcept
{
    return hasOverride ? overrideType : detected;
}

/// Calibration de gain (phase 2 heuristique) :
/// clamp(0, 1.5, targetGainForType × densityScale × saxClearance / truePeak).
inline float computeTargetGain(MixContentType type, float densityScale,
                               float clearance, float truePeak) noexcept
{
    const float gain = (targetGainForType(type) * densityScale * clearance)
                       / std::max(truePeak, 0.001f);
    return std::clamp(gain, 0.f, 1.5f);
}

// ── Duck Serum (phase 2 heuristique) ────────────────────────────────────────
//
// Duck PAD/SYNTH/LOOP slots that share spectral space with Serum.
// KICK/BASS/SNARE/HIHAT intentionally excluded to preserve groove.
// Proximity via log2 — gradual: 1.0=same oct, 0.0=±1 oct, <0=beyond.
// depth: 0 at rms=0.02, max 20 % (heuristique path).
inline bool isSerumBedType(MixContentType type) noexcept
{
    return type == MixContentType::PAD
        || type == MixContentType::SYNTH
        || type == MixContentType::LOOP;
}

/// Retourne le gain atténué (≥ 0.05) si le slot doit être ducké.
inline float serumDuckGain(float currentGain, MixContentType slotType,
                           float slotCentroid, float serumCentroid,
                           float serumRms) noexcept
{
    if (serumRms <= 0.02f) return currentGain;
    if (!isSerumBedType(slotType)) return currentGain;
    if (serumCentroid <= 0.f || slotCentroid <= 0.f) return currentGain;

    const float ratio = slotCentroid / (serumCentroid + 1e-6f);
    const float prox  = std::clamp(1.f - std::abs(std::log2(ratio)), 0.f, 1.f);
    const float depth = std::clamp((serumRms - 0.02f) * 3.f, 0.f, 0.20f);
    if (prox <= 0.f) return currentGain;
    return std::max(currentGain * (1.f - prox * depth), 0.05f);
}

} // namespace engine::mix