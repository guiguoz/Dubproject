#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// engine/mix/MixState.h
//
// État de mix persistant par slot (étape 7 / M9) — aligné sur le schéma
// projet v5 (SlotMixData : gain/pan/width/depth/applied).
//
// Un slot traité par le magic mix produit un SlotMixState persistable :
//   - capturé depuis MixOutputs (processHeuristic, étape 3) via mixStateFromOutputs,
//   - restauré depuis un projet chargé via setSlotMixState (l'appelant applique
//     ensuite gain/spatial au runtime : SlotPlayer),
//   - lu pour la sauvegarde projet via slotMixState.
//
// Zéro dépendance JUCE / dsp/ : compilable par EngineTests (C++17).
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <cstddef>
#include "engine/mix/MixEngine.h"

namespace engine::mix {

// ── Slot mix state — per-slot magic mix result ──────────────────────────────
struct SlotMixState
{
    float gain    = 1.f;   // gain cible IA (magic mix)
    float pan     = 0.f;   // pan égal-power [-1 = L … +1 = R]
    float width   = 0.f;   // Haas width [0, 1]
    float depth   = 0.f;   // depth (UI visualisation uniquement)
    bool  applied = false; // true = magic mix actif sur ce slot
};

using MixStateArray = std::array<SlotMixState, kMixSlots>;

/// Capture l'état persistant depuis les sorties d'un mix. Un slot est
/// « applied » s'il a été traité (PCM de sortie non vide) ; les autres
/// conservent leurs défauts (gain 1, spatial neutre) pour ne rien écraser
/// à la restauration.
inline MixStateArray mixStateFromOutputs(const MixOutputs& out) noexcept
{
    MixStateArray st;
    for (int i = 0; i < kMixSlots; ++i)
    {
        const auto& pcm = out.pcm[static_cast<std::size_t>(i)];
        if (pcm.empty()) continue;
        auto& s = st[static_cast<std::size_t>(i)];
        s.gain    = out.gain[i];
        s.pan     = out.pan[i];
        s.width   = out.width[i];
        s.depth   = out.depth[i];
        s.applied = true;
    }
    return st;
}

/// Écrit l'état d'un slot (restore projet) et marque `applied`. L'application
/// au runtime (gain + spatial SlotPlayer) reste à la charge de l'appelant.
inline void setSlotMixState(MixStateArray& states, int slot,
                            float gain, float pan, float width, float depth) noexcept
{
    if (slot < 0 || slot >= kMixSlots) return;
    auto& s = states[static_cast<std::size_t>(slot)];
    s.gain    = gain;
    s.pan     = pan;
    s.width   = width;
    s.depth   = depth;
    s.applied = true;
}

/// Lit l'état d'un slot (sauvegarde projet / UI). Slot invalide → défaut.
inline SlotMixState slotMixState(const MixStateArray& states, int slot) noexcept
{
    if (slot < 0 || slot >= kMixSlots) return {};
    return states[static_cast<std::size_t>(slot)];
}

/// Revert du magic mix (étape 5 / M9) : remet TOUT l'état persistant aux
/// défauts (gain 1, spatial neutre, applied=false). Le PCM n'étant jamais
/// modifié, il n'y a rien à recharger. L'application au runtime (gain 1 + spatial neutre
/// SlotPlayer) reste à la charge de l'appelant.
inline void resetMixState(MixStateArray& states) noexcept
{
    states = {};
}

} // namespace engine::mix
