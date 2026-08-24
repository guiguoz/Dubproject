#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/mix/MixAi.h
//
// Chemin IA ONNX du magic mix (étape 4 / M9) — EQ 3 bandes depuis les
// décisions du modèle, compensation de masquage Serum, calibration de gain.
//
// Le modèle ONNX lui-même reste hors du chemin par
// défaut (`SAXFX_HAS_ONNX` compilable, OFF — plan §7) : ce module est PURE et
// ne voit que des décisions [volume, lowGain, midGain, highGain] déjà
// prédites. L'inférence est fournie par l'appelant (EngineFacade / worker) ;
// la logique de post-traitement est ici, testable sans ONNX (EngineTests,
// C++17).
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "engine/mix/MixAlgorithms.h"
#include "engine/mix/MixEngine.h"

namespace engine::mix {

// ── Décision IA par slot (sortie du modèle mix ONNX) ────────────────────────
// volume   : gain linéaire cible [0, 1]
// lowGain  : EQ bande basse (dB) — low shelf  ~100 Hz
// midGain  : EQ bande médium (dB) — peaking   ~2500 Hz
// highGain : EQ bande haute (dB)  — high shelf ~8000 Hz
struct MixAiDecision
{
    float volume   = 0.5f;
    float lowGain  = 0.f;
    float midGain  = 0.f;
    float highGain = 0.f;
};

// ── Compensation de masquage Serum (chemin IA) ──────────────────────────────
// Compense le volume des slots qui partagent l'espace
// spectral avec Serum (proximité log2, duck graduel max 25 %) et creuse
// l'EQ mid/high des slots SYNTH/PAD quand Serum est lui-même SYNTH/PAD.
inline MixAiDecision serumCompensateDecision(const MixAiDecision& d,
                                             MixContentType slotType,
                                             float slotCentroid, float serumCentroid,
                                             float serumRms, float serumMidFrac,
                                             float serumHighFrac,
                                             MixContentType serumContentType) noexcept
{
    MixAiDecision comp = d;
    if (serumRms <= 0.02f) return comp;

    // Duck de volume par proximité spectrale (log2 — graduel).
    if (serumCentroid > 0.f && slotCentroid > 0.f)
    {
        const float ratio = slotCentroid / (serumCentroid + 1e-6f);
        const float prox  = std::clamp(1.f - std::abs(std::log2(ratio)), 0.f, 1.f);
        const float depth = std::clamp((serumRms - 0.02f) * 3.f, 0.f, 0.25f);
        if (prox > 0.f)
            comp.volume = std::max(comp.volume * (1.f - prox * depth), 0.1f);
    }

    // Carving EQ : Serum synthé/pad → creuse les slots similaires.
    const bool serumSynthy = (serumContentType == MixContentType::SYNTH
                           || serumContentType == MixContentType::PAD);
    const bool slotSynthy  = (slotType == MixContentType::SYNTH
                           || slotType == MixContentType::PAD);
    if (serumSynthy && slotSynthy)
    {
        comp.midGain  = std::max(comp.midGain  - serumMidFrac  * 4.f, -6.f);
        comp.highGain = std::max(comp.highGain - serumHighFrac * 4.f, -6.f);
    }
    return comp;
}

// ── Orchestrateur chemin IA ─────────────────────────────────────────────────
// 1. compensation Serum sur chaque décision
// 2. par slot actif : EQ 3 bandes (DC-block 20 → shelf 100 → peak 2500 →
//    shelf 8000 → LP 18 kHz), gains clampés ±6 dB (+1 dB air bias post-IA),
//    kick transient / bass harmonics selon le type effectif,
//    gain = clamp(0, 1.5, volume × saxClearance / truePeak)
// 3. slot 8 (DRM, hors périmètre du modèle 8 slots) : chemin heuristique LOOP
// 4. spatialisation + balance L/R (identique chemin heuristique)
inline MixOutputs processAiMix(const MixInputs& in,
                               const std::array<MixAiDecision, 8>& aiDecisions)
{
    MixOutputs out;
    const double sr = in.sampleRate;

    // Types effectifs (overrides appliqués)
    std::array<MixContentType, kMixSlots> types {};
    for (int i = 0; i < kMixSlots; ++i)
        types[i] = effectiveType(in.detected[i], in.hasOverride[i], in.overrideType[i]);

    // Centroïdes spectraux (spatialisation + duck Serum)
    std::array<float, kMixSlots> centroids {};
    for (int i = 0; i < kMixSlots; ++i)
        if (in.active[i])
            centroids[i] = estimateSpectralCentroid(in.pcm[i], sr);

    std::array<std::vector<float>, kMixSlots> pcms = in.pcm;

    // ── Slots 0-7 : chemin IA ────────────────────────────────────────────────
    for (int i = 0; i < 8; ++i)
    {
        if (!in.active[i]) continue;
        const MixContentType eff = types[i];

        MixAiDecision dec = serumCompensateDecision(
            aiDecisions[static_cast<std::size_t>(i)], eff, centroids[i],
            in.serumCentroid, in.serumRms, in.serumMidFrac, in.serumHighFrac,
            in.serumContentType);

        // EQ 3 bandes (gains clampés ±6 dB, +1 dB air bias post-IA)
        const float safeLoG = std::clamp(dec.lowGain,  -6.f, 6.f);
        const float safeMiG = std::clamp(dec.midGain,  -6.f, 6.f);
        const float safeHiG = std::clamp(dec.highGain + 1.0f, -6.f, 6.f);
        applyBiquad(pcms[i], makeHP       (20.f,           sr));  // DC block
        applyBiquad(pcms[i], makeLowShelf (100.f,  safeLoG, sr)); // sub-bass
        applyBiquad(pcms[i], makePeaking  (2500.f, safeMiG, 1.0f, sr)); // présence
        applyBiquad(pcms[i], makeHighShelf(8000.f, safeHiG, sr)); // air
        applyBiquad(pcms[i], makeLP       (18000.f,         sr)); // anti-alias
        if (eff == MixContentType::KICK) applyKickTransient(pcms[i], sr);
        if (eff == MixContentType::BASS) applyBassHarmonics(pcms[i], sr);

        const float truePeak = calculateTruePeak(pcms[i]);
        const float gain = std::clamp(
            (dec.volume * saxClearance(eff)) / std::max(truePeak, 0.001f),
            0.f, 1.5f);

        out.pcm [i] = std::move(pcms[i]);
        out.gain[i] = gain;
    }

    // ── Slot 8 (DRM loop) : hors périmètre du modèle 8 slots → heuristique ──
    {
        const int i = 8;
        if (in.active[i])
        {
            const MixContentType eff = types[i];  // LOOP par défaut / override
            applyRoleEQ(pcms[i], eff, sr);
            const float truePeak = calculateTruePeak(pcms[i]);
            const float gain = std::clamp(
                (targetGainForType(eff) * saxClearance(eff)) / std::max(truePeak, 0.001f),
                0.f, 1.5f);
            out.pcm [i] = std::move(pcms[i]);
            out.gain[i] = gain;
        }
    }

    // ── Spatialisation + balance L/R (identique chemin heuristique) ─────────
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
