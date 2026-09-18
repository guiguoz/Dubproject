// test_nav_nocapture.cpp — T-NAVNOCAP-1/2/3
//
// Invariant : la navigation ne doit pas modifier SceneDefinition.steps.
// SceneDefinition (SceneData.steps) est la source de vérité exclusive des patterns ;
// buildDirectPlan, TransitionPolicy::apply et les opérations de navigation
// engine sont READ-ONLY sur les SceneData.
//
// Avant le fix : captureCurrentScene() depuis navigateScene() pouvait écraser
// sceneStore_[A].filePaths avec des valeurs stales du panel UI.
// Après le fix : captureCurrentScene() n'est plus appelé depuis navigateScene().

#include <catch2/catch_test_macros.hpp>
#include "engine/SceneTransitionPlan.h"
#include "engine/TransitionPolicy.h"
#include "engine/SceneStore.h"
using namespace engine;

// ── Helpers ───────────────────────────────────────────────────────────────────

// FNV-1a sur le tableau steps complet — évite les annulations XOR des patterns réguliers.
static uint32_t stepCRC(const SceneData& sc) noexcept {
    uint32_t h = 2166136261u;
    for (int t = 0; t < kMaxSlots; ++t) {
        const auto& row = sc.steps[static_cast<std::size_t>(t)];
        for (int s = 0; s < 512; ++s) {
            h ^= row[static_cast<std::size_t>(s)] ? 1u : 0u;
            h *= 16777619u;
        }
    }
    return h;
}

// Scène avec steps non-vides : un pas sur 4 actif, slots actifs.
static SceneData makeActiveScene(const char* tag, int numActiveSlots = 4) {
    SceneData sc;
    sc.used = true;
    sc.trackBarCounts.fill(2); // 2 bars = 32 steps
    for (int t = 0; t < numActiveSlots && t < kMaxSlots; ++t) {
        sc.slots[static_cast<std::size_t>(t)].filePath =
            std::string(tag) + "_slot" + std::to_string(t) + ".wav";
        sc.slots[static_cast<std::size_t>(t)].active = true;
        sc.slots[static_cast<std::size_t>(t)].gain   = 1.0f;
        sc.slots[static_cast<std::size_t>(t)].role   = SlotRole::Loop;
        for (int s = 0; s < 32; ++s)
            sc.steps[static_cast<std::size_t>(t)][static_cast<std::size_t>(s)] = (s % 4 == 0);
    }
    // Vérification interne : au moins un step doit être vrai
    bool hasAny = false;
    for (int t = 0; t < numActiveSlots && !hasAny; ++t)
        for (int s = 0; s < 32 && !hasAny; ++s)
            hasAny = sc.steps[static_cast<std::size_t>(t)][static_cast<std::size_t>(s)];
    (void)hasAny;  // non utilisé en Release
    return sc;
}

// ── T-NAVNOCAP-1 ──────────────────────────────────────────────────────────────
TEST_CASE("T-NAVNOCAP-1: steps de A inchangés après buildDirectPlan(A->B) [nav stopped]",
          "[nav][nocapture]")
{
    SceneData a = makeActiveScene("A", 4);
    SceneData b = makeActiveScene("B", 6);

    // Vérification directe : step 0 de track 0 doit être vrai
    REQUIRE(a.steps[0][0] == true);
    REQUIRE(a.steps[0][1] == false);

    const uint32_t crcABefore  = stepCRC(a);
    const uint32_t barABefore  = [&]{
        uint32_t c = 0;
        for (int t = 0; t < kMaxSlots; ++t)
            c ^= static_cast<uint32_t>(a.trackBarCounts[static_cast<std::size_t>(t)] * 17 + t + 1);
        return c;
    }();
    REQUIRE(crcABefore != 0);

    // buildDirectPlan est READ-ONLY sur a et b
    SceneTransitionPlan plan;
    buildDirectPlan(a, b, 0, 1, 50000, 44100.0, plan);
    REQUIRE(plan.valid);

    REQUIRE(stepCRC(a) == crcABefore);

    // TransitionPolicy::apply aussi READ-ONLY sur a
    PolicyContext ctx{44100.0, 120.0, 4, 4, 50000};
    TransitionPolicy::apply(plan, ctx);

    REQUIRE(stepCRC(a) == crcABefore);
    REQUIRE([&]{
        uint32_t c = 0;
        for (int t = 0; t < kMaxSlots; ++t)
            c ^= static_cast<uint32_t>(a.trackBarCounts[static_cast<std::size_t>(t)] * 17 + t + 1);
        return c;
    }() == barABefore);
}

// ── T-NAVNOCAP-2 ──────────────────────────────────────────────────────────────
TEST_CASE("T-NAVNOCAP-2: 50x A<->B stopped — stepCRC(A) et stepCRC(B) constants",
          "[nav][nocapture][stress]")
{
    SceneData a = makeActiveScene("A", 4);
    SceneData b = makeActiveScene("B", 6);

    REQUIRE(a.steps[0][0] == true);
    REQUIRE(b.steps[0][0] == true);

    const uint32_t crcA = stepCRC(a);
    const uint32_t crcB = stepCRC(b);
    REQUIRE(crcA != 0);
    REQUIRE(crcB != 0);
    REQUIRE(crcA != crcB); // scènes distinctes : nombre de slots actifs différent → patterns différents

    for (int iter = 0; iter < 50; ++iter) {
        const int64_t T1 = 10000 + iter * 200000LL;
        const int64_t T2 = T1 + 100000;

        SceneTransitionPlan p1;
        buildDirectPlan(a, b, 0, 1, T1, 44100.0, p1);
        PolicyContext ctx1{44100.0, 120.0, 4, 4, T1};
        TransitionPolicy::apply(p1, ctx1);

        SceneTransitionPlan p2;
        buildDirectPlan(b, a, 1, 0, T2, 44100.0, p2);
        PolicyContext ctx2{44100.0, 120.0, 4, 4, T2};
        TransitionPolicy::apply(p2, ctx2);

        REQUIRE(stepCRC(a) == crcA);
        REQUIRE(stepCRC(b) == crcB);
    }
}

// ── T-NAVNOCAP-3 ──────────────────────────────────────────────────────────────
TEST_CASE("T-NAVNOCAP-3: nav playing A->B->A avec policy V2 — SceneDefinition.steps inchangée",
          "[nav][nocapture][playing]")
{
    TransitionPolicy::setPolicyMode(PolicyMode::V2);
    struct V2Guard { ~V2Guard() { TransitionPolicy::setPolicyMode(PolicyMode::V1); } } guard;

    SceneData a = makeActiveScene("A", 2);
    SceneData b = makeActiveScene("B", 6);
    b.slots[2].role = SlotRole::Kick;
    b.slots[3].role = SlotRole::Pad;
    b.slots[4].role = SlotRole::Perc;
    b.slots[5].role = SlotRole::Fx;

    REQUIRE(a.steps[0][0] == true);

    const uint32_t crcA = stepCRC(a);
    const uint32_t crcB = stepCRC(b);
    REQUIRE(crcA != 0);

    // A→B (playing, DIRECT avec plan + policy V2)
    SceneTransitionPlan pAB;
    buildDirectPlan(a, b, 0, 1, 100000, 44100.0, pAB);
    PolicyContext ctxAB{44100.0, 120.0, 4, 4, 100000};
    TransitionPolicy::apply(pAB, ctxAB);

    REQUIRE(stepCRC(a) == crcA);
    REQUIRE(stepCRC(b) == crcB);

    // B→A (retour)
    SceneTransitionPlan pBA;
    buildDirectPlan(b, a, 1, 0, 200000, 44100.0, pBA);
    PolicyContext ctxBA{44100.0, 120.0, 4, 4, 200000};
    TransitionPolicy::apply(pBA, ctxBA);

    REQUIRE(stepCRC(a) == crcA);
    REQUIRE(stepCRC(b) == crcB);
}

// ── T-NAVNOCAP-4 ──────────────────────────────────────────────────────────────
TEST_CASE("T-NAVNOCAP-4: isUsedDerived() — steps ou filePath → true, vide → false",
          "[nav][nocapture][used]")
{
    // Scène vide : aucun step, aucun filePath, used=false
    SceneData empty;
    empty.used = false;
    REQUIRE(empty.isUsedDerived() == false);

    // Scène avec steps mais used=false stocké (cas qui causait le bug)
    SceneData withSteps = makeActiveScene("A", 4);
    withSteps.used = false;
    REQUIRE(withSteps.isUsedDerived() == true);

    // Scène avec filePath uniquement, sans steps, used=false
    SceneData withPath;
    withPath.used = false;
    withPath.slots[0].filePath = "drum.wav";
    REQUIRE(withPath.isUsedDerived() == true);

    // Scène avec used=true mais contenu vide → dérivé = false
    SceneData staleUsed;
    staleUsed.used = true;
    REQUIRE(staleUsed.isUsedDerived() == false);
}

// ── T-NAVNOCAP-5 ──────────────────────────────────────────────────────────────
TEST_CASE("T-NAVNOCAP-5: stepCRC non nul pour scène active, indépendamment de usedStored",
          "[nav][nocapture][used]")
{
    SceneData sc = makeActiveScene("A", 4);
    sc.used = false;  // used non positionné (scène non capturée, cas du bug)

    REQUIRE(sc.isUsedDerived() == true);
    REQUIRE(sc.steps[0][0] == true);

    const uint32_t crc0 = stepCRC(sc);
    REQUIRE(crc0 != 0u);

    // buildDirectPlan + apply ne modifient pas les steps même si used=false
    SceneData b = makeActiveScene("B", 6);
    SceneTransitionPlan plan;
    buildDirectPlan(sc, b, 0, 1, 50000, 44100.0, plan);
    PolicyContext ctx{44100.0, 120.0, 4, 4, 50000};
    TransitionPolicy::apply(plan, ctx);

    REQUIRE(stepCRC(sc) == crc0);
    REQUIRE(sc.isUsedDerived() == true);
}
