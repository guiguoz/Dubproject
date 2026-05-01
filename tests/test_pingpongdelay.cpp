#include <cmath>
#include <vector>
#include "catch2/catch_test_macros.hpp"
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

    // Basic sanity: outputs should be finite numbers and not NaN
    for(int i=0;i<N;++i){ REQUIRE(std::isfinite(outL[i])); REQUIRE(std::isfinite(outR[i])); }
}
