// Tests de régression — transitions de scènes quantisées (StepSequencer).
//
// Contrat vérifié (identique au moteur, quel que soit le sens de navigation) :
//   1. L'armement d'une transition ne synchronise QUE les patterns (steps +
//      trackBarCounts) vers le double-buffer INACTIF — aucun état live du
//      moteur (buffer actif, gains, mutes) n'est touché à l'armement.
//   2. Le signal « fin de cycle » (consumeSceneEnd) est émis EXACTEMENT UNE
//      fois par armement, UNIQUEMENT sur la frontière (≡ transLen-1 ou, en cas
//      d'armement tardif, au flip ≡0) — jamais avant, jamais manqué.
//   3. Conséquence : applyScene() (appelé par timerCallback sur ce signal)
//      ne peut pas démarrer un crossfade avant la fin de cycle → pas de
//      coupure sonore au moment où l'on demande la transition.
//
// Régressions couvertes :
//   - armement pendant le DERNIER pas du cycle (≡ transLen-1) : le drapeau
//     était manqué → transition jamais appliquée + pendingScene bloqué.
//   - drapeau fin-de-cycle laissé « collé » après un stop → consommé par la
//     PROCHAINE navigation → applyScene à l'armement (coupure immédiate).

#include <catch2/catch_test_macros.hpp>

#include "dsp/Sampler.h"
#include "dsp/StepSequencer.h"

#include <cmath>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr double kSr         = 44100.0;
constexpr int    kBlock      = 512;
constexpr float  kBpm        = 120.f;
constexpr int    kTimerHz    = 30;    // MainComponent::startTimerHz(30)
constexpr int    kSceneSteps = 64;    // 4 barres × 16 pas

dsp::StepSequencer::StepBuf makeBuf(bool isB)
{
    dsp::StepSequencer::StepBuf b;
    for (int t = 0; t < 9; ++t)
        b.trackStepCount[t] = kSceneSteps;
    for (int s = 0; s < kSceneSteps; s += 4)
        b.steps[2][s] = true;                       // kick
    if (isB)
        for (int s = 1; s < kSceneSteps; s += 2)
            b.steps[4][s] = true;                   // hats
    b.steps[5][isB ? 8 : 7] = true;                 // marqueur de scène
    return b;
}

// Rejoue navigateScene() + le bloc transition de timerCallback() contre le
// vrai StepSequencer / Sampler, avec le même cadencement que l'app
// (audio 512 samples @44,1 kHz, timer 30 Hz).
struct Harness
{
    dsp::StepSequencer seq;
    dsp::Sampler       sampler;
    dsp::StepSequencer::StepBuf bufA = makeBuf(false);
    dsp::StepSequencer::StepBuf bufB = makeBuf(true);

    int currentIdx   = 0;
    int pendingScene = -1;
    int modelScene   = 0;      // scène dont le buffer est censé être actif

    long long samplesDone  = 0;
    long long nextTimerAt  = 0;
    int       armStep      = -1;
    int       applyStep    = -1;
    int       flipStep     = -1;
    int       applyCount   = 0;

    bool staleFlagAtArm = false;
    bool armBlocked     = false;

    void prepare(int fromScene)
    {
        seq.prepare(kSr);
        seq.setBpm(kBpm);
        sampler.prepare(kSr, kBlock);

        const int n = 4410;
        std::vector<float> pcm(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            pcm[static_cast<size_t>(i)] =
                0.5f * std::sin(2.0 * M_PI * 220.0 * i / kSr);
        for (int slot : { 2, 4, 5 })
        {
            sampler.loadSample(slot, pcm.data(), n, kSr);
            sampler.setSlotLoop(slot, true);
            sampler.setSlotOneShot(slot, false);
        }

        seq.prepareStepBuffer(fromScene == 0 ? bufA : bufB);
        seq.flipIfPrepared();
        seq.setPlaying(true);

        currentIdx = fromScene;
        modelScene = fromScene;
    }

    // ── Miroir de MainComponent::navigateScene() (chemin séquenceur jouant) ──
    void navigateScene(int target)
    {
        if (target == currentIdx) return;
        if (seq.hasPendingTransition() || pendingScene >= 0)
        {
            armBlocked = true;
            return;
        }

        int sceneLen = 1;
        for (int i = 0; i < 9; ++i)
            sceneLen = std::max(sceneLen, seq.getTrackStepCount(i));
        seq.setPendingTransitionLen(sceneLen);
        seq.prepareStepBuffer(target == 0 ? bufA : bufB);
        pendingScene = target;
        armStep      = seq.getCurrentStep();

        // Un drapeau déjà vrai à l'armement = drapeau périmé :
        // applyScene partirait immédiatement, avant la frontière.
        if (seq.consumeSceneEnd())
            staleFlagAtArm = true;
    }

    // ── Miroir du bloc transition de MainComponent::timerCallback() ──
    void timerTick()
    {
        if (pendingScene >= 0 && seq.consumeSceneEnd())
        {
            const int target = pendingScene;
            pendingScene     = -1;
            ++applyCount;
            applyStep        = seq.getCurrentStep();
            currentIdx       = target;
            // applyScene : re-prépare le buffer si plus d'armement en attente
            if (!seq.hasPendingTransition())
                seq.prepareStepBuffer(target == 0 ? bufA : bufB);
        }
    }

    void detectFlip()
    {
        // activeBuf_ ne change que via flipIfPrepared() : tout changement du
        // marqueur track5 est un flip effectif.
        const bool a = seq.getStep(5, 7);
        const bool b = seq.getStep(5, 8);
        int observed = -1;
        if (a && !b)        observed = 0;
        else if (b && !a)   observed = 1;
        if (observed >= 0 && observed != modelScene)
        {
            modelScene = observed;
            if (flipStep < 0)
                flipStep = seq.getCurrentStep();
        }
    }

    void block()
    {
        seq.process(kBlock, sampler);
        detectFlip();
        samplesDone += kBlock;
        if (samplesDone >= nextTimerAt)
        {
            nextTimerAt += static_cast<long long>(kSr / kTimerHz);
            timerTick();
        }
    }

    void runSteps(int n)
    {
        const long long target = samplesDone
            + static_cast<long long>(n)
              * static_cast<long long>(kSr / kBpm * 60.0 / 4.0);
        while (samplesDone < target)
            block();
    }

    // Bloque jusqu'à ce que le playhead vaille pos (premier passage).
    void waitUntilStep(int pos)
    {
        int guard = 0;
        while (seq.getCurrentStep() != pos)
        {
            block();
            if (++guard > 10 * kSceneSteps * 4)
                break;              // sécurité : jamais atteint en pratique
        }
    }

    // Exécute une transition complète : armement juste après la signature du
    // pas « off » dans le cycle, puis attente de l'application + flip.
    void runTransition(int from, int to, int off)
    {
        prepare(from);
        waitUntilStep(off);         // position = off au moment de l'armement
        navigateScene(to);

        const long long deadline =
            samplesDone + static_cast<long long>(kSceneSteps) * 4
            * static_cast<long long>(kSr / kBpm * 60.0 / 4.0);
        while (samplesDone < deadline)
        {
            block();
            if (applyCount > 0 && pendingScene < 0 && modelScene == to)
                break;
        }
    }
};

int mod(int v, int m) { return ((v % m) + m) % m; }

} // namespace

// ──1 : balayage complet de tous les offsets d'armement, dans les deux sens ──

TEST_CASE("Transition quantisée — fin de cycle exacte à tout offset, A↔B",
          "[step_sequencer]")
{
    for (int dir = 0; dir < 2; ++dir)
    {
        const int from = dir;
        const int to   = 1 - dir;
        const std::string label = dir == 0 ? "A->B" : "B->A";

        for (int off = 0; off < kSceneSteps; ++off)
        {
            INFO("sens=" << label << " offset=" << off);

            Harness h;
            h.runTransition(from, to, off);

            // 1. Aucun drapeau périmé à l'armement (applyScene ne partirait
            //    pas avant la frontière → pas de coupe prématurée).
            REQUIRE(h.staleFlagAtArm == false);
            REQUIRE(h.armBlocked == false);

            // 2. La transition est TOUJOURS appliquée (jamais de drapeau
            //    manqué → pendingScene bloqué / scène jamais appliquée).
            REQUIRE(h.applyCount == 1);
            REQUIRE(h.pendingScene == -1);

            // 3. applySceneTombe UNIQUEMENT sur la frontière : ≡63 (dernier
            //    pas) ou ≡0 (flip, armement pendant le dernier pas). Toute
            //    autre position = applyScene avant la frontière = bug.
            const int applyPos = mod(h.applyStep, kSceneSteps);
            REQUIRE((applyPos == kSceneSteps - 1 || applyPos == 0));

            // 4. Le flip du buffer (changement de motifs) a lieu pile sur ≡0,
            //    jamais avant l'application de la scène.
            REQUIRE(h.flipStep >= 0);
            const int flipPos = mod(h.flipStep, kSceneSteps);
            REQUIRE(flipPos == 0);
            if (applyPos == kSceneSteps - 1)
                REQUIRE(h.applyStep < h.flipStep);
            else
                REQUIRE(h.applyStep >= h.flipStep);

            // 5. Aucun drapeau laissé « collé » après application.
            REQUIRE(h.seq.consumeSceneEnd() == false);
        }
    }
}

// ──2 : régression — armement pendant le dernier pas du cycle ───────────────

TEST_CASE("Transition quantisée — armement au dernier pas (≡63) appliqué au flip",
          "[step_sequencer]")
{
    for (int dir = 0; dir < 2; ++dir)
    {
        INFO("sens=" << (dir == 0 ? "A->B" : "B->A"));
        Harness h;
        h.runTransition(dir, 1 - dir, kSceneSteps - 1);

        REQUIRE(h.armBlocked == false);
        REQUIRE(h.staleFlagAtArm == false);
        // Avant correctif : le drapeau ≡63 était déjà passé → jamais émis →
        // applyCount == 0 et pendingScene restait bloqué à -1… en réalité
        // bloqué ≥ 0 : la transition n'était JAMAIS appliquée.
        REQUIRE(h.applyCount == 1);
        REQUIRE(h.pendingScene == -1);
        // Appliqué sur la frontière de flip (≡0), pas avant.
        REQUIRE(mod(h.applyStep, kSceneSteps) == 0);
        REQUIRE(mod(h.flipStep, kSceneSteps) == 0);
    }
}

// ──3 : l'armement ne touche que le buffer inactif (contrat de sync) ────────

TEST_CASE("Transition quantisée — sync d'armement n'écrit que le buffer inactif",
          "[step_sequencer]")
{
    Harness h;
    h.prepare(0);                    // scène A active

    // État live avant armement
    std::vector<bool> stepsBefore;
    for (int s = 0; s < kSceneSteps; ++s)
        stepsBefore.push_back(h.seq.getStep(5, s));
    const float        gainBefore  = h.sampler.getSlotGain(2);
    const bool         muteBefore  = h.sampler.isSlotMuted(2);
    const bool         loadedBefore = h.sampler.isLoaded(2);

    h.navigateScene(1);              // armement A→B

    // Le buffer ACTIF (scène A, état live du moteur) est intact :
    for (int s = 0; s < kSceneSteps; ++s)
        REQUIRE(h.seq.getStep(5, s) == stepsBefore[static_cast<size_t>(s)]);
    REQUIRE(h.seq.getStep(5, 7) == true);    // marqueur A toujours actif
    REQUIRE(h.seq.getStep(5, 8) == false);

    // Aucun état live du sampler n'a été modifié par la sync.
    REQUIRE(h.sampler.getSlotGain(2)    == gainBefore);
    REQUIRE(h.sampler.isSlotMuted(2)    == muteBefore);
    REQUIRE(h.sampler.isLoaded(2)       == loadedBefore);

    // La longueur de scène armée reflète bien les trackBarCounts de la scène
    // courante (l'unique donnée MC-side→engine-side avec les steps).
    REQUIRE(h.seq.hasPendingTransition() == true);
    REQUIRE(h.seq.getTrackStepCount(2) == kSceneSteps);
}

// ──4 : deux transitions successives — pas de drapeau résiduel ───────────────

TEST_CASE("Transition quantisée — séquence A→B→A sans drapeau résiduel",
          "[step_sequencer]")
{
    Harness h;
    h.prepare(0);
    h.runSteps(10);
    h.navigateScene(1);
    {
        const long long deadline = h.samplesDone + 4LL * kSceneSteps
            * static_cast<long long>(kSr / kBpm * 60.0 / 4.0);
        while (h.samplesDone < deadline)
        {
            h.block();
            if (h.applyCount >= 1 && h.pendingScene < 0 && h.modelScene == 1)
                break;
        }
    }
    REQUIRE(h.applyCount == 1);
    REQUIRE(h.seq.consumeSceneEnd() == false);

    // Retour B→A à un offset arbitraire : ni blocage, ni drapeau périmé.
    h.runSteps(7);
    h.navigateScene(0);
    REQUIRE(h.staleFlagAtArm == false);     // le drapeau de la transition 1
                                            // n'a pas survécu
    REQUIRE(h.armBlocked == false);
    {
        const long long deadline = h.samplesDone + 4LL * kSceneSteps
            * static_cast<long long>(kSr / kBpm * 60.0 / 4.0);
        while (h.samplesDone < deadline)
        {
            h.block();
            if (h.applyCount >= 2 && h.pendingScene < 0 && h.modelScene == 0)
                break;
        }
    }
    REQUIRE(h.applyCount == 2);
    REQUIRE(h.staleFlagAtArm == false);
}
