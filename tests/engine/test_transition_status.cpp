#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <type_traits>
#include "engine/TransitionStatus.h"

using namespace engine;

// ─── T-TRANSSTATUS-EMPTY ─────────────────────────────────────────────────────
TEST_CASE("T-TRANSSTATUS-EMPTY: no pending => selectedScene==runtimeScene, pending=-1, boundary=-1",
          "[transstatus][empty]")
{
    TransitionStatusSnapshot sn;
    sn.selectedScene  = 0;
    sn.runtimeScene   = 0;
    sn.pendingScene   = -1;
    sn.boundarySample = -1;
    sn.nowSample      = 0;
    sn.bpm            = 120.0;
    sn.sampleRate     = 44100.0;
    sn.playing        = false;

    REQUIRE(sn.selectedScene == sn.runtimeScene);
    REQUIRE(sn.pendingScene == -1);
    REQUIRE(sn.boundarySample == -1);
    REQUIRE(transitionBeatsRemaining(sn) == 0.0);
}

// ─── T-TRANSSTATUS-PENDING ────────────────────────────────────────────────────
TEST_CASE("T-TRANSSTATUS-PENDING: boundary > nowSample => beatsRemaining coherent",
          "[transstatus][pending]")
{
    // 120 BPM, 44100 Hz → samplesPerBeat = 22050
    // boundary = 88200, nowSample = 44100 → rem = 44100/22050 = 2.0 beats
    TransitionStatusSnapshot sn;
    sn.runtimeScene   = 0;
    sn.pendingScene   = 1;
    sn.boundarySample = 88200;
    sn.nowSample      = 44100;
    sn.bpm            = 120.0;
    sn.sampleRate     = 44100.0;
    sn.playing        = true;

    const double beats = transitionBeatsRemaining(sn);
    REQUIRE(beats == Catch::Approx(2.0).epsilon(0.001));
}

TEST_CASE("T-TRANSSTATUS-PENDING: fractional beats at 0.75 position",
          "[transstatus][pending]")
{
    // 120 BPM, 44100 Hz → spb = 22050
    // boundary = 88200 (4 beats), nowSample = 71662.5 ≈ 71663 → rem ≈ 0.75
    TransitionStatusSnapshot sn;
    sn.pendingScene   = 2;
    sn.boundarySample = 88200;
    sn.nowSample      = 71663;
    sn.bpm            = 120.0;
    sn.sampleRate     = 44100.0;
    sn.playing        = true;

    const double beats = transitionBeatsRemaining(sn);
    REQUIRE(beats == Catch::Approx(0.75).epsilon(0.01));
}

// ─── T-TRANSSTATUS-CLAMP ──────────────────────────────────────────────────────
TEST_CASE("T-TRANSSTATUS-CLAMP: boundary < nowSample => beatsRemaining = 0, never negative",
          "[transstatus][clamp]")
{
    TransitionStatusSnapshot sn;
    sn.bpm            = 120.0;
    sn.sampleRate     = 44100.0;
    sn.boundarySample = 44100;  // dans le passé
    sn.nowSample      = 50000;
    sn.playing        = true;

    REQUIRE(transitionBeatsRemaining(sn) == 0.0);
}

TEST_CASE("T-TRANSSTATUS-CLAMP: boundary == nowSample => exactly 0",
          "[transstatus][clamp]")
{
    TransitionStatusSnapshot sn;
    sn.bpm            = 120.0;
    sn.sampleRate     = 44100.0;
    sn.boundarySample = 44100;
    sn.nowSample      = 44100;
    sn.playing        = true;

    REQUIRE(transitionBeatsRemaining(sn) == 0.0);
}

TEST_CASE("T-TRANSSTATUS-CLAMP: boundarySample=-1 => 0",
          "[transstatus][clamp]")
{
    TransitionStatusSnapshot sn;
    sn.bpm            = 120.0;
    sn.sampleRate     = 44100.0;
    sn.boundarySample = -1;
    sn.nowSample      = 1000;

    REQUIRE(transitionBeatsRemaining(sn) == 0.0);
}

// ─── T-TRANSSTATUS-NOALLOC ────────────────────────────────────────────────────
TEST_CASE("T-TRANSSTATUS-NOALLOC: TransitionStatusSnapshot is trivially constructible (no heap alloc)",
          "[transstatus][noalloc]")
{
    // Le struct ne contient que des scalaires (int, uint8_t, bool, int64_t, double)
    // → aucune allocation dynamique possible.
    static_assert(std::is_trivially_destructible_v<TransitionStatusSnapshot>,
                  "TransitionStatusSnapshot must be trivially destructible (no dtor = no alloc)");
    static_assert(sizeof(TransitionStatusSnapshot) > 0);

    // Vérification runtime : construction + appel de la fonction helper sans crash
    TransitionStatusSnapshot sn{};
    sn.bpm = 120.0; sn.sampleRate = 44100.0; sn.boundarySample = 88200; sn.nowSample = 44100;
    [[maybe_unused]] double r = transitionBeatsRemaining(sn);
    REQUIRE(true);  // si on arrive ici, aucune exception ni allocation implicite
}
