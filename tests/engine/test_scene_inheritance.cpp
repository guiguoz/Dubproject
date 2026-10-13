#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/SceneStore.h"
#include "engine/SceneEnergy.h"

using namespace engine;

// ─── Helpers ────────────────────────────────────────────────────────────────

static SceneData makeOwned(const std::string& path, float gain = 1.f,
                           bool step0 = false, int barCount = 1)
{
    SceneData d;
    d.slotOwnership.fill(true);
    d.sceneOwned = true;
    d.used = true;
    d.slots[0].filePath = path;
    d.slots[0].gain     = gain;
    d.steps[0][0]       = step0;
    d.trackBarCounts[0] = barCount;
    return d;
}

// ─── T-INH1 : définition en A, héritage en B..H ────────────────────────────
TEST_CASE("T-INH1: slot defined in A, inherited by B..H", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    a.slots[0].gain     = 0.8f;
    a.steps[0][0] = a.steps[0][1] = a.steps[0][2] = a.steps[0][3] = true;
    a.trackBarCounts[0] = 2;
    a.used = true;
    store.setScene(0, a);
    // scenes 1-7 : all Inherit (default)

    for (int i = 1; i < 8; ++i) {
        const SceneData r = store.resolveScene(i);
        REQUIRE(r.slots[0].filePath == "kick.wav");
        REQUIRE(r.slots[0].gain == Catch::Approx(0.8f));
        REQUIRE(r.steps[0][0] == true);
        REQUIRE(r.trackBarCounts[0] == 2);
        REQUIRE(store.getScene(i).slotOwnership[0] == false);  // store unchanged
    }
}

// ─── T-INH2 : scène A vide, définition en B ─────────────────────────────────
TEST_CASE("T-INH2: A empty, B defines slot", "[inherit]") {
    SceneStore store;
    // A : Inherit (no Own set), so resolveScene(0) returns defaults
    SceneData b;
    b.slotOwnership[0] = true;
    b.slots[0].filePath = "kick.wav";
    b.used = true;
    store.setScene(1, b);

    REQUIRE(store.resolveScene(0).slots[0].filePath.empty());
    REQUIRE(store.resolveScene(1).slots[0].filePath == "kick.wav");
    REQUIRE(store.resolveScene(2).slots[0].filePath == "kick.wav");
    REQUIRE(store.resolveScene(7).slots[0].filePath == "kick.wav");
}

// ─── T-INH3 : modification en milieu de chaîne ──────────────────────────────
TEST_CASE("T-INH3: mid-chain modification breaks inheritance", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    a.used = true;
    store.setScene(0, a);

    SceneData b;
    b.slotOwnership[0] = true;
    b.slots[0].filePath = "kick_modified.wav";
    b.steps[0][0] = true;
    b.used = true;
    store.setScene(1, b);
    // scenes 2..7 : Inherit

    REQUIRE(store.resolveScene(2).slots[0].filePath == "kick_modified.wav");
    REQUIRE(store.resolveScene(1).slots[0].filePath == "kick_modified.wav");
    REQUIRE(store.resolveScene(0).slots[0].filePath == "kick.wav");
}

// ─── T-INH4 : vidage explicite (Own vide coupe l'héritage) ──────────────────
TEST_CASE("T-INH4: explicit empty Own cuts inheritance downstream", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    a.used = true;
    store.setScene(0, a);

    SceneData cut;
    cut.slotOwnership[0] = true;
    cut.slots[0].filePath = "";  // explicit empty Own
    cut.used = true;
    store.setScene(2, cut);
    // scenes 1, 3..7 : Inherit

    REQUIRE(store.resolveScene(1).slots[0].filePath == "kick.wav");  // before the cut
    REQUIRE(store.resolveScene(3).slots[0].filePath.empty());        // after the cut
    REQUIRE(store.resolveScene(7).slots[0].filePath.empty());
    REQUIRE(store.resolveScene(2).slots[0].filePath.empty());
}

// ─── T-INH5 : granularité par slot ──────────────────────────────────────────
TEST_CASE("T-INH5: per-slot granularity", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = a.slotOwnership[1] = true;
    a.slots[0].filePath = "kick.wav";
    a.slots[1].filePath = "bass.wav";
    a.used = true;
    store.setScene(0, a);

    SceneData b;
    b.slotOwnership[0] = true;   // owns slot 0
    b.slots[0].filePath = "kick2.wav";
    // slot 1 : Inherit
    b.used = true;
    store.setScene(1, b);

    const SceneData r = store.resolveScene(2);
    REQUIRE(r.slots[0].filePath == "kick2.wav");  // from scene 1
    REQUIRE(r.slots[1].filePath == "bass.wav");   // from scene 0
}

// ─── T-INH6 : pureté et déterminisme ────────────────────────────────────────
TEST_CASE("T-INH6: resolveScene is pure and deterministic", "[inherit]") {
    SceneStore store;
    store.setScene(0, makeOwned("kick.wav", 0.8f, true));

    const SceneData r1 = store.resolveScene(3);
    const SceneData r2 = store.resolveScene(3);

    REQUIRE(r1.slots[0].filePath == r2.slots[0].filePath);
    REQUIRE(r1.slots[0].gain == r2.slots[0].gain);
    REQUIRE(r1.steps[0][0] == r2.steps[0][0]);

    // Store must be unchanged
    REQUIRE(store.getScene(3).slotOwnership[0] == false);
    REQUIRE(store.getScene(3).slots[0].filePath.empty());
}

// ─── T-INH7 : matérialisation conservatrice ─────────────────────────────────
TEST_CASE("T-INH7: materializeSlot copies resolved content exactly", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    a.slots[0].gain     = 0.7f;
    a.steps[0][0] = a.steps[0][2] = true;
    a.trackBarCounts[0] = 2;
    a.used = true;
    store.setScene(0, a);

    const SceneData before = store.resolveScene(1);
    store.materializeSlot(1, 0);

    REQUIRE(store.getScene(1).slotOwnership[0] == true);
    REQUIRE(store.getScene(1).used == true);
    REQUIRE(store.getScene(1).slots[0].filePath == before.slots[0].filePath);
    REQUIRE(store.getScene(1).slots[0].gain == Catch::Approx(before.slots[0].gain));
    REQUIRE(store.getScene(1).steps[0][0]  == before.steps[0][0]);
    REQUIRE(store.getScene(1).steps[0][2]  == before.steps[0][2]);
    REQUIRE(store.getScene(1).trackBarCounts[0] == before.trackBarCounts[0]);
    // active : recalculé (même résultat que resolveScene ici)
    REQUIRE(store.getScene(1).slots[0].active == before.slots[0].active);

    // Idempotent
    store.materializeSlot(1, 0);
    REQUIRE(store.getScene(1).slotOwnership[0] == true);
}

// ─── T-INH8 : active dérivé ─────────────────────────────────────────────────
TEST_CASE("T-INH8: active is derived from filePath+steps, not copied", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    // no steps → active=false
    a.used = true;
    store.setScene(0, a);

    // Scene 1 inherits slot 0 → active must be false (no steps)
    REQUIRE(store.resolveScene(1).slots[0].active == false);

    // Now scene 1 materializes and adds a step
    store.materializeSlot(1, 0);
    store.getScene(1).steps[0][0] = true;

    // resolveScene(1) after the step: active=true (has filePath + step)
    REQUIRE(store.resolveScene(1).slots[0].active == true);
    // Scene 0 must be unchanged
    REQUIRE(store.resolveScene(0).slots[0].active == false);
}

// ─── T-INH9 : énergie de scène via résolution ───────────────────────────────
TEST_CASE("T-INH9: SceneEnergy of inherited scene equals energy of owner", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    a.slots[0].gain     = 1.0f;
    a.steps[0][0] = true;
    a.used = true;
    store.setScene(0, a);

    const float eA = SceneEnergy::compute(store.resolveScene(0));
    const float eB = SceneEnergy::compute(store.resolveScene(3));  // inherits from A

    REQUIRE(eB == Catch::Approx(eA));
    REQUIRE(eB > 0.f);
}

// ─── T-INH10 : migration v<26 → identité ────────────────────────────────────
// Simule l'état post-migration : tous les slots Own → resolveScene(i) == getScene(i).
TEST_CASE("T-INH10: when all Own, resolveScene is identity", "[inherit]") {
    SceneStore store;

    for (int i = 0; i < 8; ++i) {
        SceneData sc;
        sc.slotOwnership.fill(true);
        sc.sceneOwned = true;
        sc.used = true;
        for (int s = 0; s < kMaxSlots; ++s)
            sc.slots[static_cast<std::size_t>(s)].filePath =
                "file" + std::to_string(i) + "_" + std::to_string(s) + ".wav";
        store.setScene(i, sc);
    }

    for (int i = 0; i < 8; ++i) {
        const SceneData r = store.resolveScene(i);
        for (int s = 0; s < kMaxSlots; ++s) {
            const std::size_t su = static_cast<std::size_t>(s);
            REQUIRE(r.slots[su].filePath == store.getScene(i).slots[su].filePath);
        }
    }
}

// ─── T-INH11 : LOT B — dub delay hérité par scène ──────────────────────────
TEST_CASE("T-INH11: DubLevel inheritance via resolveSceneLevel", "[inherit]") {
    SceneStore store;

    // A : sceneOwned, wet=0.5
    SceneData a;
    a.sceneOwned = true;
    a.dubDelayWet = 0.5f;
    a.used = true;
    store.setScene(0, a);
    // B, C : Inherit (sceneOwned=false)

    REQUIRE(store.resolveSceneLevel(1).wet == Catch::Approx(0.5f));
    REQUIRE(store.resolveSceneLevel(2).wet == Catch::Approx(0.5f));

    // B becomes owner (wet=0.1)
    SceneData b = store.getScene(1);
    b.sceneOwned      = true;
    b.dubDelayWet     = 0.1f;
    b.used            = true;
    store.setScene(1, b);

    REQUIRE(store.resolveSceneLevel(2).wet == Catch::Approx(0.1f));  // C follows B now
    REQUIRE(store.resolveSceneLevel(0).wet == Catch::Approx(0.5f));  // A unchanged

    // D : sceneOwned with default values
    SceneData d;
    d.sceneOwned = true;
    // dubDelayWet = 0.28f (default)
    d.used = true;
    store.setScene(3, d);

    REQUIRE(store.resolveSceneLevel(4).wet == Catch::Approx(0.28f));  // E follows D
}

// ─── T-INH12 : Serum hors héritage ──────────────────────────────────────────
// La résolution de niveau (DubLevel) ne contient aucun champ Serum.
TEST_CASE("T-INH12: Serum fields are not part of DubLevel resolution", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.sceneOwned = true;
    a.serumState      = "some_state_data";
    a.serumPresetName = "MyPreset";
    a.serumGain       = 1.5f;
    a.dubDelayWet     = 0.3f;
    a.used = true;
    store.setScene(0, a);

    // Scene B inherits
    const DubLevel lvl = store.resolveSceneLevel(1);
    REQUIRE(lvl.wet == Catch::Approx(0.3f));
    // DubLevel has no serum fields — compile-time guarantee via struct definition

    // resolveScene(1) preserves scene 1's own Serum fields (empty = not set)
    const SceneData r = store.resolveScene(1);
    REQUIRE(r.serumState.empty());      // scene 1 has no preset
    REQUIRE(r.serumPresetName.empty()); // scene 1 has no preset name
}

// ─── T-INH13 : pureté du niveau scène ───────────────────────────────────────
TEST_CASE("T-INH13: resolveSceneLevel does not modify ownership", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.sceneOwned = true;
    a.dubDelayWet = 0.4f;
    a.used = true;
    store.setScene(0, a);

    const DubLevel r1 = store.resolveSceneLevel(2);
    const DubLevel r2 = store.resolveSceneLevel(2);

    REQUIRE(r1 == r2);
    // Store ownership unchanged
    REQUIRE(store.getScene(2).sceneOwned == false);
}

// ─── T-W2 : matérialise + clear steps → active=false, propagation filePath ───
TEST_CASE("T-W2: materializeSlot then clear steps -> active=false, downstream inherits filePath", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "bass.wav";
    a.slots[0].gain     = 1.0f;
    a.steps[0][0] = a.steps[0][1] = true;
    a.trackBarCounts[0] = 1;
    a.used = true;
    store.setScene(0, a);

    // Matérialise slot 0 en scène 1 (Inherit → Own)
    store.materializeSlot(1, 0);
    REQUIRE(store.getScene(1).slotOwnership[0] == true);
    REQUIRE(store.getScene(1).slots[0].filePath == "bass.wav");

    // Vider les steps de la scène 1
    store.getScene(1).steps[0].fill(false);
    store.getScene(1).slots[0].active = SceneStore::computeSlotActive(store.getScene(1), 0);

    REQUIRE(store.getScene(1).slots[0].active == false);           // steps vidés
    REQUIRE(store.getScene(1).slots[0].filePath == "bass.wav");    // filePath conservé

    // La scène 2 (Inherit) doit hériter de la scène 1 (Own vide)
    const SceneData r2 = store.resolveScene(2);
    REQUIRE(r2.slots[0].filePath == "bass.wav");   // filePath de l'ancêtre le plus proche (scène 1)
    REQUIRE(r2.slots[0].active == false);           // steps vides → inactive
}

// ─── T-W3 : computeSlotActive == règle de resolveScene ──────────────────────
TEST_CASE("T-W3: computeSlotActive is consistent with resolveScene phase-2", "[inherit]") {
    SceneStore store;
    SceneData a;
    a.slotOwnership[0] = true;
    a.slots[0].filePath = "kick.wav";
    a.steps[0][0] = true;
    a.trackBarCounts[0] = 1;
    a.used = true;
    store.setScene(0, a);

    // resolveScene calcule active via computeSlotActive
    const SceneData r0 = store.resolveScene(0);
    const bool activeFromResolve = r0.slots[0].active;
    const bool activeFromHelper  = SceneStore::computeSlotActive(r0, 0);
    REQUIRE(activeFromResolve == activeFromHelper);
    REQUIRE(activeFromResolve == true);  // filePath non vide + step actif

    // Cas vide : filePath vide → false
    SceneData empty;
    empty.slotOwnership[0] = true;
    empty.steps[0][0] = true;  // step actif mais filePath vide
    store.setScene(1, empty);
    const SceneData r1 = store.resolveScene(1);
    REQUIRE(SceneStore::computeSlotActive(r1, 0) == false);
    REQUIRE(r1.slots[0].active == false);
}
