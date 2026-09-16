#pragma once
// SceneTransitionPlan — plan immutable PREPARE → COMMIT audio sample-accurate → CLEANUP.
// POD, préalloué, borné. Aucune allocation/string/comparaison de path en audio.
#include <cstdint>
#include <string>
#include <array>
#include "engine/SceneStore.h"

namespace engine {

// ── AssetId : identité stable d'un asset (chemin canonique + trims) ───────────
// Préparé hors audio. 0 = vide. Aucune string en COMMIT.
using AssetId = uint64_t;
AssetId assetIdFor(const std::string& filePath, int trimStart, int trimEnd) noexcept;
inline AssetId assetIdForSlot(const SlotConfig& cfg) noexcept {
    return assetIdFor(cfg.filePath, cfg.trimStart, cfg.trimEnd);
}

// ── Actions par slot ──────────────────────────────────────────────────────────
// KEEP  : même contenu + état compatible → rien à faire (phase loop survit)
// MORPH : même contenu, paramètres différents (gain/mode/semitones/muted/role)
// LEAVE : uniquement A
// ENTER : uniquement B (ou même asset mais slot différent → LEAVE+ENTER, pas de migration)
enum class SlotPlanAction : uint8_t { Keep = 0, Morph = 1, Leave = 2, Enter = 3 };

struct SlotPlan {
    SlotPlanAction action = SlotPlanAction::Keep;
    uint8_t  preloadIdx = 0xFF; // index pool préalloué si Enter (0xFF = N/A)
    float    gain = 1.f;        // target gain (Enter/Morph), current pour Leave
    float    curGain = 1.f;     // gain courant (Morph/Leave) — pour GainRamp cur→tgt
    float    semitones = 0.f;
    uint8_t  mode = 0;
    bool     muted = false;
    uint8_t  role = 255;
    AssetId  asset = 0;
};

// ── Événements planifiés (bornés, POD) ───────────────────────────────────────
// GainRamp : a = durée samples, b = target
// PcmFlip  : activation du PCM préchargé (pas de trigger)
// Release  : fade-out voice (après LEAVE)
// ModeSet/RoleSet/MuteSet : écritures atomiques à T
enum class PlanEventType : uint8_t { GainRamp = 0, PcmFlip = 1, Release = 2, ModeSet = 3, SemitoneSet = 4, RoleSet = 5, MuteSet = 6 };

struct PlanEvent {
    int64_t atSample = 0;
    PlanEventType type = PlanEventType::GainRamp;
    uint8_t slot = 0;
    float   a = 0.f, b = 0.f;
};

// ── Plan complet ─────────────────────────────────────────────────────────────
struct SceneTransitionPlan {
    int32_t fromScene = -1;
    int32_t toScene = -1;
    int64_t boundary = 0; // == executionSample, frontière sample-accurate
    SlotPlan  slots[kMaxSlots];
    PlanEvent events[64];
    uint8_t   numEvents = 0;
    bool      morphActive = false;
    float     morphFrom[4] = {};
    float     morphTo[4] = {};
    int64_t   morphStart = 0;
    int64_t   morphDur = 0; // samples
    bool      valid = false;
};

// Diff A→B par contenu (pas d'index). Pattern ne participe PAS à KEEP/MORPH du
// SlotPlayer : même asset + mêmes params player → KEEP, pattern B stagé à part.
// Documente exactement ce qui rend deux slots compatibles KEEP vs MORPH :
//   KEEP si : assetId !=0 identique ET mode== ET semitones== (bit-exact) ET
//             muted== ET gain== ET role== ET active des deux côtés.
//   Sinon si même asset → MORPH (param diff). Sinon LEAVE/ENTER.
//   Vide des deux côtés → KEEP (rien). Slot différent même asset → LEAVE+ENTER.
SlotPlanAction classifySlotForPlan(const SlotConfig& a, const SlotConfig& b,
                                   AssetId idA, AssetId idB) noexcept;

// Construit le plan DIRECT (sans BUILD/BREAKDOWN/DUB). Calcule les events
// GainRamp (durée dérivée de sampleRate), PcmFlip/Release/Mode/Role/Mute.
// Ne touche ni scheduler ni audio — pur POD. sampleRate pour durée GainRamp.
void buildDirectPlan(const SceneData& from, const SceneData& to,
                     int fromIdx, int toIdx, int64_t boundary,
                     double sampleRate,
                     SceneTransitionPlan& out) noexcept;

} // namespace engine
