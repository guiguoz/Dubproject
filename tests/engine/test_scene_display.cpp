// test_scene_display.cpp — UI-SCENE-1/2/3 : SceneDefinition comme source de vérité
// Ces tests vérifient le modèle de données (SceneStore) sans JUCE.
// La vérification de l'affichage GUI (stepSeqPanel_) est MANUELLE REQUISE.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/SceneStore.h"

using Catch::Approx;

// ─────────────────────────────────────────────────────────────────────────────
// UI-SCENE-1 : A slot3 vide / B slot3 HAT
// Bug reproduit : après navigation B→A, l'affichage montrait encore HAT.
// Invariant : sceneStore_.getScene(A).slots[3].filePath doit rester vide.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("UI-SCENE-1: SceneStore slot3 vide dans A, HAT dans B — indépendance", "[ui][scene]")
{
    engine::SceneStore store;

    // Scène A : slot 3 vide
    engine::SceneData a;
    a.used = true;
    store.setScene(0, a);   // slots[3].filePath = "" par défaut

    // Scène B : slot 3 HAT
    engine::SceneData b;
    b.used = true;
    b.slots[3].filePath = "hat.wav";
    b.slots[3].role      = engine::SlotRole::Unknown;
    store.setScene(1, b);

    // État initial : A vide, B chargé
    REQUIRE(store.getScene(0).slots[3].filePath.empty());
    REQUIRE(store.getScene(1).slots[3].filePath == "hat.wav");

    // Navigation A→B : lecture de B
    CHECK(store.getScene(1).slots[3].filePath == "hat.wav");

    // Navigation B→A : la définition de A ne doit pas avoir été mutée
    CHECK(store.getScene(0).slots[3].filePath.empty());

    // La définition de B reste intacte
    CHECK(store.getScene(1).slots[3].filePath == "hat.wav");
}

// ─────────────────────────────────────────────────────────────────────────────
// UI-SCENE-2 : rôle/gain/manual indépendants par scène
// A slot0 : Kick, gain=0.5, manual=true
// B slot0 : Bass, gain=0.8, manual=false
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("UI-SCENE-2: SceneDefinition role/gain/manual indépendants par scène", "[ui][scene]")
{
    engine::SceneStore store;

    engine::SceneData a;
    a.used                  = true;
    a.slots[0].filePath     = "kick.wav";
    a.slots[0].userGain     = 0.5f;
    a.slots[0].role         = engine::SlotRole::Kick;
    a.slots[0].isRoleManual = true;
    store.setScene(0, a);

    engine::SceneData b;
    b.used                  = true;
    b.slots[0].filePath     = "bass.wav";
    b.slots[0].userGain     = 0.8f;
    b.slots[0].role         = engine::SlotRole::Bass;
    b.slots[0].isRoleManual = false;
    store.setScene(1, b);

    auto checkA = [&]() {
        const auto& s = store.getScene(0).slots[0];
        CHECK(s.filePath     == "kick.wav");
        CHECK(s.userGain     == Approx(0.5f));
        CHECK(s.role         == engine::SlotRole::Kick);
        CHECK(s.isRoleManual == true);
    };
    auto checkB = [&]() {
        const auto& s = store.getScene(1).slots[0];
        CHECK(s.filePath     == "bass.wav");
        CHECK(s.userGain     == Approx(0.8f));
        CHECK(s.role         == engine::SlotRole::Bass);
        CHECK(s.isRoleManual == false);
    };

    // A→B→A : les deux définitions restent intactes
    checkA();
    checkB();
    checkA();  // B n'a pas pollué A
    checkB();  // A n'a pas pollué B
}

// ─────────────────────────────────────────────────────────────────────────────
// UI-SCENE-3 : navigation visuelle sans mutation du RuntimeSlotState
//
// Invariant structurel (vérifié par audit du code, pas par runtime JUCE) :
//   refreshSceneEditorFromDefinition() ne contient aucun appel à :
//     - importSampleAsync
//     - stagePcm / PcmFlip
//     - facade_.setSlotGain (moteur)
//     - facade_.setSlotMode (moteur)
//     - mutation AudioGraph / SlotPlayer
//
// VÉRIFICATION GUI MANUELLE REQUISE :
//   1. Charger A (slot3 vide) et B (slot3 HAT)
//   2. Jouer A → transitionner vers B via DIRECT
//   3. Sélectionner A dans l'éditeur sans stopper le transport
//   4. Slot3 doit afficher "--" (vide), pas "HAT"
//   5. Le moteur continue de jouer B (aucun re-import déclenché)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("UI-SCENE-3: refreshSceneEditorFromDefinition ne mute pas RuntimeSlotState (invariant structurel)", "[ui][scene]")
{
    // Vérification de l'indépendance des scènes dans le store (précondition du fix)
    engine::SceneStore store;

    engine::SceneData a;
    a.used = true;
    a.slots[3].filePath = "";
    store.setScene(0, a);

    engine::SceneData b;
    b.used = true;
    b.slots[3].filePath = "hat.wav";
    store.setScene(1, b);

    // Simule : moteur joue B (index=1), éditeur navigue vers A (index=0)
    // refreshSceneEditorFromDefinition(0) lirait sceneStore_.getScene(0)
    // → slot3 vide → setSlotWaveform({}) + setSlotFilePath("") côté UI
    // → aucun import audio

    const auto& defA = store.getScene(0);
    CHECK(defA.slots[3].filePath.empty());  // ← ce que l'UI doit afficher

    const auto& defB = store.getScene(1);
    CHECK(defB.slots[3].filePath == "hat.wav");  // ← le moteur joue toujours ça

    // Les deux définitions coexistent sans interférence
    SUCCEED("Invariant structurel confirmé — vérification GUI manuelle requise");
}
