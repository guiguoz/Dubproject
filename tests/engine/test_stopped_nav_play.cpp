// test_stopped_nav_play.cpp — Contrats pour activateSceneStopped (Play depuis STOP)
//
// Ces tests vérifient les invariants des sous-composants utilisés par
// EngineFacade::activateSceneStopped :
//
//   SNP-1 : SlotPlayer::clearSlot → isLoaded == false, getPcmFrames == 0
//           Contrat : slot avec filePath vide dans SceneStore → clearSlot effectif.
//
//   SNP-2 : TransitionEngine::reset() sur état Armed → état Idle
//           Contrat : activateSceneStopped annule toute transition STOP en cours.
//
//   SNP-3 : PatternBuffer::publish/readBuffer est cohérent
//           Contrat : syncWritePatternsFromScene + flipPatternBuffer reflètent
//           correctement SceneDefinition[idx] dans le buffer audio.

#include <catch2/catch_test_macros.hpp>
#include "engine/SlotPlayer.h"
#include "engine/TransitionEngine.h"
#include "engine/SceneStore.h"
#include "engine/Sequencer.h"
#include "engine/Transport.h"
#include "engine/EventScheduler.h"

using namespace engine;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static SlotPcm makeValidPcm(int numFrames = 1024, float sr = 44100.f)
{
    SlotPcm pcm;
    pcm.numChannels = 1;
    pcm.numFrames   = numFrames;
    pcm.sampleRate  = sr;
    pcm.data.assign(static_cast<std::size_t>(numFrames), 0.f);
    if (numFrames > 1) pcm.data[1] = 0.5f;
    return pcm;
}

static TransportState makeStoppedTS()
{
    TransportState ts{};
    ts.sampleRate     = 44100.0;
    ts.bpm            = 120.0;
    ts.samplesPerBeat = 44100.0 * 60.0 / 120.0;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.playing        = false;
    ts.samplePos      = 0;
    return ts;
}

static TransportState makePlayingTS()
{
    auto ts   = makeStoppedTS();
    ts.playing = true;
    return ts;
}

// ─── SNP-1 ───────────────────────────────────────────────────────────────────
//
// Après clearSlot : isLoaded == false, les voix sont inactives.
// Prouve que le chemin "filePath vide → clearSlot" dans activateSceneStopped
// bloque correctement tout nouveau trigger (gate = isLoaded).
// Note : clearSlot ne détruit pas le buffer PCM (fade-out en cours possible),
// donc getPcmFrames peut être non-nul — seul isLoaded fait foi.
//
TEST_CASE("SNP-1: clearSlot sets isLoaded false and silences voices", "[stopped][nav]")
{
    SlotPlayer sp;
    sp.prepareStretchers(1, 44100.0);
    sp.setSpatial(0, 0.f, 0.f);

    sp.loadSlot(0, makeValidPcm(1024), PlayMode::LoopSync);
    REQUIRE(sp.isLoaded(0));

    sp.clearSlot(0);

    // isLoaded == false : le thread audio ne déclenchera plus de voix
    REQUIRE_FALSE(sp.isLoaded(0));
    // Les deux voix sont inactives
    REQUIRE(sp.getActiveVoiceCount(0) == 0);
    // isVoiceActive reflète le même état
    REQUIRE_FALSE(sp.isVoiceActive(0));
}

// ─── SNP-2 ───────────────────────────────────────────────────────────────────
//
// TransitionEngine Armed → reset() → Idle.
// Prouve qu'activateSceneStopped annule bien une transition STOP déjà armée
// (sinon processBlock l'exécuterait au démarrage du transport et écraserait
// le slot avec des EngineEvents issus de l'ancienne transition).
//
TEST_CASE("SNP-2: TransitionEngine reset cancels Armed STOP transition", "[stopped][nav]")
{
    SceneStore store;
    SceneData s0, s1;
    s0.bpm = 120;
    s1.bpm = 120;
    s0.slots[0].active   = true;
    s0.slots[0].filePath = "kick.wav";
    s0.slots[0].mode     = PlayMode::LoopSync;
    s1.slots[0] = s0.slots[0];
    s1.slots[1].active   = true;
    s1.slots[1].filePath = "bass.wav";
    s1.slots[1].mode     = PlayMode::LoopSync;
    store.setScene(0, s0);
    store.setScene(1, s1);

    TransitionEngine te;
    REQUIRE(te.state() == TransitionEngine::State::Idle);

    te.requestTransition(0, 1, store, makePlayingTS());
    REQUIRE(te.state() == TransitionEngine::State::Armed);

    // activateSceneStopped appelle ceci pour annuler la transition
    te.reset();

    REQUIRE(te.state() == TransitionEngine::State::Idle);
    // Vérifier que processBlock n'émettra plus d'events (state Idle → no-op)
    EventScheduler sched;
    te.processBlock(makePlayingTS(), sched);
    REQUIRE(sched.size() == 0);
}

// ─── SNP-3 ───────────────────────────────────────────────────────────────────
//
// PatternBuffer : write → publish → readBuffer retourne les steps écrits.
// Prouve que le mécanisme triple-buffer utilisé par syncWritePatternsFromScene
// + flipPatternBuffer reflète correctement SceneDefinition[idx].steps après
// activateSceneStopped.
//
TEST_CASE("SNP-3: PatternBuffer reflects written steps after publish", "[stopped][nav]")
{
    PatternBuffer buf;

    // Simulate syncWritePatternsFromScene writing scene steps into writeBuffer
    TrackPattern* w0 = buf.writeBuffer(0);
    w0->numSteps   = 16;
    w0->steps[3]   = true;
    w0->steps[11]  = true;

    TrackPattern* w2 = buf.writeBuffer(2);
    w2->numSteps   = 32;
    w2->steps[7]   = true;

    buf.publish();   // simule flipPatternBuffer()

    // Consommateur (audio thread) lit readBuffer
    const TrackPattern* r0 = buf.readBuffer(0);
    REQUIRE(r0->numSteps    == 16);
    REQUIRE(r0->steps[3]    == true);
    REQUIRE(r0->steps[11]   == true);
    REQUIRE(r0->steps[0]    == false);
    REQUIRE(r0->steps[4]    == false);

    const TrackPattern* r2 = buf.readBuffer(2);
    REQUIRE(r2->numSteps   == 32);
    REQUIRE(r2->steps[7]   == true);
    REQUIRE(r2->steps[0]   == false);

    // Vérifier que le track 1 (non modifié) reste au défaut
    const TrackPattern* r1 = buf.readBuffer(1);
    for (int s = 0; s < 16; ++s)
        REQUIRE(r1->steps[s] == false);
}
