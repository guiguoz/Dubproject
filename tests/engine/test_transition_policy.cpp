#include <catch2/catch_test_macros.hpp>
#include "engine/TransitionPolicy.h"
#include "engine/SceneTransitionPlan.h"
using namespace engine;

static SceneData makeSceneWithSlots(int n, const std::vector<std::string>& paths) {
    SceneData sc; sc.used=true;
    for(int i=0;i<kMaxSlots;++i){ sc.slots[i].active=false; sc.steps[i].fill(false); sc.trackBarCounts[i]=1; }
    for(int i=0;i<n && i<(int)paths.size(); ++i){ sc.slots[i].filePath=paths[i]; sc.slots[i].active=true; sc.slots[i].gain=1.f; sc.slots[i].role=SlotRole::Loop; }
    return sc;
}

TEST_CASE("Policy: DIRECT identical to 125d27b", "[policy]") {
    SceneData from=makeSceneWithSlots(2,{"a.wav","b.wav"});
    SceneData to=makeSceneWithSlots(2,{"a.wav","b.wav"});
    to.slots[0].gain=0.5f; // Morph
    SceneTransitionPlan plan; buildDirectPlan(from,to,0,1,1000,44100.0,plan);
    SceneTransitionPlan copy=plan;
    PolicyContext ctx{44100.0,120.0,4,4,1000};
    auto p = TransitionPolicy::choose(plan);
    REQUIRE(p==PolicyType::Direct);
    TransitionPolicy::apply(plan, ctx);
    REQUIRE(plan.numEvents==copy.numEvents);
    for(int i=0;i<plan.numEvents;++i) REQUIRE(plan.events[i].atSample==copy.events[i].atSample);
}

TEST_CASE("Policy: KEEP sacred no events", "[policy]") {
    SceneData from=makeSceneWithSlots(2,{"a.wav","b.wav"});
    SceneData to=from;
    SceneTransitionPlan plan; buildDirectPlan(from,to,0,1,1000,44100.0,plan);
    for(int s=0;s<kMaxSlots;++s) REQUIRE(plan.slots[s].action==SlotPlanAction::Keep);
    for(int i=0;i<plan.numEvents;++i) FAIL("KEEP should have no events");
}

TEST_CASE("Policy: BUILD 2->6 distributes ENTER", "[policy][build]") {
    SceneData from=makeSceneWithSlots(2,{"a.wav","b.wav"});
    SceneData to=makeSceneWithSlots(6,{"a.wav","b.wav","c.wav","d.wav","e.wav","f.wav"});
    SceneTransitionPlan plan; buildDirectPlan(from,to,0,1,10000,44100.0,plan);
    PolicyContext ctx{44100.0,120.0,4,4,10000};
    REQUIRE(TransitionPolicy::choose(plan)==PolicyType::Build);
    TransitionPolicy::apply(plan, ctx);
    // ENTER slots 2..5 should have PcmFlip at T + n*beat
    int64_t beat = TransitionPolicy::samplesPerBeat(ctx);
    for(int s=2;s<6;++s){
        bool found=false;
        for(int i=0;i<plan.numEvents;++i) if(plan.events[i].slot==s && plan.events[i].type==PlanEventType::PcmFlip){
            int64_t expected = 10000 + (s-2)*beat;
            REQUIRE(plan.events[i].atSample==expected);
            found=true;
        }
        REQUIRE(found);
    }
    // No KEEP events
    for(int i=0;i<plan.numEvents;++i) REQUIRE(plan.slots[plan.events[i].slot].action!=SlotPlanAction::Keep);
}

TEST_CASE("Policy: BREAKDOWN 6->2 distributes LEAVE", "[policy][breakdown]") {
    SceneData from=makeSceneWithSlots(6,{"a.wav","b.wav","c.wav","d.wav","e.wav","f.wav"});
    SceneData to=makeSceneWithSlots(2,{"a.wav","b.wav"});
    SceneTransitionPlan plan; buildDirectPlan(from,to,0,1,20000,44100.0,plan);
    PolicyContext ctx{44100.0,120.0,4,4,20000};
    REQUIRE(TransitionPolicy::choose(plan)==PolicyType::Breakdown);
    TransitionPolicy::apply(plan, ctx);
    int64_t measure = TransitionPolicy::samplesPerBeat(ctx)*4;
    // First LEAVE at T-measure, last at T
    int64_t minAt = INT64_MAX, maxAt = INT64_MIN;
    for(int i=0;i<plan.numEvents;++i) if(plan.events[i].type==PlanEventType::GainRamp && plan.slots[plan.events[i].slot].action==SlotPlanAction::Leave){
        minAt = std::min(minAt, plan.events[i].atSample);
        maxAt = std::max(maxAt, plan.events[i].atSample);
    }
    REQUIRE(minAt==20000 - measure);
    REQUIRE(maxAt==20000);
}

TEST_CASE("Policy: block-size independence", "[policy]") {
    SceneData from=makeSceneWithSlots(2,{"a.wav","b.wav"});
    SceneData to=makeSceneWithSlots(6,{"a.wav","b.wav","c.wav","d.wav","e.wav","f.wav"});
    SceneTransitionPlan p1,p2;
    buildDirectPlan(from,to,0,1,10000,44100.0,p1);
    buildDirectPlan(from,to,0,1,10000,48000.0,p2); // different SR, but same boundary
    PolicyContext ctx1{44100.0,120.0,4,4,10000}, ctx2{44100.0,120.0,4,4,10000};
    TransitionPolicy::apply(p1, ctx1);
    TransitionPolicy::apply(p2, ctx2);
    REQUIRE(p1.numEvents==p2.numEvents);
    for(int i=0;i<p1.numEvents;++i) REQUIRE(p1.events[i].atSample==p2.events[i].atSample);
}

TEST_CASE("Policy: no double trigger ENTER", "[policy]") {
    Sequencer seq;
    TrackPattern pat; pat.numSteps=16; for(int i=0;i<16;++i) pat.steps[i]=true;
    *seq.patterns().writeBuffer(0)=pat; seq.patterns().publish();
    int64_t activation = 10000 + 22050;
    seq.setSlotActiveAt(0, activation);
    TransportState ts{}; ts.sampleRate=44100; ts.bpm=120; ts.samplesPerBeat=22050; ts.samplesPerStep=5512.5; ts.playing=true;
    EventScheduler sched;
    seq.generateEvents(ts, 10000, 22050, 0.f, sched);
    int cnt=0; sched.processBlock(10000,22050,[&](int32_t, const EngineEvent& ev){ if(ev.slot==0) cnt++; });
    REQUIRE(cnt==0);
    EventScheduler sched2;
    seq.generateEvents(ts, 10000+22050, 22050, 0.f, sched2);
    int cnt2=0; sched2.processBlock(10000+22050,22050,[&](int32_t, const EngineEvent& ev){ if(ev.slot==0) cnt2++; });
    REQUIRE(cnt2>0);
}

TEST_CASE("Policy: A->B->C no stale commit", "[policy]") {
    SceneData a=makeSceneWithSlots(2,{"a.wav","b.wav"});
    SceneData b=makeSceneWithSlots(4,{"a.wav","b.wav","c.wav","d.wav"});
    SceneData c=makeSceneWithSlots(6,{"a.wav","b.wav","c.wav","d.wav","e.wav","f.wav"});
    SceneTransitionPlan p1,p2;
    buildDirectPlan(a,b,0,1,10000,44100.0,p1);
    buildDirectPlan(b,c,1,2,20000,44100.0,p2);
    PolicyContext ctx{44100.0,120.0,4,4,10000};
    TransitionPolicy::apply(p1, ctx);
    PolicyContext ctx2{44100.0,120.0,4,4,20000};
    TransitionPolicy::apply(p2, ctx2);
    // If A->B is still armed (boundary 10000) and we try to prepare B->C at 20000, it should be allowed only after first completes
    // Here we just verify that p1 and p2 have distinct boundaries and no overlap
    REQUIRE(p1.boundary==10000);
    REQUIRE(p2.boundary==20000);
    // Ensure no event from p1 has atSample >= p2.boundary that would be stale
    for(int i=0;i<p1.numEvents;++i) REQUIRE(p1.events[i].atSample < p2.boundary);
}
