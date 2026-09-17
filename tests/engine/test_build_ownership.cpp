// test_build_ownership.cpp — BUILD slot event ownership
//
// Invariant principal :
//   TransitionPolicy::apply() peut décaler atSample et réordonner les events
//   par atSample (re-sort), mais ne doit JAMAIS modifier event.slot ni
//   transférer l'ownership d'un event vers un autre slot.
//
//   La comparaison DOIT être par comptage (events per slot), pas par position,
//   car le re-sort final change l'ordre des events dans le tableau.
//
//   KEEP slots : aucun PcmFlip, aucun GainRamp de transition, aucun Release.
//   → slotActiveAt ne sera jamais positionné pour un KEEP slot
//   → GateRejected KEEP == 0 par construction.
//
// Résultat attendu sur HEAD : BUILD BUG NOT REPRODUCED.
// apply() ne modifie jamais event.slot — seul atSample est décalé.

#include <catch2/catch_test_macros.hpp>
#include "engine/TransitionPolicy.h"
#include "engine/SceneTransitionPlan.h"
#include "engine/Sequencer.h"
#include <array>
#include <vector>

using namespace engine;

// ─────────────────────────────────────────────────────────────────────────────
// Helper : scène avec N slots actifs, roles assignés individuellement.
// ─────────────────────────────────────────────────────────────────────────────
static SceneData makeScene(const std::vector<std::pair<std::string, SlotRole>>& slots)
{
    SceneData sc;
    sc.used = true;
    for (int i = 0; i < kMaxSlots; ++i) {
        sc.slots[i].active = false;
        sc.steps[i].fill(false);
        sc.trackBarCounts[i] = 1;
    }
    for (int i = 0; i < (int)slots.size() && i < kMaxSlots; ++i) {
        sc.slots[i].filePath = slots[i].first;
        sc.slots[i].active   = !slots[i].first.empty();
        sc.slots[i].gain     = 1.f;
        sc.slots[i].role     = slots[i].second;
    }
    return sc;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildPcmFlipOffsetsStayWithTheirSlot
//
// A : KICK (slot0) + BASS (slot1)   → KEEP
// B : KICK + BASS + HAT + PERC + PAD + FX  → KEEP(0,1) + ENTER(2,3,4,5)
//
// Après apply(), le re-sort réordonne les events par atSample.
// Le champ event.slot n'est JAMAIS modifié par apply() — seul atSample change.
//
// Vérification correcte : compter les events par slot avant et après apply.
// La distribution (slot → count) doit être identique.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("BuildPcmFlipOffsetsStayWithTheirSlot", "[policy][build][ownership]")
{
    SceneData from = makeScene({
        {"kick.wav", SlotRole::Kick},
        {"bass.wav", SlotRole::Bass},
    });
    SceneData to = makeScene({
        {"kick.wav",  SlotRole::Kick},
        {"bass.wav",  SlotRole::Bass},
        {"hat.wav",   SlotRole::Unknown},
        {"perc.wav",  SlotRole::Perc},
        {"pad.wav",   SlotRole::Pad},
        {"fx.wav",    SlotRole::Fx},
    });

    const int64_t T  = 10000;
    const double  sr = 44100.0;

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, T, sr, plan);
    REQUIRE(plan.valid);

    PolicyContext ctx{sr, 120.0, 4, 4, T};
    REQUIRE(TransitionPolicy::choose(plan) == PolicyType::Build);

    // ── Capturer la distribution slot→count avant apply() ───────────────────
    // (La comparaison DOIT être par count, pas par position : le re-sort final
    // dans apply() réordonne les events par atSample sans modifier event.slot.)
    std::array<int, kMaxSlots> countBefore{};
    for (int i = 0; i < plan.numEvents; ++i)
        countBefore[plan.events[i].slot]++;

    TransitionPolicy::apply(plan, ctx);

    // ── Invariant A : le nombre d'events par slot est inchangé ──────────────
    // apply() ne modifie jamais event.slot (seul atSample est décalé).
    std::array<int, kMaxSlots> countAfter{};
    for (int i = 0; i < plan.numEvents; ++i)
        countAfter[plan.events[i].slot]++;

    for (int s = 0; s < kMaxSlots; ++s) {
        INFO("slot=" << s << " before=" << countBefore[s] << " after=" << countAfter[s]);
        CHECK(countAfter[s] == countBefore[s]);
    }

    // ── Invariant B : KEEP slots (0=KICK, 1=BASS) — zéro event ─────────────
    // Garantit GateRejected==0 pour KEEP : slotActiveAt ne sera jamais repoussé.
    CHECK(plan.slots[0].action == SlotPlanAction::Keep);
    CHECK(plan.slots[1].action == SlotPlanAction::Keep);
    int eventsOnKeep = 0;
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].slot == 0 || plan.events[i].slot == 1)
            eventsOnKeep++;
    CHECK(eventsOnKeep == 0);

    // ── Invariant C : chaque ENTER slot a son propre PcmFlip (slot==S) ──────
    for (int s = 2; s <= 5; ++s) {
        REQUIRE(plan.slots[s].action == SlotPlanAction::Enter);
        bool foundFlip = false;
        for (int i = 0; i < plan.numEvents; ++i) {
            if (plan.events[i].type == PlanEventType::PcmFlip
                && plan.events[i].slot == s)
            {
                CHECK(plan.events[i].slot == s);     // ownership strict
                CHECK(plan.events[i].atSample >= T); // activation après T
                foundFlip = true;
            }
        }
        INFO("ENTER slot " << s << " : PcmFlip manquant");
        CHECK(foundFlip);
    }

    // ── Invariant D : 4 activations distinctes et >= T ──────────────────────
    std::vector<int64_t> flipTimes;
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].type == PlanEventType::PcmFlip)
            flipTimes.push_back(plan.events[i].atSample);
    REQUIRE(flipTimes.size() == 4u);
    for (int64_t t : flipTimes)
        CHECK(t >= T);

    // Vérifier que toutes les activations sont distinctes (pas de collision)
    std::sort(flipTimes.begin(), flipTimes.end());
    for (size_t i = 1; i < flipTimes.size(); ++i) {
        INFO("flipTimes[" << i << "]=" << flipTimes[i]
             << " flipTimes[" << (i-1) << "]=" << flipTimes[i-1]);
        CHECK(flipTimes[i] > flipTimes[i - 1]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stress A→B→A→B ×100 : KEEP slots ne reçoivent jamais d'events
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("BuildPcmFlipOwnership: stress A->B->A->B x100", "[policy][build][ownership][stress]")
{
    SceneData a = makeScene({
        {"kick.wav", SlotRole::Kick},
        {"bass.wav", SlotRole::Bass},
    });
    SceneData b = makeScene({
        {"kick.wav", SlotRole::Kick},
        {"bass.wav", SlotRole::Bass},
        {"hat.wav",  SlotRole::Unknown},
        {"perc.wav", SlotRole::Perc},
        {"pad.wav",  SlotRole::Pad},
        {"fx.wav",   SlotRole::Fx},
    });

    const double sr = 44100.0;
    int64_t T = 10000;

    for (int iter = 0; iter < 100; ++iter) {
        const SceneData& from = (iter % 2 == 0) ? a : b;
        const SceneData& to   = (iter % 2 == 0) ? b : a;
        T += 22050;

        SceneTransitionPlan plan;
        buildDirectPlan(from, to, iter % 2, (iter + 1) % 2, T, sr, plan);
        if (!plan.valid) continue;

        PolicyContext ctx{sr, 120.0, 4, 4, T};
        TransitionPolicy::apply(plan, ctx);

        // Distribution slot→count identique avant/après est garantie par apply()
        // On vérifie l'invariant KEEP : zéro event pour les slots KEEP
        for (int i = 0; i < plan.numEvents; ++i) {
            const int s = plan.events[i].slot;
            if (plan.slots[s].action == SlotPlanAction::Keep) {
                FAIL("iter=" << iter << " slot=" << s
                     << " est KEEP mais reçoit event type="
                     << (int)plan.events[i].type);
            }
        }

        // Chaque event.slot est dans un domaine valide
        for (int i = 0; i < plan.numEvents; ++i)
            CHECK(plan.events[i].slot < kMaxSlots);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GateRejected KEEP == 0 — preuve structurelle
//
// Après BUILD apply(), KEEP slots ont zéro event dans le plan.
// TransitionEngine n'appellera donc jamais setSlotActiveAt pour ces slots.
// slotActiveAt[KEEP] reste 0 → gate toujours ouvert → GateRejected == 0.
//
// On vérifie la condition structurelle (aucun event KEEP) qui garantit
// l'invariant à l'exécution sans nécessiter une simulation complète.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("GateRejectedKEEP == 0: aucun event KEEP dans le plan BUILD", "[policy][build][ownership][gate]")
{
    SceneData from = makeScene({
        {"kick.wav", SlotRole::Kick},
        {"bass.wav", SlotRole::Bass},
    });
    // 6 slots pour obtenir BUILD (net ENTER = 4 >= 3, keepMorph = 2 >= 2)
    SceneData to = makeScene({
        {"kick.wav", SlotRole::Kick},
        {"bass.wav", SlotRole::Bass},
        {"hat.wav",  SlotRole::Unknown},
        {"perc.wav", SlotRole::Perc},
        {"pad.wav",  SlotRole::Pad},
        {"fx.wav",   SlotRole::Fx},
    });

    const int64_t T  = 44100;
    const double  sr = 44100.0;
    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, T, sr, plan);
    REQUIRE(plan.valid);

    PolicyContext ctx{sr, 120.0, 4, 4, T};
    REQUIRE(TransitionPolicy::choose(plan) == PolicyType::Build);
    TransitionPolicy::apply(plan, ctx);

    // Précondition : slots 0 et 1 sont KEEP
    REQUIRE(plan.slots[0].action == SlotPlanAction::Keep);
    REQUIRE(plan.slots[1].action == SlotPlanAction::Keep);

    // Aucun event ne concerne un slot KEEP
    // → TransitionEngine ne modifiera jamais slotActiveAt pour ces slots
    // → GateRejected KEEP == 0 par construction
    int eventsOnKeep = 0;
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].slot == 0 || plan.events[i].slot == 1)
            eventsOnKeep++;
    CHECK(eventsOnKeep == 0);

    // Vérification Sequencer : slotActiveAt[KEEP] == 0 après simulation TransitionEngine
    // (TransitionEngine appelle setSlotActiveAt uniquement pour les PcmFlip events)
    Sequencer seq;
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].type == PlanEventType::PcmFlip)
            seq.setSlotActiveAt(plan.events[i].slot, plan.events[i].atSample);

    // Les slots KEEP n'ont pas reçu de setSlotActiveAt → leur gate est ouvert (==0)
    // On le vérifie en générant des triggers dans une fenêtre longue et en comptant
    TrackPattern pat;
    pat.numSteps = 16;
    for (int s = 0; s < 16; ++s) pat.steps[s] = true;
    for (int t = 0; t < 2; ++t) {
        *seq.patterns().writeBuffer(t) = pat;
    }
    seq.patterns().publish();

    const int64_t beatSamples = static_cast<int64_t>(sr * 60.0 / 120.0);
    TransportState ts{};
    ts.sampleRate     = sr;
    ts.bpm            = 120.0;
    ts.samplesPerBeat = beatSamples;
    ts.samplesPerStep = static_cast<double>(beatSamples) / 4.0;
    ts.playing        = true;

    // Fenêtre démarrant bien avant T (KEEP toujours actif, ENTER inactifs)
    const int64_t blockStart = 0;
    const int64_t blockLen   = T;  // [0, T) : avant toute activation ENTER

    EventScheduler sched;
    seq.generateEvents(ts, blockStart, blockLen, 0.f, sched);

    int keepTriggered = 0;
    sched.processBlock(blockStart, blockLen,
        [&](int32_t /*sample*/, const EngineEvent& ev) {
            if (ev.slot == 0 || ev.slot == 1) keepTriggered++;
        });

    // slotActiveAt[0] == slotActiveAt[1] == 0 → tous les triggers passent
    CHECK(keepTriggered > 0);
}
