#pragma once

#include <array>
#include <vector>

#include "engine/mix/MixEngine.h"
#include "engine/mix/MixState.h"
#include "engine/mix/MixAi.h"

namespace engine::mix {

// Snapshot du runtime V2 capturé par la façade (message thread) avant le lancement
// du worker magic mix (M9 étape 8). Header-only / sans JUCE : testable dans
// EngineTests.
struct MixWorkerInputs {
    // PCM mono par slot ; vide = slot non chargé.
    std::array<std::vector<float>, kMixSlots> pcm;
    // Type effectif par slot (rôles → types résolus par la façade).
    std::array<MixContentType, kMixSlots> types {};
    // Slot chargé (a un échantillon) / muet (exclu du mix).
    std::array<bool, kMixSlots> loaded {};
    std::array<bool, kMixSlots> muted {};
    // Scène courante (densité utilisée par le mix heuristique).
    SceneSnapshot scene;
    float  masterBpm  = 0.f;
    double sampleRate = 44100.0;

    // Contexte Serum (compensation de masquage) — rempli par la façade.
    float serumRms      = 0.f;
    float serumCentroid = 0.f;
    MixContentType serumContentType = MixContentType::OTHER;
    float serumMidFrac  = 0.f;
    float serumHighFrac = 0.f;
};

// Construit les MixInputs du moteur de mix depuis le snapshot runtime.
inline MixInputs toMixInputs(const MixWorkerInputs& w) noexcept
{
    MixInputs in;
    in.masterBpm  = w.masterBpm;
    in.sampleRate = w.sampleRate;
    in.scene      = w.scene;
    in.serumRms      = w.serumRms;
    in.serumCentroid = w.serumCentroid;
    in.serumContentType = w.serumContentType;
    in.serumMidFrac  = w.serumMidFrac;
    in.serumHighFrac = w.serumHighFrac;
    for (int i = 0; i < kMixSlots; ++i) {
        in.pcm[i]      = w.pcm[i];
        in.detected[i] = w.types[i];
        in.active[i]   = w.loaded[i] && !w.muted[i];
    }
    return in;
}

// Orchestration complète du worker : snapshot → processHeuristic → état
// persistant (gain/pan/width/depth par slot). La façade applique ensuite
// l'état au runtime via setSlotMixState.
inline MixStateArray runHeuristicMix(const MixWorkerInputs& w) noexcept
{
    return mixStateFromOutputs(processHeuristic(toMixInputs(w)));
}

// Orchestration du chemin IA ONNX (étape 4 / M9) : snapshot → processAiMix
// avec les décisions du modèle (8 slots) → état persistant. Le modèle de prédit
// que 8 slots ; le slot 8 (DRM) est traité en heuristique par processAiMix.
inline MixStateArray runAiMix(const MixWorkerInputs& w,
                              const std::array<MixAiDecision, 8>& decisions) noexcept
{
    return mixStateFromOutputs(processAiMix(toMixInputs(w), decisions));
}

} // namespace engine::mix
