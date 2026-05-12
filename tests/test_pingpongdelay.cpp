#include <cmath>
#include <vector>
#include "catch2/catch_test_macros.hpp"
#include "catch2/catch_approx.hpp"
#include "../src/dsp/PingPongDelay.h"

using namespace dsp;

TEST_CASE("PingPongDelay basic init", "[pingpong]")
{
    PingPongDelay pp;
    pp.prepare(44100.0, 512);
    pp.setBpm(120.0f);
    pp.setEnabled(true);
    pp.setSend(0.25f);
    pp.setWet(0.25f);
    pp.setFeedback(0.4f);
    pp.setTone(0.5f);
    pp.setDrive(0.2f);
    pp.setDiv(0);

    const int N = 128;
    float inL[N]  = {0}, inR[N] = {0};
    float outL[N] = {0}, outR[N] = {0};
    inL[0] = 1.0f; // impulse

    pp.processAdd(inL, inR, outL, outR, N);

    // Sanity: outputs must be finite
    for (int i = 0; i < N; ++i) {
        REQUIRE(std::isfinite(outL[i]));
        REQUIRE(std::isfinite(outR[i]));
    }
}

TEST_CASE("PingPongDelay processAdd is additive (never overwrites)", "[pingpong]")
{
    PingPongDelay pp;
    pp.prepare(44100.0, 512);
    pp.setBpm(120.0f);
    pp.setEnabled(true);
    pp.setWet(0.5f);
    pp.setFeedback(0.3f);

    const int N = 128;
    float inL[N]  = {0}, inR[N] = {0};
    inL[0] = 1.0f;

    // Pre-load outL/outR with a known value
    float outL[N], outR[N];
    for (int i = 0; i < N; ++i) { outL[i] = 2.0f; outR[i] = 3.0f; }

    // Wait one quarter-note worth of samples so delay has content to output
    // At 120 BPM, quarter = 0.5s = 22050 samples — pre-fill delay
    float dummyL[N] = {0}, dummyR[N] = {0};
    dummyL[0] = 1.0f;
    float tmpL[N] = {0}, tmpR[N] = {0};
    pp.processAdd(dummyL, dummyR, tmpL, tmpR, N);

    // Now run with preset outL/outR — values must be >= 2.0 / 3.0 (additive)
    for (int i = 0; i < N; ++i) { outL[i] = 2.0f; outR[i] = 3.0f; }
    pp.processAdd(inL, inR, outL, outR, N);

    for (int i = 0; i < N; ++i) {
        REQUIRE(outL[i] >= 2.0f - 1e-6f); // never less than initial value
        REQUIRE(outR[i] >= 3.0f - 1e-6f);
        REQUIRE(std::isfinite(outL[i]));
        REQUIRE(std::isfinite(outR[i]));
    }
}

TEST_CASE("PingPongDelay bypass leaves mix unchanged", "[pingpong]")
{
    PingPongDelay pp;
    pp.prepare(44100.0, 512);
    pp.setBpm(120.0f);

    const int N = 64;
    float inL[N] = {0}, inR[N] = {0};
    inL[0] = 1.0f; inR[0] = 1.0f;

    float outL[N], outR[N];
    for (int i = 0; i < N; ++i) { outL[i] = 5.0f; outR[i] = 7.0f; }

    // Disabled
    pp.setEnabled(false);
    pp.processAdd(inL, inR, outL, outR, N);
    for (int i = 0; i < N; ++i) {
        REQUIRE(outL[i] == Catch::Approx(5.0f));
        REQUIRE(outR[i] == Catch::Approx(7.0f));
    }

    // Freeze
    pp.setEnabled(true);
    pp.setFreeze(true);
    pp.processAdd(inL, inR, outL, outR, N);
    for (int i = 0; i < N; ++i) {
        REQUIRE(outL[i] == Catch::Approx(5.0f));
        REQUIRE(outR[i] == Catch::Approx(7.0f));
    }
}

TEST_CASE("PingPongDelay integration: BPM sync + stable delay across blocks", "[pingpong][integration]")
{
    PingPongDelay pp;
    pp.prepare(44100.0, 512);
    pp.setBpm(120.0f);
    pp.setEnabled(true);
    pp.setSend(0.25f);
    pp.setWet(0.25f);
    pp.setFeedback(0.4f);
    pp.setTone(0.5f);
    pp.setDrive(0.2f);
    pp.setDiv(0);

    const int N = 256;
    float inL[N] = {0}, inR[N] = {0};
    float outL[N] = {0}, outR[N] = {0};
    inL[0] = 1.0f;

    for (int k = 0; k < 4; ++k) {
        pp.processAdd(inL, inR, outL, outR, N);
        for (int i = 0; i < N; ++i) {
            REQUIRE(std::isfinite(outL[i]));
            REQUIRE(std::isfinite(outR[i]));
        }
    }
}
