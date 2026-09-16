#include <catch2/catch_test_macros.hpp>
#include "engine/SceneTransitionPlan.h"
using namespace engine;

static SlotConfig makeCfg(const std::string& path, int trimS, int trimE, bool active, PlayMode mode, float gain, float semi, bool muted, SlotRole role) {
    SlotConfig c;
    c.filePath = path;
    c.trimStart = trimS;
    c.trimEnd = trimE;
    c.active = active;
    c.mode = mode;
    c.gain = gain;
    c.semitones = semi;
    c.muted = muted;
    c.role = role;
    return c;
}

TEST_CASE("AssetId: empty -> 0, same path same trims equal, trims differ", "[plan][asset]") {
    REQUIRE(assetIdFor("", 0, 0) == 0);
    REQUIRE(assetIdFor("kick.wav", 0, 0) != 0);
    REQUIRE(assetIdFor("kick.wav", 0, 0) == assetIdFor("kick.wav", 0, 0));
    REQUIRE(assetIdFor("kick.wav", 0, 100) != assetIdFor("kick.wav", 0, 0));
    REQUIRE(assetIdFor("KICK.WAV", 0, 0) == assetIdFor("kick.wav", 0, 0)); // canon lower
    REQUIRE(assetIdFor("a\\b\\kick.wav", 0, 0) == assetIdFor("a/b/kick.wav", 0, 0));
}

TEST_CASE("classify: Keep/Morph/Leave/Enter", "[plan][diff]") {
    SlotConfig a = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    SlotConfig b = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    REQUIRE(classifySlotForPlan(a,b, assetIdForSlot(a), assetIdForSlot(b)) == SlotPlanAction::Keep);
    b.gain = 0.5f;
    REQUIRE(classifySlotForPlan(a,b, assetIdForSlot(a), assetIdForSlot(b)) == SlotPlanAction::Morph);
    b = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    b.semitones = 2.f;
    REQUIRE(classifySlotForPlan(a,b, assetIdForSlot(a), assetIdForSlot(b)) == SlotPlanAction::Morph);
    // Leave
    SlotConfig empty; empty.active = false;
    REQUIRE(classifySlotForPlan(a, empty, assetIdForSlot(a), assetIdForSlot(empty)) == SlotPlanAction::Leave);
    REQUIRE(classifySlotForPlan(empty, a, assetIdForSlot(empty), assetIdForSlot(a)) == SlotPlanAction::Enter);
    // Same slot different asset -> Enter (Leave+Enter)
    SlotConfig c = makeCfg("b.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    REQUIRE(classifySlotForPlan(a,c, assetIdForSlot(a), assetIdForSlot(c)) == SlotPlanAction::Enter);
    // Pattern not participating: same asset same params but active diff steps still Keep
    SlotConfig d = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    REQUIRE(classifySlotForPlan(a,d, assetIdForSlot(a), assetIdForSlot(d)) == SlotPlanAction::Keep);
}

TEST_CASE("buildDirectPlan: KEEP no events, MORPH GainRamp, LEAVE/ENTER", "[plan][build]") {
    SceneData from, to;
    from.used = to.used = true;
    // slot0 Keep
    from.slots[0] = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    to.slots[0] = from.slots[0];
    // slot1 Morph gain
    from.slots[1] = makeCfg("b.wav", 0, 0, true, PlayMode::Free, 1.f, 0.f, false, SlotRole::Bass);
    to.slots[1] = makeCfg("b.wav", 0, 0, true, PlayMode::Free, 0.5f, 0.f, false, SlotRole::Bass);
    // slot2 Leave
    from.slots[2] = makeCfg("c.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Snare);
    to.slots[2].active = false;
    // slot3 Enter
    from.slots[3].active = false;
    to.slots[3] = makeCfg("d.wav", 0, 0, true, PlayMode::OneShot, 0.8f, 0.f, false, SlotRole::Pad);
    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, 1000, 44100.0, plan);
    REQUIRE(plan.valid);
    REQUIRE(plan.slots[0].action == SlotPlanAction::Keep);
    REQUIRE(plan.slots[1].action == SlotPlanAction::Morph);
    REQUIRE(plan.slots[2].action == SlotPlanAction::Leave);
    REQUIRE(plan.slots[3].action == SlotPlanAction::Enter);
    // Keep -> no GainRamp for slot0
    for (int i=0;i<plan.numEvents;++i) REQUIRE(plan.events[i].slot != 0);
    // Morph -> GainRamp present
    bool hasMorphRamp=false; for(int i=0;i<plan.numEvents;++i) if(plan.events[i].slot==1 && plan.events[i].type==PlanEventType::GainRamp) hasMorphRamp=true;
    REQUIRE(hasMorphRamp);
    // Leave -> GainRamp + Release
    bool hasLeaveRamp=false, hasLeaveRel=false; for(int i=0;i<plan.numEvents;++i) if(plan.events[i].slot==2){ if(plan.events[i].type==PlanEventType::GainRamp) hasLeaveRamp=true; if(plan.events[i].type==PlanEventType::Release) hasLeaveRel=true; }
    REQUIRE(hasLeaveRamp); REQUIRE(hasLeaveRel);
    // Enter -> PcmFlip + GainRamp, no trigger event (Sequencer authority)
    bool hasEnterFlip=false, hasEnterRamp=false; for(int i=0;i<plan.numEvents;++i) if(plan.events[i].slot==3){ if(plan.events[i].type==PlanEventType::PcmFlip) hasEnterFlip=true; if(plan.events[i].type==PlanEventType::GainRamp) hasEnterRamp=true; if(plan.events[i].type==PlanEventType::Release) REQUIRE(false); }
    REQUIRE(hasEnterFlip); REQUIRE(hasEnterRamp);
    // Ensure no string comparison in plan events
    // Fade duration derived from sampleRate
    for(int i=0;i<plan.numEvents;++i) if(plan.events[i].type==PlanEventType::GainRamp) REQUIRE(plan.events[i].a == 441.f); // 10ms @44100
    // Same build at 48k -> 480
    buildDirectPlan(from, to, 0, 1, 1000, 48000.0, plan);
    for(int i=0;i<plan.numEvents;++i) if(plan.events[i].type==PlanEventType::GainRamp) REQUIRE(plan.events[i].a == 480.f);
}

TEST_CASE("buildDirectPlan: same asset different slots -> Leave+Enter per slot, not migration", "[plan][move]") {
    SceneData from, to;
    from.used = to.used = true;
    from.slots[0] = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    from.slots[1].active = false;
    to.slots[0].active = false;
    to.slots[1] = makeCfg("a.wav", 0, 0, true, PlayMode::OneShot, 1.f, 0.f, false, SlotRole::Kick);
    SceneTransitionPlan plan;
    buildDirectPlan(from, to, 0, 1, 1000, 44100.0, plan);
    REQUIRE(plan.slots[0].action == SlotPlanAction::Leave);
    REQUIRE(plan.slots[1].action == SlotPlanAction::Enter);
}
