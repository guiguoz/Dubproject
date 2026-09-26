#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "engine/SceneTransitionPlan.h"
#include "engine/AutoMixDub.h"
using namespace engine;

// ── Helpers ───────────────────────────────────────────────────────────────────

static SceneData makeScene(const std::vector<std::pair<SlotRole, PlayMode>>& specs,
                           bool active = true)
{
    SceneData sc; sc.used = true;
    for (int i = 0; i < kMaxSlots; ++i) {
        sc.slots[i].active = false;
        sc.trackBarCounts[i] = 1;
    }
    for (int i = 0; i < (int)specs.size() && i < kMaxSlots; ++i) {
        sc.slots[i].filePath = std::string("slot") + std::to_string(i) + ".wav";
        sc.slots[i].role     = specs[i].first;
        sc.slots[i].mode     = specs[i].second;
        sc.slots[i].active   = active;
        sc.slots[i].gain     = 1.f;
    }
    return sc;
}

static int countSendRamps(const SceneTransitionPlan& plan)
{
    int n = 0;
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].type == PlanEventType::SendRamp) ++n;
    return n;
}

static bool hasSendRampForSlot(const SceneTransitionPlan& plan, int slot)
{
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].type == PlanEventType::SendRamp
                && plan.events[i].slot == static_cast<uint8_t>(slot))
            return true;
    return false;
}

// ── DUB_NEVER_KICK_BASS ───────────────────────────────────────────────────────
TEST_CASE("DUB_NEVER_KICK_BASS: Kick et Bass ne reçoivent jamais de SendRamp",
          "[dub][policy]")
{
    // 2 LEAVE (Kick, Bass) — Force mode bypass le churn check
    SceneData from = makeScene({{SlotRole::Kick, PlayMode::Free},
                                {SlotRole::Bass, PlayMode::Free}});
    SceneData to;   // vide

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, 44100, 44100.0, plan);
    applyDubModifier(plan, from, to, 44100.f, 0.0, DubMode::Force);

    REQUIRE(countSendRamps(plan) == 0);
    REQUIRE_FALSE(plan.dubActive);
}

// ── DUB_ELIGIBLE_LOOP ─────────────────────────────────────────────────────────
TEST_CASE("DUB_ELIGIBLE_LOOP: slot Loop (Free) LEAVE reçoit 2 SendRamp events",
          "[dub][policy]")
{
    SceneData from = makeScene({{SlotRole::Loop, PlayMode::Free}});
    SceneData to;   // vide

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, 44100, 44100.0, plan);
    applyDubModifier(plan, from, to, 44100.f, 0.0, DubMode::Force);

    // 2 SendRamp pour slot 0 (ramp-up + tail)
    REQUIRE(countSendRamps(plan) == 2);
    REQUIRE(hasSendRampForSlot(plan, 0));
    REQUIRE(plan.dubActive);

    // Le premier SendRamp a un target > 0 (ramp-up), le second target = 0 (tail)
    int upCount = 0, downCount = 0;
    for (int i = 0; i < plan.numEvents; ++i) {
        if (plan.events[i].type != PlanEventType::SendRamp) continue;
        if (plan.events[i].b > 0.f) ++upCount;
        else                         ++downCount;
    }
    REQUIRE(upCount == 1);
    REQUIRE(downCount == 1);
}

// ── DUB_ONESHOT_NO_TRIGGER ────────────────────────────────────────────────────
TEST_CASE("DUB_ONESHOT_NO_TRIGGER: OneShot sans step dans la fenêtre → pas de SendRamp",
          "[dub][policy][oneshot]")
{
    SceneData from = makeScene({{SlotRole::Loop, PlayMode::OneShot}});
    SceneData to;   // vide
    // Aucun step activé dans le pattern

    const int64_t boundary      = 10000;
    const double  samplesPerStep = 1000.0;

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, boundary, 44100.0, plan);
    applyDubModifier(plan, from, to, 44100.f, samplesPerStep, DubMode::Force);

    REQUIRE(countSendRamps(plan) == 0);
    REQUIRE_FALSE(plan.dubActive);
}

// ── DUB_ONESHOT_WITH_TRIGGER ──────────────────────────────────────────────────
TEST_CASE("DUB_ONESHOT_WITH_TRIGGER: OneShot avec step dans la fenêtre → 2 SendRamp",
          "[dub][policy][oneshot]")
{
    SceneData from = makeScene({{SlotRole::Loop, PlayMode::OneShot}});
    SceneData to;   // vide

    const int64_t boundary      = 10000;
    const double  samplesPerStep = 1000.0;

    // Step 9 est dans [leaveAt/spstep - 2, leaveAt/spstep] = [8, 10]
    from.steps[0][9] = true;

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, boundary, 44100.0, plan);
    applyDubModifier(plan, from, to, 44100.f, samplesPerStep, DubMode::Force);

    REQUIRE(countSendRamps(plan) == 2);
    REQUIRE(hasSendRampForSlot(plan, 0));
    REQUIRE(plan.dubActive);
}

// ── DUB_PLAN_NOOVERFLOW ───────────────────────────────────────────────────────
TEST_CASE("DUB_PLAN_NOOVERFLOW: 9 slots LEAVE éligibles → numEvents <= 96",
          "[dub][policy][safety]")
{
    // 9 slots LEAVE (tous Loop, Free) + 0 ENTER → churn = 9
    SceneData from = makeScene({{SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free},
                                {SlotRole::Loop, PlayMode::Free}});
    SceneData to;   // vide

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, 44100, 44100.0, plan);
    applyDubModifier(plan, from, to, 44100.f, 0.0, DubMode::Force);

    REQUIRE(plan.numEvents <= 96);
    REQUIRE(plan.dubActive);
    // 9 LEAVE × 2 base events = 18, + 9 × 2 SendRamp = 18 → total 36
    REQUIRE(countSendRamps(plan) == 18);
}

// ── DUB_TAIL_TIMING ───────────────────────────────────────────────────────────
TEST_CASE("DUB_TAIL_TIMING: ramp-up et tail ne sont pas au même sample (évite conflit même bloc)",
          "[dub][policy][timing]")
{
    const float sr = 44100.f;
    const int64_t boundary = 44100;  // = 1 s

    SceneData from = makeScene({{SlotRole::Loop, PlayMode::Free}});
    SceneData to;

    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, boundary, static_cast<double>(sr), plan);
    applyDubModifier(plan, from, to, sr, 0.0, DubMode::Force);

    // Trouver les 2 SendRamp pour slot 0
    int64_t rampUpSample = -1, tailSample = -1;
    for (int i = 0; i < plan.numEvents; ++i) {
        if (plan.events[i].type != PlanEventType::SendRamp) continue;
        if (plan.events[i].slot != 0) continue;
        if (plan.events[i].b > 0.f) rampUpSample = plan.events[i].atSample;
        else                          tailSample   = plan.events[i].atSample;
    }

    REQUIRE(rampUpSample != -1);
    REQUIRE(tailSample   != -1);
    // Le tail doit être APRÈS le ramp-up
    REQUIRE(tailSample > rampUpSample);
    // L'écart doit être exactement rampSamples (round(sr * 0.010))
    const int64_t expectedGap = static_cast<int64_t>(std::round(sr * 0.010f));
    REQUIRE(tailSample - rampUpSample == expectedGap);
}

// ── DUB_SEND_RETURNS_TO_ZERO ──────────────────────────────────────────────────
TEST_CASE("DUB_SEND_RETURNS_TO_ZERO: après ramp-up + tail, le send AutoMixDub revient à 0",
          "[dub][policy][automix]")
{
    const float sr = 44100.f;
    const int blockSize = 512;
    const int rampSamples = static_cast<int>(std::round(sr * 0.010f));  // ~441
    const int tailSamples = static_cast<int>(std::round(sr * 1.2f));    // ~52920

    AutoMixDub mix;
    mix.prepare(sr);

    // Armer ramp-up (slot 0 : 0→0.55 sur rampSamples)
    mix.scheduleSendRamp(0, 0.55f, rampSamples);

    // Avancer jusqu'à la fin du ramp-up (un seul bloc > rampSamples suffit car frac=1.0)
    mix.advanceDelayRamp(0, rampSamples);   // consomme durLeft exact → current snaps to target
    // Le send doit être exactement 0.55
    const float afterRamp = mix.advanceDelayRamp(0, 1);
    REQUIRE(afterRamp >= 0.50f);

    // Armer tail (slot 0 : 0.55→0 sur tailSamples)
    mix.scheduleSendRamp(0, 0.f, tailSamples);

    // Avancer jusqu'à la fin du tail
    {
        int processed = 0;
        while (processed < tailSamples + blockSize) {
            mix.advanceDelayRamp(0, blockSize);
            processed += blockSize;
        }
    }
    // Le send doit être essentiellement 0
    const float afterTail = mix.advanceDelayRamp(0, 1);
    REQUIRE(afterTail < 0.001f);
}
