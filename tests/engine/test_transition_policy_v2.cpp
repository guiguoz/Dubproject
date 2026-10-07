#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "engine/TransitionPolicy.h"
#include "engine/SceneTransitionPlan.h"
using namespace engine;

// ── Helpers ───────────────────────────────────────────────────────────────────

struct SlotSpec { std::string path; SlotRole role; };

static SceneData makeSceneV2(const std::vector<SlotSpec>& specs) {
    SceneData sc; sc.used = true;
    for (int i = 0; i < kMaxSlots; ++i) {
        sc.slots[i].active = false;
        sc.steps[i].fill(false);
        sc.trackBarCounts[i] = 1;
    }
    for (int i = 0; i < (int)specs.size() && i < kMaxSlots; ++i) {
        sc.slots[i].filePath = specs[i].path;
        sc.slots[i].active   = true;
        sc.slots[i].gain     = 1.f;
        sc.slots[i].role     = specs[i].role;
    }
    return sc;
}

static int64_t pcmFlipAt(const SceneTransitionPlan& plan, int slot) {
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].slot == slot && plan.events[i].type == PlanEventType::PcmFlip)
            return plan.events[i].atSample;
    return -1;
}

static int64_t gainRampAt(const SceneTransitionPlan& plan, int slot) {
    for (int i = 0; i < plan.numEvents; ++i)
        if (plan.events[i].slot == slot && plan.events[i].type == PlanEventType::GainRamp)
            return plan.events[i].atSample;
    return -1;
}

// RAII pour remettre le mode V1 après chaque test
struct V2Guard {
    V2Guard()  { TransitionPolicy::setPolicyMode(PolicyMode::V2); }
    ~V2Guard() { TransitionPolicy::setPolicyMode(PolicyMode::V1); }
};

// ── T-POLICYV2-BUILD-ROLES ────────────────────────────────────────────────────
TEST_CASE("T-POLICYV2-BUILD-ROLES: ENTER slots arrive selon leur role (Perc/Pad/Loop/Fx)",
          "[policy][v2][build]")
{
    V2Guard v2;
    // KEEP: slots 0,1 (Loop)  ENTER: slots 2(Perc) 3(Pad) 4(Loop) 5(Fx)
    SceneData from = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop}});
    SceneData to   = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop},
                                  {"c.wav",SlotRole::Perc},{"d.wav",SlotRole::Pad},
                                  {"e.wav",SlotRole::Loop},{"f.wav",SlotRole::Fx}});
    const int64_t T = 10000;
    SceneTransitionPlan plan; buildDirectPlan(from, to, 0, 1, T, 44100.0, plan);
    PolicyContext ctx{44100.0, 120.0, 4, 4, T};

    REQUIRE(TransitionPolicy::choose(plan) == PolicyType::Build);
    TransitionPolicy::apply(plan, ctx);

    const int64_t beat = TransitionPolicy::samplesPerBeat(ctx);
    REQUIRE(pcmFlipAt(plan, 2) == T + 1 * beat); // Perc → beat 1
    REQUIRE(pcmFlipAt(plan, 3) == T + 2 * beat); // Pad  → beat 2
    REQUIRE(pcmFlipAt(plan, 4) == T + 2 * beat); // Loop → beat 2
    REQUIRE(pcmFlipAt(plan, 5) == T + 3 * beat); // Fx   → beat 3
}

// ── T-POLICYV2-BUILD-KICK0 ────────────────────────────────────────────────────
TEST_CASE("T-POLICYV2-BUILD-KICK0: Kick et Bass entrent au beat 0 (== T)",
          "[policy][v2][build]")
{
    V2Guard v2;
    SceneData from = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop}});
    SceneData to   = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop},
                                  {"c.wav",SlotRole::Kick},{"d.wav",SlotRole::Bass},
                                  {"e.wav",SlotRole::Pad}, {"f.wav",SlotRole::Fx}});
    const int64_t T = 10000;
    SceneTransitionPlan plan; buildDirectPlan(from, to, 0, 1, T, 44100.0, plan);
    PolicyContext ctx{44100.0, 120.0, 4, 4, T};

    REQUIRE(TransitionPolicy::choose(plan) == PolicyType::Build);
    TransitionPolicy::apply(plan, ctx);

    const int64_t beat = TransitionPolicy::samplesPerBeat(ctx);
    REQUIRE(pcmFlipAt(plan, 2) == T);               // Kick → beat 0
    REQUIRE(pcmFlipAt(plan, 3) == T);               // Bass → beat 0
    REQUIRE(pcmFlipAt(plan, 4) == T + 2 * beat);    // Pad  → beat 2
    REQUIRE(pcmFlipAt(plan, 5) == T + 3 * beat);    // Fx   → beat 3
}

// ── T-POLICYV2-BREAKDOWN-ROLES ────────────────────────────────────────────────
TEST_CASE("T-POLICYV2-BREAKDOWN-ROLES: LEAVE slots quittent selon leur role (Fx/Pad/Perc/Kick)",
          "[policy][v2][breakdown]")
{
    V2Guard v2;
    // KEEP: slots 0,1  LEAVE: slots 2(Fx) 3(Pad) 4(Perc) 5(Kick)
    SceneData from = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop},
                                  {"c.wav",SlotRole::Fx},  {"d.wav",SlotRole::Pad},
                                  {"e.wav",SlotRole::Perc},{"f.wav",SlotRole::Kick}});
    SceneData to   = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop}});
    const int64_t T = 100000; // assez grand pour eviter atSample < 0
    SceneTransitionPlan plan; buildDirectPlan(from, to, 0, 1, T, 44100.0, plan);
    PolicyContext ctx{44100.0, 120.0, 4, 4, T};

    REQUIRE(TransitionPolicy::choose(plan) == PolicyType::Breakdown);
    TransitionPolicy::apply(plan, ctx);

    // Option A (clamp cycle sortant intangible) : tous les LEAVE restent à T.
    // L'étalement par rôle (Fx -3 beats, Pad -2, Perc -1) était avant la frontière
    // → clampé. Les fades s'enchaînent immédiatement à T, pas avant.
    REQUIRE(gainRampAt(plan, 2) == T); // Fx   → clamped to T
    REQUIRE(gainRampAt(plan, 3) == T); // Pad  → clamped to T
    REQUIRE(gainRampAt(plan, 4) == T); // Perc → clamped to T
    REQUIRE(gainRampAt(plan, 5) == T); // Kick → T (inchangé)
}

// ── T-POLICYV2-BREAKDOWN-INVARIANT ───────────────────────────────────────────
TEST_CASE("T-POLICYV2-BREAKDOWN-INVARIANT: releaseAt == leaveAt + fadeSamples",
          "[policy][v2][breakdown][invariant]")
{
    V2Guard v2;
    for (double sr : {44100.0, 48000.0}) {
        SceneData from = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop},
                                      {"c.wav",SlotRole::Fx},  {"d.wav",SlotRole::Pad},
                                      {"e.wav",SlotRole::Perc},{"f.wav",SlotRole::Kick}});
        SceneData to   = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop}});
        const int64_t T = 200000;
        SceneTransitionPlan plan; buildDirectPlan(from, to, 0, 1, T, sr, plan);
        PolicyContext ctx{sr, 120.0, 4, 4, T};
        TransitionPolicy::apply(plan, ctx);

        const int fade = static_cast<int>(std::round(sr * 0.010));
        for (int s = 0; s < kMaxSlots; ++s) {
            if (plan.slots[s].action != SlotPlanAction::Leave) continue;
            int64_t leaveAt = -1, releaseAt = -1;
            for (int i = 0; i < plan.numEvents; ++i) {
                if (plan.events[i].slot != s) continue;
                if (plan.events[i].type == PlanEventType::GainRamp)  leaveAt   = plan.events[i].atSample;
                if (plan.events[i].type == PlanEventType::Release)   releaseAt = plan.events[i].atSample;
            }
            REQUIRE(leaveAt   != -1);
            REQUIRE(releaseAt != -1);
            INFO("sr=" << sr << " slot=" << s << " leaveAt=" << leaveAt << " fade=" << fade);
            REQUIRE(releaseAt == leaveAt + fade);
        }
    }
}

// ── T-POLICYV2-KEEP-ZERO ──────────────────────────────────────────────────────
TEST_CASE("T-POLICYV2-KEEP-ZERO: V2 ne génère aucun event pour les slots KEEP",
          "[policy][v2]")
{
    V2Guard v2;
    // Scènes identiques → tout KEEP, DirectType, aucun event
    SceneData sc = makeSceneV2({{"a.wav",SlotRole::Kick},{"b.wav",SlotRole::Bass}});
    SceneTransitionPlan plan; buildDirectPlan(sc, sc, 0, 1, 5000, 44100.0, plan);
    for (int s = 0; s < kMaxSlots; ++s)
        REQUIRE(plan.slots[s].action == SlotPlanAction::Keep);
    REQUIRE(plan.numEvents == 0);
}

// ── T-POLICYV2-STRESS-ABA ────────────────────────────────────────────────────
TEST_CASE("T-POLICYV2-STRESS-ABA: 100x A->B->A sans crash ni event pour KEEP",
          "[policy][v2][stress]")
{
    V2Guard v2;
    SceneData a = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop}});
    SceneData b = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop},
                                {"c.wav",SlotRole::Kick},{"d.wav",SlotRole::Bass},
                                {"e.wav",SlotRole::Pad}, {"f.wav",SlotRole::Fx}});
    PolicyContext ctx{44100.0, 120.0, 4, 4, 0};

    for (int iter = 0; iter < 100; ++iter) {
        const int64_t T1 = 10000 + iter * 100000LL;
        const int64_t T2 = T1 + 200000;
        ctx.boundary = T1;

        SceneTransitionPlan p1; buildDirectPlan(a, b, 0, 1, T1, 44100.0, p1);
        TransitionPolicy::apply(p1, ctx);

        ctx.boundary = T2;
        SceneTransitionPlan p2; buildDirectPlan(b, a, 1, 0, T2, 44100.0, p2);
        TransitionPolicy::apply(p2, ctx);

        // KEEP sacré : aucun event ne touche un slot Keep
        for (int i = 0; i < p1.numEvents; ++i)
            REQUIRE(p1.slots[p1.events[i].slot].action != SlotPlanAction::Keep);
        for (int i = 0; i < p2.numEvents; ++i)
            REQUIRE(p2.slots[p2.events[i].slot].action != SlotPlanAction::Keep);
    }
}

// ── T-POLICYV2-BLOCKSIZE-INDEP ───────────────────────────────────────────────
TEST_CASE("T-POLICYV2-BLOCKSIZE-INDEP: PcmFlip atSample identique quel que soit le SR du plan builder",
          "[policy][v2][build]")
{
    V2Guard v2;
    SceneData from = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop}});
    SceneData to   = makeSceneV2({{"a.wav",SlotRole::Loop},{"b.wav",SlotRole::Loop},
                                  {"c.wav",SlotRole::Kick},{"d.wav",SlotRole::Pad},
                                  {"e.wav",SlotRole::Perc},{"f.wav",SlotRole::Fx}});
    const int64_t T = 10000;
    // Plans construits avec SR différents, même PolicyContext → PcmFlip identiques
    SceneTransitionPlan p1, p2;
    buildDirectPlan(from, to, 0, 1, T, 44100.0, p1);
    buildDirectPlan(from, to, 0, 1, T, 48000.0, p2);
    PolicyContext ctx{44100.0, 120.0, 4, 4, T}; // SR de référence = 44100

    TransitionPolicy::apply(p1, ctx);
    TransitionPolicy::apply(p2, ctx);

    REQUIRE(p1.numEvents == p2.numEvents);
    for (int i = 0; i < p1.numEvents; ++i) {
        if (p1.events[i].type == PlanEventType::PcmFlip)
            REQUIRE(p1.events[i].atSample == p2.events[i].atSample);
    }
}
