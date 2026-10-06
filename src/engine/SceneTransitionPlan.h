#pragma once
// SceneTransitionPlan — plan immutable PREPARE → COMMIT audio sample-accurate → CLEANUP.
// POD, préalloué, borné. Aucune allocation/string/comparaison de path en audio.
#include <cstdint>
#include <string>
#include <array>
#include <algorithm>
#include "engine/SceneStore.h"
#include "engine/Transport.h"   // nextCycleStep / sampleOfStep (frontière de cycle)

namespace engine {

// ── Frontière de transition de scène (règle unique : fin du cycle courant) ───
//
// Cycle d'une scène = longueur de sa plus longue piste, en steps. Les patterns
// d'une même scène peuvent avoir des longueurs différentes ; la plus longue
// borne le cycle (même définition que le `sceneLen` de la V1 : les bar counts
// de l'UI étant en pratique des multiples — 1/2/4/8/16/32 — le plus long est
// aussi le PPCM de tous, donc toutes les pistes retombent sur leur step 0).
inline int sceneCycleSteps(const SceneData& sc) noexcept {
    int len = 16;   // une mesure minimum : jamais de bascule infra-mesure
    for (int i = 0; i < kMaxSlots; ++i) {
        const int bars = sc.trackBarCounts[static_cast<std::size_t>(i)];
        if (bars > 0) len = std::max(len, bars * 16);
    }
    return len;
}

// Frontière d'exécution d'une transition depuis `from` : fin du cycle en cours.
// `step` = index global (monotone) du step frontière ; `sample` = son sample
// absolu ; `cycleSteps` = longueur de cycle retenue (UI / durée d'attente).
struct TransitionBoundary {
    int64_t step       = 0;
    int64_t sample     = 0;
    int     cycleSteps = 16;
};

inline TransitionBoundary planTransitionBoundary(const TransportState& ts,
                                                 const SceneData& from) noexcept {
    TransitionBoundary b{};
    b.cycleSteps = sceneCycleSteps(from);
    b.step       = nextCycleStep(ts, b.cycleSteps);
    b.sample     = sampleOfStep(ts, b.step);
    return b;
}

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
// SendRamp : a = durée samples, b = target send linéaire [0,1] (DUB modifier)
enum class PlanEventType : uint8_t { GainRamp = 0, PcmFlip = 1, Release = 2, ModeSet = 3, SemitoneSet = 4, RoleSet = 5, MuteSet = 6, SendRamp = 7 };

struct PlanEvent {
    int64_t atSample = 0;
    PlanEventType type = PlanEventType::GainRamp;
    uint8_t slot = 0;
    float   a = 0.f, b = 0.f;
};

// ── Mode DUB (modifier de plan) ───────────────────────────────────────────────
enum class DubMode : uint8_t { Auto = 0, Force = 1, Disable = 2 };

// ── Plan complet ─────────────────────────────────────────────────────────────
struct SceneTransitionPlan {
    int32_t fromScene = -1;
    int32_t toScene = -1;
    int64_t boundary = 0; // == executionSample, frontière sample-accurate
    SlotPlan  slots[kMaxSlots];
    PlanEvent events[96];  // 64 base + 32 DUB (2 SendRamp × 9 slots × marge)
    uint8_t   numEvents = 0;
    bool      morphActive = false;
    float     morphFrom[4] = {};
    float     morphTo[4] = {};
    int64_t   morphStart = 0;
    int64_t   morphDur = 0; // samples
    bool      valid = false;
    uint8_t   policy = 0;    // 0=DIRECT, 1=BUILD, 2=BREAKDOWN (propagé au snapshot)
    bool      dubActive = false;
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

// Applique le modificateur DUB au plan existant (après buildDirectPlan + TransitionPolicy::apply).
// Ajoute des events SendRamp pour les slots LEAVE éligibles.
// from/to : scènes source et destination (pour steps et mode OneShot).
// samplesPerStep : pour la fenêtre feed-check OneShot (0 = désactive le check).
void applyDubModifier(SceneTransitionPlan& plan,
                      const SceneData& from, const SceneData& to,
                      float sampleRate, double samplesPerStep,
                      DubMode mode) noexcept;

} // namespace engine
