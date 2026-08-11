#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/Transport.h"

using namespace engine;

// ─── T-TR1 : monotonie de samplePos ─────────────────────────────────────────
TEST_CASE("T-TR1: samplePos strictly monotone while playing", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    t.play();

    int64_t prev = t.state().samplePos;
    for (int i = 0; i < 1000; ++i) {
        const auto& s = t.advance(512);
        REQUIRE(s.samplePos > prev);
        prev = s.samplePos;
    }
}

TEST_CASE("T-TR1b: samplePos frozen when stopped", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    t.play();
    t.advance(512);
    t.stop();
    int64_t pos = t.state().samplePos;
    t.advance(512);
    REQUIRE(t.state().samplePos == pos);
}

// ─── T-TR1c : blockStart = début du bloc courant ─────────────────────────────
// Verrou de non-régression : le snapshot du transport doit exposer le premier
// sample du bloc courant, pour que les lectures (loop sync, triggers) soient
// ancrées au DÉBUT du bloc et non à sa fin (samplePos est avancé en tête de bloc).
TEST_CASE("T-TR1c: blockStart tracks start of the current block", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    REQUIRE(t.state().blockStart == 0);

    t.play();
    t.advance(512);
    // Après advance : samplePos = fin du bloc, blockStart = début du bloc
    REQUIRE(t.state().samplePos  == 512);
    REQUIRE(t.state().blockStart == 0);

    t.advance(128);
    REQUIRE(t.state().samplePos  == 640);
    REQUIRE(t.state().blockStart == 512);

    // En stop, blockStart reste figé (comme samplePos)
    t.stop();
    const int64_t bs = t.state().blockStart;
    t.advance(256);
    REQUIRE(t.state().blockStart == bs);
    REQUIRE(t.state().samplePos   == 640);
}

// ─── T-TR2 : stepIndexAt exact sur 10^9 samples ─────────────────────────────
TEST_CASE("T-TR2: stepIndexAt exact at 1e9 samples (double precision, no float rounding)", "[transport]") {
    // 120 BPM, 44100 Hz
    {
        Transport t;
        t.prepare(44100.0, 120.0);
        t.play();
        const auto& ts = t.state();
        // samplesPerStep = 44100 * 60 / 120 / 4 = 5512.5
        // step at 1e9 = 1000000000 / 5512.5 = 181474.48...  → 181474
        int64_t pos      = 1'000'000'000LL;
        int64_t expected = static_cast<int64_t>(static_cast<double>(pos) / ts.samplesPerStep);
        int64_t got      = stepIndexAt(ts, pos);
        REQUIRE(got == expected);
        // Verify no off-by-one at step boundary
        int64_t stepStart = sampleOfStep(ts, got);
        REQUIRE(stepIndexAt(ts, stepStart) == got);
    }

    // 133.7 BPM — non-round
    {
        Transport t;
        t.prepare(44100.0, 133.7);
        t.play();
        const auto& ts = t.state();
        int64_t pos      = 1'000'000'000LL;
        int64_t expected = static_cast<int64_t>(static_cast<double>(pos) / ts.samplesPerStep);
        int64_t got      = stepIndexAt(ts, pos);
        REQUIRE(got == expected);
    }
}

// ─── T-TR3 : frontières de step stables pour BPM non ronds ─────────────────
TEST_CASE("T-TR3: step boundaries stable for fractional BPM (133.7)", "[transport]") {
    Transport t;
    t.prepare(44100.0, 133.7);
    t.play();
    const auto& ts = t.state();

    // Pour 10 000 steps : sampleOfStep(N) doit pointer sur le step N
    for (int64_t step = 0; step < 10'000; ++step) {
        int64_t pos   = sampleOfStep(ts, step);
        int64_t found = stepIndexAt(ts, pos);
        // Le sample de début du step N doit appartenir au step N
        REQUIRE(found == step);

        // Et le sample précédent appartient au step N-1 (si N > 0)
        if (step > 0 && pos > 0) {
            REQUIRE(stepIndexAt(ts, pos - 1) == step - 1);
        }
    }
}

// ─── T-TR4 : changement de sample rate ──────────────────────────────────────
TEST_CASE("T-TR4: prepare(44100) then prepare(48000) recalculates correctly", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    REQUIRE(t.state().samplesPerBeat == Catch::Approx(22050.0));
    REQUIRE(t.state().samplesPerStep == Catch::Approx(5512.5));

    t.prepare(48000.0, 120.0);
    REQUIRE(t.state().samplesPerBeat == Catch::Approx(24000.0));
    REQUIRE(t.state().samplesPerStep == Catch::Approx(6000.0));
    REQUIRE(t.state().samplePos == 0);
    REQUIRE(t.state().playing == false);

    // Doit fonctionner correctement après re-prepare
    t.play();
    t.advance(6000);
    // On doit être exactement au step 1
    REQUIRE(stepIndexAt(t.state(), t.state().samplePos) == 1);
}

// ─── Helpers purs : beatAt ──────────────────────────────────────────────────
TEST_CASE("beatAt returns correct fractional beat", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    t.play();
    const auto& ts = t.state();
    REQUIRE(beatAt(ts, 0)     == Catch::Approx(0.0));
    REQUIRE(beatAt(ts, 22050) == Catch::Approx(1.0));
    REQUIRE(beatAt(ts, 11025) == Catch::Approx(0.5));
}

// ─── Helpers purs : nextBoundary ────────────────────────────────────────────
TEST_CASE("nextBoundary returns correct absolute sample", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    t.play();
    // samplesPerStep = 5512.5
    // On est à samplePos=0 (step 0), cycle de 16 steps
    // prochaine frontière = step 16 = 16 * 5512.5 = 88200
    t.advance(0);
    int64_t nb = nextBoundary(t.state(), 16);
    REQUIRE(nb == static_cast<int64_t>(16.0 * t.state().samplesPerStep));

    // Avancer à step 7 (pos ~38587), prochaine frontière = step 16
    t.advance(static_cast<int32_t>(7 * t.state().samplesPerStep));
    nb = nextBoundary(t.state(), 16);
    REQUIRE(nb == static_cast<int64_t>(16.0 * t.state().samplesPerStep));
}

// ─── play() remet samplePos à 0 ─────────────────────────────────────────────
TEST_CASE("play() resets samplePos to 0", "[transport]") {
    Transport t;
    t.prepare(44100.0, 120.0);
    t.play();
    t.advance(1024);
    REQUIRE(t.state().samplePos == 1024);
    t.play(); // re-start
    REQUIRE(t.state().samplePos == 0);
}
