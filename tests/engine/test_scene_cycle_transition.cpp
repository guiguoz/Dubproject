// ─────────────────────────────────────────────────────────────────────────────
// Transitions de scène : frontière = FIN DU CYCLE de la scène courante
// + recalage de phase de la scène entrante (elle redémarre sur son step 0).
//
// Bug corrigé (rapport utilisateur) :
//   « je lis la scène A (64 pas) et demande le passage en scène B : le passage
//     se fait au milieu de la scène A. Je lis B (32 pas) et demande le retour
//     en A : ça revient au milieu de A. »
//
// Cause : la frontière était `nextBoundary(ts, 16)` (prochaine mesure) quelle
// que soit la longueur des scènes, et l'index dans le pattern était
// `stepGlobal % numSteps` — la scène entrante reprenait donc à un step
// arbitraire (ex. retour en A de 64 pas sur une frontière ≡ 48 → step 48).
// ─────────────────────────────────────────────────────────────────────────────

#include <catch2/catch_test_macros.hpp>

#include "engine/Sequencer.h"
#include "engine/SceneTransitionPlan.h"
#include "engine/EventScheduler.h"
#include "engine/Transport.h"

#include <vector>

using namespace engine;

namespace {

TransportState makeTs(int64_t samplePos = 0)
{
    TransportState ts{};
    ts.sampleRate     = 44100.0;
    ts.bpm            = 120.0;
    ts.samplesPerBeat = 44100.0 * 60.0 / 120.0;   // 22050
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;  // 5512.5
    ts.playing        = true;
    ts.blockStart     = samplePos;
    ts.samplePos      = samplePos;
    return ts;
}

SceneData makeScene(int barsTrack0, int barsTrack1 = 0)
{
    SceneData sc{};
    for (auto& c : sc.trackBarCounts) c = 1;
    sc.trackBarCounts[0] = barsTrack0;
    if (barsTrack1 > 0) sc.trackBarCounts[1] = barsTrack1;
    return sc;
}

// Collecte les triggers d'un slot sur une plage de steps, en avançant par blocs.
std::vector<int64_t> collectTriggers(Sequencer& seq, const TransportState& ts,
                                     int slot, int64_t fromStep, int64_t toStep,
                                     int32_t blockSize = 512)
{
    std::vector<int64_t> out;
    EventScheduler sched;
    const int64_t start = sampleOfStep(ts, fromStep);
    const int64_t end   = sampleOfStep(ts, toStep);
    int64_t pos = start;
    while (pos < end) {
        const int32_t n = static_cast<int32_t>(
            std::min<int64_t>(blockSize, end - pos));
        TransportState blk = ts;
        blk.blockStart = pos;
        blk.samplePos  = pos + n;
        seq.generateEvents(blk, pos, n, 0.f, sched);
        sched.processBlock(pos, n, [&](int32_t, const EngineEvent& ev) {
            if (ev.slot == static_cast<uint8_t>(slot)
                && ev.type == EventType::Trigger)
                out.push_back(ev.time);
        });
        pos += n;
    }
    return out;
}

} // namespace

// ─── Règle de frontière ──────────────────────────────────────────────────────

TEST_CASE("Cycle de scène — plus longue piste, jamais sous une mesure", "[transition][cycle]")
{
    REQUIRE(sceneCycleSteps(makeScene(1))  == 16);
    REQUIRE(sceneCycleSteps(makeScene(4))  == 64);   // A : 4 mesures
    REQUIRE(sceneCycleSteps(makeScene(2))  == 32);   // B : 2 mesures
    REQUIRE(sceneCycleSteps(makeScene(2, 8)) == 128); // la plus longue gagne
    REQUIRE(sceneCycleSteps(makeScene(3))  == 48);   // 3 mesures = 48 pas
}

TEST_CASE("Frontière — fin du CYCLE courant, pas la prochaine mesure", "[transition][cycle]")
{
    const TransportState ts = makeTs(sampleOfStep(makeTs(), 40));   // mesure 3 d'une scène de 4 mesures

    // Scène A = 64 pas → la bascule tombe au step 64 (fin du cycle), pas au step 48.
    const TransitionBoundary bA = planTransitionBoundary(ts, makeScene(4));
    REQUIRE(bA.cycleSteps == 64);
    REQUIRE(bA.step       == 64);
    REQUIRE(bA.sample     == sampleOfStep(ts, 64));
    REQUIRE(bA.step % 64  == 0);          // toutes les pistes de A sont sur leur step 0

    // L'ancienne règle (« prochaine mesure ») tombait bien au milieu du cycle :
    REQUIRE(stepIndexAt(ts, nextBoundary(ts, 16)) == 48);

    // Scène B = 32 pas → fin du cycle à 64 depuis le step 40 (32 % n'est pas la règle).
    const TransitionBoundary bB = planTransitionBoundary(ts, makeScene(2));
    REQUIRE(bB.step == 64);
}

TEST_CASE("Frontière — pile sur une frontière : on joue le cycle complet", "[transition][cycle]")
{
    const TransportState ts = makeTs(sampleOfStep(makeTs(), 64));  // exactement step 64
    const TransitionBoundary b = planTransitionBoundary(ts, makeScene(4));
    REQUIRE(b.step == 128);        // cycle suivant, jamais une bascule instantanée
    REQUIRE(b.step % 64 == 0);

    // Au step 63 (dernier step du cycle) : la bascule est à 64, soit un step plus tard.
    const TransportState ts63 = makeTs(sampleOfStep(makeTs(), 63));
    REQUIRE(planTransitionBoundary(ts63, makeScene(4)).step == 64);
}

// ─── Recalage de phase : la scène entrante redémarre sur son step 0 ──────────

TEST_CASE("Recalage — B (32 pas) démarre sur son step 0 après une frontière à 48",
          "[transition][cycle][phase]")
{
    // Cas critique : A = 3 mesures (cycle 48), B = 2 mesures (32).
    // 48 % 32 == 16 → sans recalage, B reprendrait à son step 16 (milieu de pattern).
    Sequencer seq;
    const TransportState ts = makeTs();

    // Pattern A : step 0 seulement (sur 48 pas). Pattern B : steps 0 et 8 (sur 32 pas).
    TrackPattern patA{}; patA.numSteps = 48; patA.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patA; seq.patterns().publish();

    TrackPattern staged[kMaxSlots] = {};
    staged[0].numSteps = 32; staged[0].steps[0] = true; staged[0].steps[8] = true;

    const int64_t boundaryStep = 48;
    const int64_t boundary     = sampleOfStep(ts, boundaryStep);
    seq.stageForBoundary(staged, boundary, boundaryStep);

    const auto trigs = collectTriggers(seq, ts, 0, 0, 80);

    // A joue son step 0 aux steps 0 et 48 (le pattern A dure 48 pas)...
    REQUIRE(trigs.size() >= 3);
    REQUIRE(trigs[0] == sampleOfStep(ts, 0));

    // ...puis B démarre à la frontière sur SON step 0 (pas sur son step 16)...
    REQUIRE(trigs[1] == sampleOfStep(ts, 48));

    // ...et son step 8 suit 8 pas plus tard dans le nouveau référentiel (48+8 = 56).
    REQUIRE(trigs[2] == sampleOfStep(ts, 56));
}

TEST_CASE("Recalage — sans 3e argument, la phase historique est conservée",
          "[transition][cycle][phase]")
{
    // Non-régression : la surcharge 2 arguments garde `step % numSteps`
    // (utilisée par les tests P0 sync et l'édition live).
    Sequencer seq;
    const TransportState ts = makeTs();

    TrackPattern patA{}; patA.numSteps = 16; patA.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patA; seq.patterns().publish();

    TrackPattern staged[kMaxSlots] = {};
    staged[0].numSteps = 16; staged[0].steps[0] = true;

    const int64_t boundary = sampleOfStep(ts, 16);
    seq.stageForBoundary(staged, boundary);          // 2 args → pas de recalage

    const auto trigs = collectTriggers(seq, ts, 0, 0, 32);
    REQUIRE(trigs.size() == 2);
    REQUIRE(trigs[0] == sampleOfStep(ts, 0));
    REQUIRE(trigs[1] == sampleOfStep(ts, 16));       // 16 % 16 == 0, identique ici
    REQUIRE(seq.phaseBase(0) == 0);
}

TEST_CASE("Recalage — clearStaged NE touche PAS la base de phase", "[transition][cycle][phase]")
{
    // Invariant : la base de phase appartient au transport, pas au stage.
    // clearStaged() annule le flip pattern mais ne remet pas phaseBase à 0.
    // C'est resetPatternPhase() qui est prévu pour ça (appelé par play()).
    //
    // NOTE : stageForBoundary() écrit dans stagedPhaseBase_, pas dans phaseBase_[].
    // La base effective n'est posée dans phaseBase_[] qu'à publishStaged() (audio).
    // Le test simule donc d'abord le bloc audio traversant la frontière, puis
    // vérifie que clearStaged() ne touche pas à la base déjà publiée.
    Sequencer seq;
    const TransportState ts = makeTs();
    TrackPattern patA{}; patA.numSteps = 16; patA.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patA; seq.patterns().publish();

    TrackPattern staged[kMaxSlots] = {};
    staged[0].numSteps = 32; staged[0].steps[0] = true;
    const int64_t boundaryStep = 48;
    const int64_t boundary = sampleOfStep(ts, boundaryStep);
    seq.stageForBoundary(staged, boundary, boundaryStep);

    // Bloc audio traversant la frontière → publishStaged() → phaseBase_ = 48
    {
        EventScheduler sched;
        const int64_t blockStart = boundary - 512;
        TransportState blk = ts;
        blk.blockStart = blockStart;
        blk.samplePos  = blockStart + 1024;
        seq.generateEvents(blk, blockStart, 1024, 0.f, sched);
    }
    REQUIRE(seq.phaseBase(0) == boundaryStep);  // publiée par l'audio
    REQUIRE(seq.hasStaged() == false);           // stage consommé

    // clearStaged() NE doit PAS remettre phaseBase à 0
    seq.clearStaged();
    REQUIRE(seq.hasStaged() == false);
    REQUIRE(seq.phaseBase(0) == boundaryStep);  // base préservée

    // resetPatternPhase() remet bien à 0 (appelé par play()/navigation arrêtée).
    seq.resetPatternPhase();
    REQUIRE(seq.phaseBase(0) == 0);

    TrackPattern patB{}; patB.numSteps = 32; patB.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patB; seq.patterns().publish();
    const auto trigs = collectTriggers(seq, ts, 0, 0, 64);
    REQUIRE(trigs.size() == 2);
    REQUIRE(trigs[0] == sampleOfStep(ts, 0));
    REQUIRE(trigs[1] == sampleOfStep(ts, 32));
}

TEST_CASE("Recalage — la base de phase survit au republish post-commit (régression D1)",
          "[transition][cycle][phase]")
{
    // Régression : applyScene appelle prepareStepBuffer → clearStaged ~33 ms après
    // la frontière. Avant le correctif, cela remettait phaseBase à 0 et B démarrait
    // au milieu de son pattern (ex. frontière=64, B=48 pas → index 64%48=16).
    Sequencer seq;
    const TransportState ts = makeTs();

    // Pattern A en cours de lecture
    TrackPattern patA{}; patA.numSteps = 64; patA.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patA; seq.patterns().publish();

    // Stage B (48 pas) pour la frontière au step 64
    TrackPattern stagedB[kMaxSlots] = {};
    stagedB[0].numSteps = 48; stagedB[0].steps[0] = true;
    const int64_t boundaryStep = 64;
    const int64_t boundary = sampleOfStep(ts, boundaryStep);
    seq.stageForBoundary(stagedB, boundary, boundaryStep);

    // Bloc audio traversant la frontière → publishStaged() pose phaseBase = 64
    {
        EventScheduler sched;
        const int64_t blockStart = boundary - 512;
        const int32_t blockLen   = 1024;
        TransportState blk = ts;
        blk.blockStart = blockStart;
        blk.samplePos  = blockStart + blockLen;
        seq.generateEvents(blk, blockStart, blockLen, 0.f, sched);
    }
    REQUIRE(seq.phaseBase(0) == boundaryStep);  // base posée par publishStaged

    // Simuler le tick timer : clearStaged() + republier B (équivalent prepareStepBuffer)
    seq.clearStaged();
    *seq.patterns().writeBuffer(0) = stagedB[0]; seq.patterns().publish();

    // RÉGRESSION : avant le correctif, phaseBase retombait à 0 → index = 64%48 = 16
    REQUIRE(seq.phaseBase(0) == boundaryStep);  // base préservée — échouait avant fix

    // Vérifier que B déclenche bien sur son step 0 (= global step 64), pas sur 16
    const int64_t expectedFirst = sampleOfStep(ts, 64);   // B step 0 ← correct
    const auto trigs = collectTriggers(seq, ts, 0, boundaryStep, boundaryStep + 49);
    REQUIRE(!trigs.empty());
    REQUIRE(trigs[0] == expectedFirst);  // 352800 ; avant fix : sampleOfStep(ts, 80) = 441000
}

TEST_CASE("Recalage — swing : pas de coup fantôme sur la fin du pattern entrant",
          "[transition][cycle][phase][swing]")
{
    // Avec du swing, un step du cycle sortant peut être relâché APRÈS la frontière
    // (swing = 1.0 → décalage ≈ 1 step). Sans garde, il déclencherait la dernière
    // case du pattern entrant — un coup fantôme au sample exact de la bascule.
    Sequencer seq;
    const TransportState ts = makeTs();

    TrackPattern patA{}; patA.numSteps = 16; patA.steps[15] = true;   // sortant : step 15
    *seq.patterns().writeBuffer(0) = patA; seq.patterns().publish();

    TrackPattern staged[kMaxSlots] = {};
    staged[0].numSteps = 16; staged[0].steps[15] = true;              // entrant : step 15 aussi
    const int64_t boundaryStep = 16;
    const int64_t boundary     = sampleOfStep(ts, boundaryStep);
    seq.stageForBoundary(staged, boundary, boundaryStep);

    // Sans swing, rien ne doit sortir de B : son step 0 est inactif.
    const auto quiet = collectTriggers(seq, ts, 0, boundaryStep, boundaryStep + 1);
    REQUIRE(quiet.empty());

    // Même contrôle avec un swing maximal : le step 15 de A est relâché 1 sample
    // après la frontière, mais il appartient au cycle précédent → ignoré.
    Sequencer seq2;
    *seq2.patterns().writeBuffer(0) = patA; seq2.patterns().publish();
    seq2.stageForBoundary(staged, boundary, boundaryStep);

    EventScheduler sched;
    const int32_t n = 4096;
    const int64_t pos = boundary - 1024;
    TransportState blk = ts;
    blk.blockStart = pos;
    blk.samplePos  = pos + n;
    seq2.generateEvents(blk, pos, n, 1.0f, sched);          // swing max
    std::vector<int64_t> afterBoundary;
    sched.processBlock(pos, n, [&](int32_t, const EngineEvent& ev) {
        if (ev.slot == 0 && ev.type == EventType::Trigger
            && ev.time >= boundary)
            afterBoundary.push_back(ev.time);
    });
    REQUIRE(afterBoundary.empty());
}

// ─── Scénario utilisateur de bout en bout (A 64 ↔ B 32) ─────────────────────

TEST_CASE("Scénario rapporté — A(64) → B(32) → A(64) : aucun démarrage au milieu",
          "[transition][cycle][phase]")
{
    const TransportState ts0 = makeTs();
    const SceneData A = makeScene(4);   // 64 pas
    const SceneData B = makeScene(2);   // 32 pas

    Sequencer seq;
    TrackPattern patA{}; patA.numSteps = 64; patA.steps[0] = true;
    *seq.patterns().writeBuffer(0) = patA; seq.patterns().publish();

    // ── A joue : son step 0 tombe aux steps 0, 64, 128… ──────────────────────
    {
        const auto t = collectTriggers(seq, ts0, 0, 0, 48);
        REQUIRE(t.size() == 1);
        REQUIRE(t[0] == sampleOfStep(ts0, 0));
    }

    // ── Navigation demandée au step 40 (milieu de la mesure 3 de A) ──────────
    // Frontière = fin du cycle de A = step 64 (et non 48 = prochaine mesure).
    const TransitionBoundary b1 = planTransitionBoundary(makeTs(sampleOfStep(ts0, 40)), A);
    REQUIRE(b1.step == 64);
    REQUIRE(b1.step % sceneCycleSteps(A) == 0);      // A a fini son cycle

    TrackPattern stagedB[kMaxSlots] = {};
    stagedB[0].numSteps = 32; stagedB[0].steps[0] = true;
    seq.stageForBoundary(stagedB, b1.sample, b1.step);

    // ── A termine son cycle puis B démarre sur SON step 0 (64 % 32 == 0) ─────
    {
        const auto t = collectTriggers(seq, ts0, 0, 40, 97);   // borne haute exclusive
        REQUIRE(t.size() == 2);
        REQUIRE(stepIndexAt(ts0, t[0]) == 64);       // unique trigger à la frontière : B step 0
        REQUIRE(stepIndexAt(ts0, t[1]) == 96);       // B boucle (64 + 32)
        REQUIRE(seq.phaseBase(0) == 64);             // base de phase posée
    }

    // ── Retour en A demandé au step 80 (milieu de B) ─────────────────────────
    // Frontière = fin du cycle de B = step 96 ; A doit repartir à SON step 0.
    const TransitionBoundary b2 = planTransitionBoundary(makeTs(sampleOfStep(ts0, 80)), B);
    REQUIRE(b2.step == 96);
    REQUIRE((b2.step - b1.step) % sceneCycleSteps(B) == 0);   // B a fini son cycle

    TrackPattern stagedA[kMaxSlots] = {};
    stagedA[0].numSteps = 64; stagedA[0].steps[0] = true;
    seq.stageForBoundary(stagedA, b2.sample, b2.step);

    {
        const auto t = collectTriggers(seq, ts0, 0, 96, 144);
        REQUIRE(t.size() == 1);
        REQUIRE(stepIndexAt(ts0, t[0]) == 96);       // A repart à son step 0…
        // …et non au milieu de son pattern : sans recalage l'index serait 96 % 64 = 32.
        REQUIRE((stepIndexAt(ts0, t[0]) - seq.phaseBase(0)) % sceneCycleSteps(A) == 0);
    }

    // ── A continue : cycle complet de 64 pas depuis la frontière ─────────────
    {
        const auto t = collectTriggers(seq, ts0, 0, 144, 168);
        REQUIRE(t.size() == 1);
        REQUIRE(stepIndexAt(ts0, t[0]) == 160);      // 96 + 64
    }
}

// ─── UI : index par piste aligné sur l'audio ─────────────────────────────────

TEST_CASE("Sequencer — numStepsForSlot expose la longueur jouée", "[transition][cycle]")
{
    Sequencer seq;
    TrackPattern p{}; p.numSteps = 48;
    *seq.patterns().writeBuffer(2) = p; seq.patterns().publish();
    REQUIRE(seq.numStepsForSlot(2) == 48);
    REQUIRE(seq.numStepsForSlot(99) == 16);      // hors bornes → valeur neutre
}
