#pragma once
#include <array>
#include "engine/Sequencer.h"   // kMaxSteps

namespace engine {

// ── Séquenceur (compléments) ────────────────────────────────────────────────
static constexpr int kStepsPerBar = 16;
static constexpr int kTracks      = 9;

struct StepBuf {
    bool steps[kTracks][kMaxSteps] {};
    int  trackStepCount[kTracks] {};
};

// ── Arrêt des slots ────────────────────────────────────────────────────────
enum class StopMode : int {
    Normal    = 1,
    SceneSwap = 2,
    Retrigger = 3,
    Instant   = 4
};

// ── Division rythmique (grid sync) ─────────────────────────────────────────
enum class GridDiv { Eighth, Quarter, HalfBar, Bar };

} // namespace engine
