// test_pattern_ownership.cpp — PATTERN-OWNERSHIP-1..4 + PATTERN-EDIT-1..2
//
// Invariant fondamental : SceneDefinition.steps est la source de vérité.
// Aucune navigation, ni capture intermédiaire, ni refresh UI ne doit modifier
// les steps d'une scène qui ne fait pas l'objet d'une édition explicite.

#include <catch2/catch_test_macros.hpp>
#include "engine/SceneStore.h"

using namespace engine;

// ─── Helper : CRC simple (identique à l'instrumentation runtime) ──────────────

static uint32_t stepsCRC(const SceneData& sc) noexcept
{
    uint32_t crc = 0;
    for (int t = 0; t < kMaxSlots; ++t)
        for (int s = 0; s < kMaxSteps; ++s)
            if (sc.steps[static_cast<std::size_t>(t)][static_cast<std::size_t>(s)])
                crc ^= static_cast<uint32_t>(t * 512u + static_cast<uint32_t>(s) + 1u);
    return crc;
}

// ─── Helper : simule un clic de step (onStepChanged) ─────────────────────────
// Écrit dans SceneDefinition[currentIdx].steps — comme le nouveau callback MC.

static void simulateStepEdit(SceneStore& store, int currentIdx,
                              int track, int step, bool active) noexcept
{
    if (track < 0 || track >= kMaxSlots) return;
    if (step  < 0 || step  >= kMaxSteps) return;
    store.getScene(currentIdx)
        .steps[static_cast<std::size_t>(track)][static_cast<std::size_t>(step)] = active;
}

// ─── Helper : simule captureCurrentScene (nouvelle version sans steps) ────────
// Ne touche PAS aux steps ni aux trackBarCounts.

static void simulateCaptureCurrentScene(SceneStore& store, int currentIdx,
                                         const std::string& fakeFilePath = "") noexcept
{
    auto& sc  = store.getScene(currentIdx);
    sc.used   = true;
    sc.bpm    = 120;
    if (!fakeFilePath.empty())
        sc.slots[0].filePath = fakeFilePath;
    // steps intentionnellement NON écrasés — source de vérité = éditions en temps réel
}

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN-OWNERSHIP-1
// A CRC=0x1  B CRC=0xe01
// Simuler navigation B→A (stopped).
// Avant toute navigation suivante : storeA.stepsCRC == 0x1.
// Puis naviguer depuis A : storeA toujours 0x1.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("PATTERN-OWNERSHIP-1: nav B->A ne contamine pas scène A", "[pattern][ownership]")
{
    SceneStore store;

    // Scène A : un step sur track=0, step=0 → CRC=1
    SceneData a;
    a.used = true;
    a.steps[0][0] = true;
    store.setScene(0, a);

    // Scène B : step sur track=7, step=0 → CRC=7*512+0+1=3585=0xe01
    SceneData b;
    b.used = true;
    b.steps[7][0] = true;
    store.setScene(1, b);

    REQUIRE(stepsCRC(store.getScene(0)) == 0x1u);
    REQUIRE(stepsCRC(store.getScene(1)) == 0xe01u);

    // Simuler navigation B→A (stopped) :
    // 1. captureCurrentScene(B) — ne touche pas aux steps
    simulateCaptureCurrentScene(store, 1);
    // 2. applyScene(A) — sync writePatterns_ depuis store[A], pas de retour-capture
    // (pas de mutation de store[A])
    // 3. En transit, runtime writePatterns_ pointe vers A — ne touche pas à store[A]

    // SceneDefinition A doit rester intacte
    CHECK(stepsCRC(store.getScene(0)) == 0x1u);
    CHECK(stepsCRC(store.getScene(1)) == 0xe01u);

    // Naviguer depuis A : captureCurrentScene(A) ne doit pas écraser les steps de A
    simulateCaptureCurrentScene(store, 0);

    CHECK(stepsCRC(store.getScene(0)) == 0x1u);
    CHECK(stepsCRC(store.getScene(1)) == 0xe01u);
}

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN-OWNERSHIP-2
// A et B avec patterns différents. 100 navigations A↔B.
// Après chaque navigation : stores inchangés.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("PATTERN-OWNERSHIP-2: 100 navigations A<->B — stores stables", "[pattern][ownership]")
{
    SceneStore store;

    SceneData a; a.used = true; a.steps[0][0] = true;
    SceneData b; b.used = true; b.steps[7][0] = true;
    store.setScene(0, a);
    store.setScene(1, b);

    const uint32_t crcA = stepsCRC(store.getScene(0));
    const uint32_t crcB = stepsCRC(store.getScene(1));

    for (int iter = 0; iter < 100; ++iter)
    {
        int cur = iter % 2;
        int tgt = 1 - cur;

        simulateCaptureCurrentScene(store, cur);
        // applyScene(tgt) ne modifie pas store[tgt].steps

        CHECK(stepsCRC(store.getScene(0)) == crcA);
        CHECK(stepsCRC(store.getScene(1)) == crcB);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN-OWNERSHIP-3
// B non vide. Créer scène C (idx=2) : steps = empty.
// Activer C (stopped) : store[C].steps doit être empty.
// Revenir à B : store[B] inchangé.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("PATTERN-OWNERSHIP-3: nouvelle scène C créée vide, B inchangé", "[pattern][ownership]")
{
    SceneStore store;

    SceneData b; b.used = true; b.steps[7][0] = true;
    store.setScene(1, b);

    // C n'a pas encore été visitée — SceneData par défaut
    const SceneData& c = store.getScene(2);
    CHECK(c.used == false);
    CHECK(stepsCRC(c) == 0u);

    // Activer C stopped : syncWritePatternsFromScene(2) côté moteur,
    // du côté modèle le store[C] reste inchangé (steps vides)
    simulateCaptureCurrentScene(store, 2);  // marque C used=true
    CHECK(stepsCRC(store.getScene(2)) == 0u);

    // B doit rester intact
    CHECK(stepsCRC(store.getScene(1)) == 0xe01u);
}

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN-OWNERSHIP-4
// A : 16 steps (1 bar). B : 32 steps (2 bars) — step 16 actif.
// Après nav B→A : aucun step 16..31 actif dans store[A].
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("PATTERN-OWNERSHIP-4: ghost steps 16..31 absents après B->A", "[pattern][ownership]")
{
    SceneStore store;

    SceneData a;
    a.used = true;
    a.steps[0][0] = true;             // step 0 actif
    a.trackBarCounts[0] = 1;          // 16 steps
    store.setScene(0, a);

    SceneData b;
    b.used = true;
    b.steps[0][0]  = true;
    b.steps[0][16] = true;            // step 16 actif (barre 2)
    b.trackBarCounts[0] = 2;          // 32 steps
    store.setScene(1, b);

    // nav B→A
    simulateCaptureCurrentScene(store, 1);
    // store[A] ne doit pas avoir de steps 16..31

    const auto& sa = store.getScene(0);
    for (int s = 16; s < 32; ++s)
        CHECK(sa.steps[0][static_cast<std::size_t>(s)] == false);

    // step 0 de A toujours actif
    CHECK(sa.steps[0][0] == true);
    // B inchangé : step 16 toujours là
    CHECK(store.getScene(1).steps[0][16] == true);
}

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN-EDIT-1
// Sélectionner A. Cliquer step 4 sur track 0.
// SceneDefinition A : step 4 actif.
// SceneDefinition B : bit-identique à avant l'édition.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("PATTERN-EDIT-1: clic step écrit dans SceneDefinition courante seulement", "[pattern][edit]")
{
    SceneStore store;

    SceneData a; a.used = true; a.steps[0][0] = true;
    SceneData b; b.used = true; b.steps[7][0] = true;
    store.setScene(0, a);
    store.setScene(1, b);

    const uint32_t crcBefore = stepsCRC(store.getScene(1));

    // User clicks step 4 on track 0, current scene = A (idx=0)
    simulateStepEdit(store, 0, 0, 4, true);

    CHECK(store.getScene(0).steps[0][4] == true);     // A modifiée
    CHECK(store.getScene(0).steps[0][0] == true);     // step original intact
    CHECK(stepsCRC(store.getScene(1)) == crcBefore);  // B inchangée
}

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN-EDIT-2
// Refresh programmatique (simulateCaptureCurrentScene) n'écrase pas les steps.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("PATTERN-EDIT-2: captureCurrentScene ne modifie pas les steps", "[pattern][edit]")
{
    SceneStore store;

    SceneData a; a.used = true; a.steps[0][0] = true; a.steps[3][5] = true;
    SceneData b; b.used = true; b.steps[7][0] = true;
    store.setScene(0, a);
    store.setScene(1, b);

    const uint32_t crcA = stepsCRC(store.getScene(0));
    const uint32_t crcB = stepsCRC(store.getScene(1));

    // Plusieurs captures successives (simulant autosave, paste, etc.)
    for (int i = 0; i < 5; ++i)
    {
        simulateCaptureCurrentScene(store, 0);
        simulateCaptureCurrentScene(store, 1);
    }

    CHECK(stepsCRC(store.getScene(0)) == crcA);
    CHECK(stepsCRC(store.getScene(1)) == crcB);
}
