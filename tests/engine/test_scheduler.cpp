#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/Transport.h"
#include "engine/EventScheduler.h"
#include "engine/Sequencer.h"

using namespace engine;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static TransportState makeTS(double sr = 44100.0, double bpm = 120.0) {
    TransportState ts;
    ts.sampleRate     = sr;
    ts.bpm            = bpm;
    ts.samplesPerBeat = sr * 60.0 / bpm;
    ts.samplesPerStep = ts.samplesPerBeat / 4.0;
    ts.samplePos      = 0;
    ts.playing        = true;
    return ts;
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SEQ1 : step tombant mi-bloc → trigger à l'offset exact
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SEQ1: step mid-block triggers at exact offset", "[sequencer]") {
    const auto ts = makeTS(44100.0, 120.0); // samplesPerStep = 5512.5

    // Trouver un bloc qui contient un step en milieu de bloc
    // step 1 commence à ceil(5512.5) = 5513
    // bloc qui le contient : blockStart = 5513 - 200 = 5313, taille 512
    const int64_t stepSample = sampleOfStep(ts, 1); // 5513
    const int64_t blockStart = stepSample - 200;
    const int32_t numSamples = 512;

    Sequencer seq;
    // Activer slot 0, step 1 dans le pattern de 16 steps
    {
        TrackPattern* pat = seq.patterns().writeBuffer(0);
        pat->numSteps     = 16;
        pat->steps[1]     = true;
        seq.patterns().flip();
    }

    EventScheduler sched;
    seq.generateEvents(ts, blockStart, numSamples, 0.0f, sched);

    REQUIRE(sched.size() == 1);
    const EngineEvent& ev = sched.at(0);
    REQUIRE(ev.type == EventType::Trigger);
    REQUIRE(ev.slot == 0);
    REQUIRE(ev.time == stepSample);

    // Vérifier l'offset via processBlock
    int callCount = 0;
    int32_t capturedOffset = -1;
    sched.processBlock(blockStart, numSamples, [&](int32_t offset, const EngineEvent&) {
        capturedOffset = offset;
        ++callCount;
    });
    REQUIRE(callCount == 1);
    REQUIRE(capturedOffset == static_cast<int32_t>(stepSample - blockStart));
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SEQ2 : swing 60% → steps impairs décalés de la valeur théorique ±1 sample
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SEQ2: swing 60% shifts odd steps by expected amount", "[sequencer]") {
    const auto ts = makeTS(44100.0, 120.0); // samplesPerStep = 5512.5

    const float swingFactor = 0.6f;
    // Décalage attendu = round(0.6 * 5512.5) = round(3307.5) = 3308
    const int64_t expectedSwing = static_cast<int64_t>(
        std::round(static_cast<double>(swingFactor) * ts.samplesPerStep));

    Sequencer seq;
    {
        // Activer steps 0 (pair) et 1 (impair) sur slot 0
        TrackPattern* pat = seq.patterns().writeBuffer(0);
        pat->numSteps     = 16;
        pat->steps[0]     = true;
        pat->steps[1]     = true;
        seq.patterns().flip();
    }

    // Bloc large qui couvre les steps 0 et 1 (step 1 avec swing tombe à 5513+3308=8821)
    // step 0 = 0, step 1 no swing = 5513, step 1 with swing = 5513 + 3308 = 8821
    // step 2 = ceil(2 * 5512.5) = 11025 → juste après notre bloc
    const int64_t blockStart = 0;
    const int32_t numSamples = 9000; // couvre step 0 (0) et step 1+swing (8821)

    EventScheduler sched;
    seq.generateEvents(ts, blockStart, numSamples, swingFactor, sched);

    // On doit avoir exactement 2 triggers
    REQUIRE(sched.size() == 2);

    // step 0 (pair) : pas de swing
    const EngineEvent& ev0 = sched.at(0);
    REQUIRE(ev0.type == EventType::Trigger);
    REQUIRE(ev0.time == sampleOfStep(ts, 0));

    // step 1 (impair) : décalé de expectedSwing
    const EngineEvent& ev1 = sched.at(1);
    REQUIRE(ev1.type == EventType::Trigger);
    const int64_t actualSwing = ev1.time - sampleOfStep(ts, 1);
    // Tolérance ±1 sample (round peut différer)
    REQUIRE(std::abs(static_cast<long long>(actualSwing - expectedSwing)) <= 1LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SEQ3 : 15 min à 140 BPM → écart entre trigger n et n+1 CONSTANT (±0 sample)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SEQ3: no drift over 15 minutes at 140 BPM", "[sequencer]") {
    const double sr      = 44100.0;
    const double bpm     = 140.0;
    const auto   ts      = makeTS(sr, bpm);
    // samplesPerStep = 44100 * 60 / 140 / 4 = 4725.0

    const int64_t totalSamples = static_cast<int64_t>(15.0 * 60.0 * sr); // 15 min
    const int32_t blockSize    = 512;

    Sequencer seq;
    {
        TrackPattern* pat = seq.patterns().writeBuffer(0);
        pat->numSteps     = 16;
        for (int i = 0; i < 16; ++i) pat->steps[i] = true; // tous les steps actifs
        seq.patterns().flip();
    }

    // Collecter tous les triggers
    std::vector<int64_t> triggerTimes;
    triggerTimes.reserve(64000);

    int64_t blockStart = 0;
    while (blockStart < totalSamples) {
        const int32_t numSamples = static_cast<int32_t>(
            std::min(static_cast<int64_t>(blockSize), totalSamples - blockStart));

        EventScheduler sched;
        seq.generateEvents(ts, blockStart, numSamples, 0.0f, sched);
        for (int i = 0; i < sched.size(); ++i) {
            triggerTimes.push_back(sched.at(i).time);
        }
        blockStart += numSamples;
    }

    REQUIRE(!triggerTimes.empty());

    // Vérifier que l'écart entre triggers consécutifs est constant (samplesPerStep)
    // sampleOfStep(N+1) - sampleOfStep(N) = ceil((N+1)*sps) - ceil(N*sps)
    // Peut valoir floor(sps) ou ceil(sps), mais pas plus.
    const int64_t expectedGapLow  = static_cast<int64_t>(std::floor(ts.samplesPerStep));
    const int64_t expectedGapHigh = static_cast<int64_t>(std::ceil(ts.samplesPerStep));

    for (size_t i = 1; i < triggerTimes.size(); ++i) {
        const int64_t gap = triggerTimes[i] - triggerTimes[i - 1];
        // L'écart doit être exactement floor ou ceil de samplesPerStep (pas de dérive)
        REQUIRE(gap >= expectedGapLow);
        REQUIRE(gap <= expectedGapHigh);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SEQ4 : patterns de longueurs mixtes (16/32/64) alignés sur 10 000 steps
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SEQ4: mixed pattern lengths 16/32/64 over 10000 steps", "[sequencer]") {
    const auto ts = makeTS(44100.0, 120.0); // samplesPerStep = 5512.5

    Sequencer seq;
    // slot 0 : 16 steps, seulement step 0
    {
        TrackPattern* p = seq.patterns().writeBuffer(0);
        p->numSteps     = 16;
        p->steps[0]     = true;
        // tous les autres à false (initialisés à false par défaut)
    }
    // slot 1 : 32 steps, seulement step 0
    {
        TrackPattern* p = seq.patterns().writeBuffer(1);
        p->numSteps     = 32;
        p->steps[0]     = true;
    }
    // slot 2 : 64 steps, seulement step 0
    {
        TrackPattern* p = seq.patterns().writeBuffer(2);
        p->numSteps     = 64;
        p->steps[0]     = true;
    }
    seq.patterns().flip();

    const int64_t numSteps  = 10000;
    const int64_t endSample = sampleOfStep(ts, numSteps);
    const int32_t blockSize = 512;

    int slot0Count = 0;
    int slot1Count = 0;
    int slot2Count = 0;

    int64_t blockStart = 0;
    while (blockStart < endSample) {
        const int32_t numSamples = static_cast<int32_t>(
            std::min(static_cast<int64_t>(blockSize), endSample - blockStart));

        EventScheduler sched;
        seq.generateEvents(ts, blockStart, numSamples, 0.0f, sched);
        for (int i = 0; i < sched.size(); ++i) {
            if (sched.at(i).slot == 0) ++slot0Count;
            if (sched.at(i).slot == 1) ++slot1Count;
            if (sched.at(i).slot == 2) ++slot2Count;
        }
        blockStart += numSamples;
    }

    // Sur numSteps steps numérotés [0, numSteps), le step 0 du pattern
    // se déclenche à chaque multiple du numSteps du pattern.
    // Multiples de N dans [0, numSteps) : 0, N, 2N, ... → floor((numSteps-1)/N) + 1
    auto expectedCount = [&](int64_t patLen) -> int {
        return static_cast<int>((numSteps - 1) / patLen + 1);
    };
    // slot 0 (16 steps) : steps 0,16,32,...,9984 → 625 déclenchements
    REQUIRE(slot0Count == expectedCount(16));
    // slot 1 (32 steps) : steps 0,32,64,...,9984 → 313 déclenchements
    REQUIRE(slot1Count == expectedCount(32));
    // slot 2 (64 steps) : steps 0,64,128,...,9984 → 157 déclenchements
    REQUIRE(slot2Count == expectedCount(64));
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SCHED1 : kNextBlock → exécution à offset 0
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SCHED1: kNextBlock event dispatched at offset 0", "[scheduler]") {
    EventScheduler sched;
    EngineEvent ev{};
    ev.time = kNextBlock;
    ev.type = EventType::Mute;
    ev.slot = 3;
    sched.push(ev);

    int callCount      = 0;
    int32_t capturedOffset = -1;
    sched.processBlock(1000, 512, [&](int32_t offset, const EngineEvent& e) {
        capturedOffset = offset;
        ++callCount;
        REQUIRE(e.type == EventType::Mute);
        REQUIRE(e.slot == 3);
    });

    REQUIRE(callCount == 1);
    REQUIRE(capturedOffset == 0);
    // Après traitement, la file doit être vide
    REQUIRE(sched.size() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SCHED2 : overflow à 257 événements → drop + compteur > 0
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SCHED2: overflow at 257 events increments overflow counter", "[scheduler]") {
    EventScheduler sched;

    for (int i = 0; i < 257; ++i) {
        EngineEvent ev{};
        ev.time = static_cast<int64_t>(i);
        ev.type = EventType::Trigger;
        ev.slot = 0;
        sched.push(ev);
    }

    // Les 256 premiers sont acceptés, le 257e est droppé
    REQUIRE(sched.size() == 256);
    REQUIRE(sched.overflowCount() > 0);
    REQUIRE(sched.overflowCount() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-SCHED3 : split-processing — callback dans l'ordre chronologique
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-SCHED3: processBlock calls callback in chronological order", "[scheduler]") {
    EventScheduler sched;

    // Insérer des événements dans le désordre
    const int64_t blockStart = 1000;
    const int32_t numSamples = 512;

    EngineEvent e3{}, e1{}, e2{}, eNext{};
    e3.time = blockStart + 400; e3.type = EventType::Release; e3.slot = 2;
    e1.time = blockStart + 10;  e1.type = EventType::Trigger;  e1.slot = 0;
    e2.time = blockStart + 200; e2.type = EventType::Mute;     e2.slot = 1;
    eNext.time = kNextBlock;    eNext.type = EventType::Unmute; eNext.slot = 5;

    // Push dans le désordre
    sched.push(e3);
    sched.push(e1);
    sched.push(eNext);
    sched.push(e2);

    std::vector<int32_t> offsets;
    std::vector<EventType> types;

    sched.processBlock(blockStart, numSamples, [&](int32_t offset, const EngineEvent& ev) {
        offsets.push_back(offset);
        types.push_back(ev.type);
    });

    REQUIRE(offsets.size() == 4);

    // kNextBlock en premier (offset 0)
    REQUIRE(offsets[0] == 0);
    REQUIRE(types[0] == EventType::Unmute);

    // Puis dans l'ordre chronologique
    REQUIRE(offsets[1] == 10);
    REQUIRE(types[1] == EventType::Trigger);

    REQUIRE(offsets[2] == 200);
    REQUIRE(types[2] == EventType::Mute);

    REQUIRE(offsets[3] == 400);
    REQUIRE(types[3] == EventType::Release);

    // File vide après traitement
    REQUIRE(sched.size() == 0);
}
