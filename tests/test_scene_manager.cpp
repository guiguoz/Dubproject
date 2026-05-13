#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/SceneManager.h"
#include "dsp/Sampler.h"

using dsp::SceneManager;
using dsp::SceneData;
using dsp::CrossfadeCurve;
using Catch::Matchers::WithinAbs;

// ── chooseProfile ─────────────────────────────────────────────────────────────

TEST_CASE("chooseProfile -- Musical->Calme : 400 ms EaseIn", "[scene_manager]")
{
    const auto p = SceneManager::chooseProfile(0.5f, 0.05f);
    REQUIRE(p.durationMs == 400);
    REQUIRE(p.curve == CrossfadeCurve::EaseIn);
}

TEST_CASE("chooseProfile -- Calme->Musical : 120 ms EaseOut", "[scene_manager]")
{
    const auto p = SceneManager::chooseProfile(0.05f, 0.5f);
    REQUIRE(p.durationMs == 120);
    REQUIRE(p.curve == CrossfadeCurve::EaseOut);
}

TEST_CASE("chooseProfile -- Musical->Musical : 200 ms Linear", "[scene_manager]")
{
    const auto p = SceneManager::chooseProfile(0.5f, 0.9f);
    REQUIRE(p.durationMs == 200);
    REQUIRE(p.curve == CrossfadeCurve::Linear);
}

TEST_CASE("chooseProfile -- Calme->Calme : 250 ms Smoothstep", "[scene_manager]")
{
    const auto p = SceneManager::chooseProfile(0.05f, 0.10f);
    REQUIRE(p.durationMs == 250);
    REQUIRE(p.curve == CrossfadeCurve::Smoothstep);
}

TEST_CASE("chooseProfile -- boundary kT=0.20 compte comme Musical", "[scene_manager]")
{
    const auto p = SceneManager::chooseProfile(0.20f, 0.05f);
    REQUIRE(p.durationMs == 400);
    REQUIRE(p.curve == CrossfadeCurve::EaseIn);
}

TEST_CASE("chooseProfile -- fromE==toE Musical produit Musical->Musical", "[scene_manager]")
{
    const auto p = SceneManager::chooseProfile(0.5f, 0.5f);
    REQUIRE(p.durationMs == 200);
    REQUIRE(p.curve == CrossfadeCurve::Linear);
}

// ── updateCrossfade — bornes et absence d'overshoot ──────────────────────────

TEST_CASE("updateCrossfade -- linear : gains finaux == target, pas d'overshoot", "[scene_manager]")
{
    SceneManager sm;
    dsp::Sampler sampler;
    sampler.prepare(44100.0, 512);

    std::array<float, 9> start{}, target{};
    target.fill(1.0f);
    sm.armAdaptiveCrossfade(start, target, 0.5f, 0.5f); // Musical->Musical : 200 ms Linear

    // Avancer au-delà de la durée (10 x 30 ms = 300 ms > 200 ms)
    for (int i = 0; i < 10; ++i)
        sm.updateCrossfade(30, sampler);

    // Gains finaux == targetGains, pas d'overshoot
    for (int i = 0; i < 9; ++i)
        REQUIRE_THAT(sampler.getSlotGain(i), WithinAbs(1.f, 1e-3f));
    REQUIRE(!sm.isCrossfadeActive());
}

TEST_CASE("updateCrossfade -- gains dans [start, target] pendant interpolation", "[scene_manager]")
{
    SceneManager sm;
    dsp::Sampler sampler;
    sampler.prepare(44100.0, 512);

    std::array<float, 9> start{}, target{};
    start.fill(0.2f);
    target.fill(0.8f);
    sm.armAdaptiveCrossfade(start, target, 0.5f, 0.5f); // Linear

    for (int tick = 0; tick < 8; ++tick)
    {
        sm.updateCrossfade(25, sampler);
        for (int i = 0; i < 9; ++i)
        {
            const float g = sampler.getSlotGain(i);
            REQUIRE(g >= 0.2f - 1e-3f);
            REQUIRE(g <= 0.8f + 1e-3f);
        }
    }
}

// ── computeSceneEnergy ────────────────────────────────────────────────────────

TEST_CASE("computeSceneEnergy -- scene non utilisee retourne 0", "[scene_manager]")
{
    SceneData sc;
    sc.used = false;
    REQUIRE(SceneManager::computeSceneEnergy(sc) == 0.f);
}

TEST_CASE("computeSceneEnergy -- tous slots mutes retourne 0", "[scene_manager]")
{
    SceneData sc;
    sc.used = true;
    sc.mutes.fill(true);
    for (auto& p : sc.filePaths) p = "x";
    REQUIRE(SceneManager::computeSceneEnergy(sc) == 0.f);
}

TEST_CASE("computeSceneEnergy -- 1 slot actif 1 pas : energie dans ]0, 1/9]", "[scene_manager]")
{
    SceneData sc;
    sc.used = true;
    sc.filePaths[0] = "x";
    sc.trackBarCounts[0] = 1;
    sc.steps[0][0] = true;
    const float e = SceneManager::computeSceneEnergy(sc);
    REQUIRE(e > 0.f);
    REQUIRE(e <= 1.f / 9.f + 1e-4f);
}

TEST_CASE("computeSceneEnergy -- 9 slots plein non mutes : energie proche de 1", "[scene_manager]")
{
    SceneData sc;
    sc.used = true;
    for (int i = 0; i < 9; ++i)
    {
        sc.filePaths[i] = "x";
        sc.trackBarCounts[i] = 1;
        for (int s = 0; s < 16; ++s)
            sc.steps[i][static_cast<std::size_t>(s)] = true;
    }
    const float e = SceneManager::computeSceneEnergy(sc);
    REQUIRE_THAT(e, WithinAbs(1.f, 1e-4f));
}
