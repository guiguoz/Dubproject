#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/SceneStore.h"
#include "engine/SceneEnergy.h"

using namespace engine;

// ─── SE1 : scène vide (aucun slot actif) → 0.0 ────────────────────────────────
TEST_CASE("SE1: empty scene scores 0.0", "[sceneenergy]") {
    SceneData sc;   // tous les slots inactifs par défaut
    REQUIRE(SceneEnergy::compute(sc) == 0.0f);
}

// ─── SE2 : 1 slot actif gain 1.0 → (0.5 + 0.5) / kMaxSlots ───────────────────
TEST_CASE("SE2: single active slot with gain 1.0", "[sceneenergy]") {
    SceneData sc;
    sc.slots[0].active   = true;
    sc.slots[0].filePath = "kick.wav";
    sc.slots[0].gain     = 1.0f;

    // sum = 0.5 + 0.5*1.0 = 1.0 → 1.0 / kMaxSlots
    const float expected = (0.5f + 0.5f * 1.0f) / static_cast<float>(kMaxSlots);
    REQUIRE(SceneEnergy::compute(sc) == Catch::Approx(expected).margin(0.0001f));
}

// ─── SE3 : slot actif mais filePath vide → ne contribue pas ──────────────────
TEST_CASE("SE3: active slot with empty filePath does not contribute", "[sceneenergy]") {
    SceneData sc;
    sc.slots[0].active   = true;
    sc.slots[0].filePath = "";   // actif sans fichier
    sc.slots[0].gain     = 1.0f;
    REQUIRE(SceneEnergy::compute(sc) == 0.0f);
}

// ─── SE4 : tous les slots actifs gain 1.0 → 1.0 ──────────────────────────────
TEST_CASE("SE4: all slots active with gain 1.0 scores 1.0", "[sceneenergy]") {
    SceneData sc;
    for (int i = 0; i < kMaxSlots; ++i) {
        sc.slots[i].active   = true;
        sc.slots[i].filePath = "track" + std::to_string(i) + ".wav";
        sc.slots[i].gain     = 1.0f;
    }
    // sum = 9 * 1.0 → clamp(9.0/9) = 1.0
    REQUIRE(SceneEnergy::compute(sc) == Catch::Approx(1.0f).margin(0.0001f));
}

// ─── SE5 : augmenter un gain ne diminue jamais le score ──────────────────────
TEST_CASE("SE5: raising a gain never decreases the score", "[sceneenergy]") {
    SceneData sc;
    for (int i = 0; i < 3; ++i) {
        sc.slots[i].active   = true;
        sc.slots[i].filePath = "track" + std::to_string(i) + ".wav";
        sc.slots[i].gain     = 0.3f;
    }

    const float before = SceneEnergy::compute(sc);
    sc.slots[1].gain = 0.9f;
    const float after = SceneEnergy::compute(sc);

    REQUIRE(after >= before);
    REQUIRE(after >  before);   // gain strictement augmenté → score strictement croissant
}
