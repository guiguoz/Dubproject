#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/mix/MixEngine.h
//
// Orchestrateur hors-ligne du magic mix (étape 3 / M9) — reproduit
// l'enchaînement complet de la branche HEURISTIQUE
// (phase 1 detection + centroid, phase 2 EQ/gain/dub-echo/serum-duck,
// phase 4 spatialisation + balance L/R)
// SANS side-effects sur un player : entrées PCM + flag de scène → sorties
// PCM traités + gains + spatials.
//
// Zéro dépendance JUCE / dsp/ : compilable par EngineTests (C++17).
//
// Le chemin IA ONNX est dans MixAi.h : ce module ne porte QUE la branche
// heuristique utilisée en fallback.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <cstddef>
#include <vector>
#include "engine/mix/MixAlgorithms.h"
#include "engine/mix/MixDecisions.h"

namespace engine::mix {

inline constexpr int kMixSlots = 9;  // S1–S8 + Drum loop (slot 8)

struct MixInputs
{
    // PCM mono par slot (vide = non chargé). Les slots non actifs (mutés ou
    // non chargés) sont ignorés par le mix.
    std::array<std::vector<float>, kMixSlots> pcm {};
    // active[i] = chargé && non muté
    std::array<bool, kMixSlots> active {};

    // Types détectés (pré-classification) + overrides manuels utilisateur
    std::array<MixContentType, kMixSlots> detected     {};
    std::array<bool, kMixSlots>           hasOverride  {};
    std::array<MixContentType, kMixSlots> overrideType {};

    SceneSnapshot scene;      // scène courante (densité → scale de gain)
    float  masterBpm    = 0.f;
    double sampleRate   = 44100.0;

    // Contexte Serum (compensation de masquage). rms <= 0.02 → pas de duck.
    float serumRms      = 0.f;
    float serumCentroid = 0.f;

    // Contexte Serum complet pour le chemin IA (étape 4 / M9) — carving EQ du
    // modèle : type Serum (SYNTH/PAD → creuse les slots similaires), fractions
    // de bandes mid/high du Serum. Defaults neutres → pas de carving si non
    // renseigné par l'appelant.
    MixContentType serumContentType = MixContentType::OTHER;
    float serumMidFrac  = 0.f;
    float serumHighFrac = 0.f;
};

struct MixOutputs
{
    // PCM traités (non vides pour les slots actifs ; inchangés sinon).
    // L'appelant décide comment les réinjecter (mono vs stéréo, voir QUESTIONS.md).
    std::array<std::vector<float>, kMixSlots> pcm {};
    std::array<float, kMixSlots>              gain    {};
    std::array<float, kMixSlots>              pan     {};
    std::array<float, kMixSlots>              width   {};
    std::array<float, kMixSlots>              depth   {};
};

// ── Phase 4 pass 2 : correction globale de balance L/R ──────────────────────
// Flip les slots PAD/SYNTH/PERC si |leftCount - rightCount| > 3.
inline void balanceLeftRight(std::array<SpatialDecision, kMixSlots>& spatials,
                             const std::array<bool, kMixSlots>& active,
                             const std::array<MixContentType, kMixSlots>& types)
{
    int leftCount = 0, rightCount = 0;
    for (int i = 0; i < kMixSlots; ++i)
    {
        if (!active[i]) continue;
        if (spatials[i].pan < -0.1f) ++leftCount;
        if (spatials[i].pan >  0.1f) ++rightCount;
    }
    if (std::abs(leftCount - rightCount) <= 3) return;

    for (int i = 0; i < kMixSlots && std::abs(leftCount - rightCount) > 1; ++i)
    {
        if (!active[i]) continue;
        const MixContentType t = types[i];
        if (t != MixContentType::PAD && t != MixContentType::SYNTH
         && t != MixContentType::PERC) continue;

        auto& sp = spatials[i];
        if (leftCount > rightCount && sp.pan < 0.f)
        {
            sp.pan = -sp.pan;
            --leftCount; ++rightCount;
        }
        else if (rightCount > leftCount && sp.pan > 0.f)
        {
            sp.pan = -sp.pan;
            --rightCount; ++leftCount;
        }
    }
}

// ── Orchestrateur heuristique ────────────────────────────────────────────────
//
// Ordre du traitement :
//   1. densityScale + bassPresent depuis la scène
//   2. applySubOwnership (KICK/BASS 30-60 Hz)
//   3. par slot : applyRoleEQ → kick transient → bass harmonics
//      → comp bass absente (PAD/SYNTH) → applyUnmasking → dub echo
//      → gain calibré (target × density × clearance / truePeak, clamp 0-1.5)
//      → duck Serum (PAD/SYNTH/LOOP seulement)
//   4. computeSpatialization + balance L/R
inline MixOutputs processHeuristic(const MixInputs& in)
{
    MixOutputs out;
    const double sr = in.sampleRate;

    // Types effectifs (overrides appliqués) — tableau plein pour unmasking
    std::array<MixContentType, kMixSlots> types   {};
    for (int i = 0; i < kMixSlots; ++i)
        types[i] = effectiveType(in.detected[i], in.hasOverride[i], in.overrideType[i]);

    // Centroïdes spectraux (recalculés ici)
    std::array<float, kMixSlots> centroids {};
    for (int i = 0; i < kMixSlots; ++i)
        if (in.active[i])
            centroids[i] = estimateSpectralCentroid(in.pcm[i], sr);

    // Phase 2 — sous-produits PCM traités
    std::array<std::vector<float>, kMixSlots> pcms = in.pcm;

    const float dScale = densityScale(in.scene);
    const bool  bassOn = hasActiveBass(in.scene);

    // Sub ownership 30-60 Hz (types détectés bruts)
    applySubOwnership(pcms.data(), types.data(), kMixSlots, sr);

    for (int i = 0; i < kMixSlots; ++i)
    {
        if (!in.active[i]) continue;
        const MixContentType eff = types[i];

        applyRoleEQ(pcms[i], eff, sr);
        if (eff == MixContentType::KICK) applyKickTransient(pcms[i], sr);
        if (eff == MixContentType::BASS) applyBassHarmonics(pcms[i], sr);

        // Compensation bass absente → boost mid-lows des pads/synths
        if (!bassOn && (eff == MixContentType::PAD || eff == MixContentType::SYNTH))
            applyBiquad(pcms[i], makeLowShelf(150.f, 2.f, sr));

        applyUnmasking(pcms[i], i, types.data(), kMixSlots, sr);

        // Echo dub rythmique (PAD, SYNTH, PERC)
        if (in.masterBpm > 0.f)
        {
            if (eff == MixContentType::PAD)
                applyDubEcho(pcms[i], in.masterBpm, sr, 0.35f, 4);
            else if (eff == MixContentType::SYNTH)
                applyDubEcho(pcms[i], in.masterBpm, sr, 0.25f, 8);
            else if (eff == MixContentType::PERC)
                applyDubEcho(pcms[i], in.masterBpm, sr, 0.20f, 8);
        }

        // Gain calibré
        const float truePeak = calculateTruePeak(pcms[i]);
        float gain = computeTargetGain(eff, dScale, saxClearance(eff), truePeak);

        // Duck Serum (heuristique)
        gain = serumDuckGain(gain, eff, centroids[i], in.serumCentroid, in.serumRms);

        out.pcm [i] = std::move(pcms[i]);
        out.gain[i] = gain;
    }

    // Phase 4 — spatialisation + balance L/R
    std::array<SpatialDecision, kMixSlots> spatials {};
    for (int i = 0; i < kMixSlots; ++i)
        if (in.active[i])
            spatials[i] = computeSpatialization(i, types[i], centroids[i]);

    balanceLeftRight(spatials, in.active, types);

    for (int i = 0; i < kMixSlots; ++i)
    {
        if (!in.active[i]) continue;
        out.pan   [i] = spatials[i].pan;
        out.width [i] = spatials[i].width;
        out.depth [i] = spatials[i].depth;
    }

    return out;
}

} // namespace engine::mix