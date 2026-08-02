#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/SceneStore.h"
#include "engine/TransitionEngine.h"
#include "engine/Transport.h"
#include "engine/EventScheduler.h"

using namespace engine;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static TransportState makeTS(int64_t samplePos = 0, double bpm = 120.0,
                              double sr = 44100.0) {
    TransportState ts;
    ts.samplePos      = samplePos;
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.playing        = true;
    return ts;
}

// Crée une SceneData avec N slots actifs (fichiers "fileX.wav")
static SceneData makeScene(int activeSlots, std::string namePrefix = "file") {
    SceneData sc;
    for (int i = 0; i < activeSlots && i < kMaxSlots; ++i) {
        sc.slots[i].active   = true;
        sc.slots[i].filePath = namePrefix + std::to_string(i) + ".wav";
        sc.slots[i].mode     = PlayMode::LoopSync;
    }
    sc.bpm = 120;
    return sc;
}

// Compte les EngineEvents d'un type donné pour un slot donné
static int countEvents(const EventScheduler& sched, EventType type, int slot) {
    int n = 0;
    for (int i = 0; i < sched.size(); ++i) {
        const auto& ev = sched.at(i);
        if (ev.type == type && static_cast<int>(ev.slot) == slot)
            ++n;
    }
    return n;
}

// ─── T-TX1 : requestTransition → ARMED ───────────────────────────────────────
TEST_CASE("T-TX1: requestTransition moves state to Armed", "[transition]") {
    SceneStore store;
    store.setScene(0, makeScene(3));
    store.setScene(1, makeScene(5));

    TransitionEngine te;
    REQUIRE(te.state() == TransitionEngine::State::Idle);

    const auto ts = makeTS(0);
    te.requestTransition(0, 1, store, ts);

    REQUIRE(te.state() == TransitionEngine::State::Armed);
    REQUIRE(te.plan().valid == true);
    REQUIRE(te.plan().fromScene == 0);
    REQUIRE(te.plan().toScene   == 1);
}

// ─── T-TX2 : slot KEEP → SlotAction::Keep ────────────────────────────────────
TEST_CASE("T-TX2: identical slot yields SlotAction::Keep", "[transition]") {
    SceneData a, b;
    // Slot 0 : identique dans A et B
    a.slots[0].active   = true;
    a.slots[0].filePath = "kick.wav";
    a.slots[0].mode     = PlayMode::LoopSync;
    a.slots[0].trimStart = 0;
    a.slots[0].trimEnd   = 0;
    a.slots[0].semitones = 0.0f;

    b.slots[0] = a.slots[0];   // copie exacte

    SceneStore store;
    store.setScene(0, a);
    store.setScene(1, b);

    TransitionEngine te;
    te.requestTransition(0, 1, store, makeTS(0));

    REQUIRE(te.plan().slotActions[0] == SlotAction::Keep);
}

// ─── T-TX3 : slot absent dans B → SlotAction::Exit ───────────────────────────
TEST_CASE("T-TX3: slot absent in B yields SlotAction::Exit", "[transition]") {
    SceneData a, b;
    a.slots[2].active   = true;
    a.slots[2].filePath = "snare.wav";
    // b.slots[2] non actif (défaut : active = false)

    SceneStore store;
    store.setScene(0, a);
    store.setScene(1, b);

    TransitionEngine te;
    te.requestTransition(0, 1, store, makeTS(0));

    REQUIRE(te.plan().slotActions[2] == SlotAction::Exit);
}

// ─── T-TX4 : slot différent → SlotAction::Replace ────────────────────────────
TEST_CASE("T-TX4: different file in both scenes yields SlotAction::Replace", "[transition]") {
    SceneData a, b;
    a.slots[1].active   = true;
    a.slots[1].filePath = "bass_A.wav";

    b.slots[1].active   = true;
    b.slots[1].filePath = "bass_B.wav";

    SceneStore store;
    store.setScene(0, a);
    store.setScene(1, b);

    TransitionEngine te;
    te.requestTransition(0, 1, store, makeTS(0));

    REQUIRE(te.plan().slotActions[1] == SlotAction::Replace);
}

// ─── T-TX5 : calme → musical → TransitionType::Build ─────────────────────────
TEST_CASE("T-TX5: calm-to-dense yields Build", "[transition]") {
    // Calme : < 0.3 × kMaxSlots = < 2.7 slots actifs → 2
    SceneData from = makeScene(2);
    // Musical : >= 0.3 × kMaxSlots = >= 2.7 → 3+
    SceneData to   = makeScene(5);

    REQUIRE(TransitionEngine::chooseType(from, to) == TransitionType::Build);
}

// ─── T-TX6 : musical → calme → TransitionType::Breakdown ────────────────────
TEST_CASE("T-TX6: dense-to-calm yields Breakdown", "[transition]") {
    SceneData from = makeScene(5);
    SceneData to   = makeScene(2);

    REQUIRE(TransitionEngine::chooseType(from, to) == TransitionType::Breakdown);
}

// ─── T-TX7 : ARMED → franchissement frontière → Executing ───────────────────
TEST_CASE("T-TX7: processBlock past boundary moves to Executing", "[transition]") {
    SceneStore store;
    store.setScene(0, makeScene(3));
    store.setScene(1, makeScene(3));

    TransitionEngine te;
    // Transport au step 0 → frontière dans 16 steps
    const auto ts0 = makeTS(0);
    te.requestTransition(0, 1, store, ts0);
    REQUIRE(te.state() == TransitionEngine::State::Armed);

    const int64_t boundary = te.plan().executionSample;

    EventScheduler sched;

    // Bloc avant la frontière → toujours Armed
    const auto tsBefore = makeTS(boundary - 1024);
    te.processBlock(tsBefore, sched);
    REQUIRE(te.state() == TransitionEngine::State::Armed);

    // Bloc à la frontière → Executing
    const auto tsAt = makeTS(boundary);
    te.processBlock(tsAt, sched);
    REQUIRE(te.state() == TransitionEngine::State::Executing);
}

// ─── T-TX8 : slot KEEP → aucun EngineEvent pour ce slot ─────────────────────
TEST_CASE("T-TX8: Keep slot generates no EngineEvent", "[transition]") {
    SceneData a, b;
    // Slot 0 identique → KEEP
    a.slots[0].active   = true;
    a.slots[0].filePath = "kick.wav";
    a.slots[0].mode     = PlayMode::LoopSync;
    b.slots[0]          = a.slots[0];

    // Slot 1 : absent dans B → EXIT (pour avoir au moins un event)
    a.slots[1].active   = true;
    a.slots[1].filePath = "snare.wav";

    SceneStore store;
    store.setScene(0, a);
    store.setScene(1, b);

    TransitionEngine te;
    const auto ts0 = makeTS(0);
    te.requestTransition(0, 1, store, ts0);

    const int64_t boundary = te.plan().executionSample;
    EventScheduler sched;
    const auto tsAt = makeTS(boundary);
    te.processBlock(tsAt, sched);

    // Aucun event de type Trigger, GainRamp ou Mute pour le slot 0 (KEEP)
    REQUIRE(countEvents(sched, EventType::Trigger,  0) == 0);
    REQUIRE(countEvents(sched, EventType::GainRamp, 0) == 0);
    REQUIRE(countEvents(sched, EventType::Mute,     0) == 0);

    // Slot 1 (EXIT) doit avoir un GainRamp
    REQUIRE(countEvents(sched, EventType::GainRamp, 1) >= 1);
}

// ─── T-TX9 : slot KEEP en LOOP SYNC → phase dérivée continue ────────────────
// Null-test : un SlotPlayer en LoopSync non interrompu produit la même sortie
// qu'une transition KEEP sans aucun EngineEvent généré.
// On vérifie ici que compilePlan ne pousse AUCUN event pour le slot KEEP,
// garantissant que la position dérivée n'est pas réinitialisée.
TEST_CASE("T-TX9: Keep LoopSync slot — phase continuity (no events)", "[transition]") {
    SceneData a, b;
    a.slots[0].active    = true;
    a.slots[0].filePath  = "loop.wav";
    a.slots[0].mode      = PlayMode::LoopSync;
    a.slots[0].loopBeats = 8;
    b.slots[0]           = a.slots[0];   // identique → KEEP

    SceneStore store;
    store.setScene(0, a);
    store.setScene(1, b);

    TransitionEngine te;
    te.requestTransition(0, 1, store, makeTS(0));

    const int64_t boundary = te.plan().executionSample;
    EventScheduler sched;
    te.processBlock(makeTS(boundary), sched);

    // Vérification : zéro event pour le slot 0 (KEEP LoopSync)
    int eventsForSlot0 = 0;
    for (int i = 0; i < sched.size(); ++i)
        if (static_cast<int>(sched.at(i).slot) == 0)
            ++eventsForSlot0;

    REQUIRE(eventsForSlot0 == 0);

    // Plan bien formé
    REQUIRE(te.plan().slotActions[0] == SlotAction::Keep);
}

// ─── T-TX extra : SceneStore getScene hors bornes ────────────────────────────
TEST_CASE("T-TX-extra1: SceneStore setScene/getScene roundtrip", "[scenestore]") {
    SceneStore store;
    SceneData  sc = makeScene(4, "track");
    sc.name = "TestScene";
    sc.bpm  = 140;
    store.setScene(3, sc);

    const SceneData& got = store.getScene(3);
    REQUIRE(got.name == "TestScene");
    REQUIRE(got.bpm  == 140);
    REQUIRE(got.slots[0].active   == true);
    REQUIRE(got.slots[0].filePath == "track0.wav");
}

TEST_CASE("T-TX-extra2: sceneDensity calculation", "[transition]") {
    SceneData sc = makeScene(3);   // 3/9 actifs
    const float d = TransitionEngine::sceneDensity(sc);
    REQUIRE(d == Catch::Approx(3.0f / 9.0f).margin(0.001f));
}

TEST_CASE("T-TX-extra3: Musical→Musical yields Smooth", "[transition]") {
    SceneData from = makeScene(5);
    SceneData to   = makeScene(7);
    REQUIRE(TransitionEngine::chooseType(from, to) == TransitionType::Smooth);
}

TEST_CASE("T-TX-extra4: Calm→Calm yields Smooth", "[transition]") {
    SceneData from = makeScene(1);   // 1/9 < 0.3
    SceneData to   = makeScene(2);   // 2/9 < 0.3
    REQUIRE(TransitionEngine::chooseType(from, to) == TransitionType::Smooth);
}
