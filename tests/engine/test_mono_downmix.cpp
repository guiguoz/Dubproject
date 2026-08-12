#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/AudioGraph.h"
#include <vector>

// --- T-DOWNMIX: downmix stereo entrelace -> mono (EngineFacade voie mono) ---

TEST_CASE("T-DOWNMIX1: downmix produit (L+R)/2", "[downmix]")
{
    // L = 0.8, R = 0.4 -> mono = 0.6
    const float interleaved[] = { 0.8f, 0.4f, -0.8f, -0.4f, 0.f, 0.f };
    float mono[3];
    engine::downmixInterleavedToMono(interleaved, mono, 3);
    REQUIRE(mono[0] == Catch::Approx(0.6f));
    REQUIRE(mono[1] == Catch::Approx(-0.6f));
    REQUIRE(mono[2] == Catch::Approx(0.0f).margin(1e-7f));
}

TEST_CASE("T-DOWNMIX2: signal un canal seul -> -6 dB en mono", "[downmix]")
{
    // L seul a pleine echelle, R = 0 -> mono = 0.5 (-6 dB)
    const float interleaved[] = { 1.0f, 0.0f, 1.0f, 0.0f };
    float mono[2];
    engine::downmixInterleavedToMono(interleaved, mono, 2);
    REQUIRE(mono[0] == Catch::Approx(0.5f));
    REQUIRE(mono[1] == Catch::Approx(0.5f));
}

TEST_CASE("T-DOWNMIX3: silence -> zeros", "[downmix]")
{
    const float interleaved[] = { 0.f, 0.f, 0.f, 0.f };
    float mono[2];
    engine::downmixInterleavedToMono(interleaved, mono, 2);
    REQUIRE(mono[0] == Catch::Approx(0.0f).margin(1e-7f));
    REQUIRE(mono[1] == Catch::Approx(0.0f).margin(1e-7f));
}
