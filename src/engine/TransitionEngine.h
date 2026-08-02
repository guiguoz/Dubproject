#pragma once
#include <cstdint>
#include "engine/SceneStore.h"
#include "engine/Transport.h"
#include "engine/EventScheduler.h"

namespace engine {

// ─── Types de transition ─────────────────────────────────────────────────────

enum class TransitionType : uint8_t {
    Smooth    = 0,
    Build     = 1,
    Breakdown = 2,
    Dub       = 3,
    Cut       = 4,
};

// ─── Action par slot lors du diff A→B ────────────────────────────────────────

enum class SlotAction : uint8_t {
    Keep    = 0,   // même fichier + mode + trim + semitones → rien
    Exit    = 1,   // présent dans A, absent dans B → fade-out
    Enter   = 2,   // absent dans A, présent dans B → fade-in
    Replace = 3,   // présent dans A et B mais fichier différent → EXIT then ENTER
};

// ─── Plan de transition (POD) ────────────────────────────────────────────────

struct TransitionPlan {
    int            fromScene       = -1;
    int            toScene         = -1;
    TransitionType type            = TransitionType::Smooth;
    SlotAction     slotActions[kMaxSlots] = {};
    int64_t        executionSample = 0;   // sample transport de la frontière
    bool           valid           = false;
};

// ─── TransitionEngine ────────────────────────────────────────────────────────

class TransitionEngine {
public:
    enum class State : uint8_t {
        Idle,
        Preparing,
        Armed,
        Executing,
        Settling,
    };

    // Demande une transition vers la scène `toScene`.
    // Calcule le diff, choisit le type, prépare le plan.
    // La frontière d'exécution est la prochaine frontière de 16 steps
    // calculée depuis le transport.
    void requestTransition(int fromScene, int toScene,
                           const SceneStore& store,
                           const TransportState& ts) noexcept;

    // Appelé à chaque bloc audio.
    // Si ARMED et transportPos >= executionSample, compile les EngineEvents
    // et les pousse au scheduler, puis passe en Executing.
    // Si Executing et tous les fades sont finis (après kSettleBlocks blocs),
    // passe en Settling puis Idle.
    void processBlock(const TransportState& ts,
                      EventScheduler& scheduler) noexcept;

    State               state() const noexcept { return state_; }
    const TransitionPlan& plan() const noexcept { return plan_; }

    // Densité d'une scène : fraction de slots actifs [0, 1].
    static float sceneDensity(const SceneData& scene) noexcept;

    // Choisit le type de transition selon les densités de A et B.
    static TransitionType chooseType(const SceneData& from,
                                     const SceneData& to) noexcept;

private:
    State          state_ = State::Idle;
    TransitionPlan plan_;
    int            settleBlocksLeft_ = 0;

    // Seuil densité : en-dessous → « calme », au-dessus → « musical »
    static constexpr float kDensityThreshold = 0.3f;

    // Nombre de blocs à attendre en phase Executing avant de passer en Settling
    static constexpr int kSettleBlocks = 4;

    // Durée du fade de sortie/entrée : ~10 ms à 44100 Hz
    static constexpr float kFadeOutDur  = 441.0f;   // param a = durée en samples
    static constexpr float kFadeInStart = 441.0f;   // param b = target gain

    // Compile le plan en EngineEvents et les pousse dans le scheduler.
    void compilePlan(EventScheduler& scheduler, int64_t boundary) noexcept;

    // Calcule le diff slot à slot entre A et B.
    static SlotAction diffSlot(const SlotConfig& a, const SlotConfig& b) noexcept;
};

} // namespace engine
