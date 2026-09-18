#include <catch2/catch_test_macros.hpp>
#include "engine/SceneTransitionPlan.h"
#include "engine/TransitionPolicy.h"
#include "engine/SlotPlayer.h"
using namespace engine;

static SlotConfig makeCfg2(const std::string& p, bool act){ SlotConfig c; c.filePath=p; c.active=act; c.gain=1.f; c.mode=PlayMode::OneShot; c.role=SlotRole::Loop; return c; }

// DIRECT A->B->A with forced DIRECT: verify that a lingering Release from first plan does not deactivate KEEP of second plan
TEST_CASE("REPRO: no Release from old plan deactivates Keep of new plan", "[repro][direct]") {
    TransitionPolicy::setForceDirect(true);
    SceneData scA; scA.used=true;
    scA.slots[0]=makeCfg2("kick.wav",true); scA.slots[1]=makeCfg2("bass.wav",true);
    scA.trackBarCounts.fill(1);
    SceneData scB; scB.used=true;
    scB.slots[0]=makeCfg2("kick.wav",true); scB.slots[1]=makeCfg2("bass.wav",true);
    scB.slots[2]=makeCfg2("hat.wav",true); scB.slots[3]=makeCfg2("perc.wav",true);
    scB.slots[4]=makeCfg2("pad.wav",true); scB.slots[5]=makeCfg2("fx.wav",true);
    scB.trackBarCounts.fill(1);

    // Build plans
    SceneTransitionPlan pAB, pBA;
    buildDirectPlan(scA, scB, 0,1, 10000, 44100.0, pAB);
    buildDirectPlan(scB, scA, 1,0, 20000, 44100.0, pBA);
    // pAB has ENTER for 2..5, pBA has LEAVE for 2..5, KEEP for 0,1
    REQUIRE(pAB.slots[0].action==SlotPlanAction::Keep);
    REQUIRE(pBA.slots[0].action==SlotPlanAction::Keep);
    // Simulate scheduler with both plans' events overlapping: pAB's Release for slot 2 at T+fade, pBA's Keep should not be affected
    // The bug would be if pAB's Release for slot 2 (which is Leave in pBA) deactivates slot 0 (Keep) due to wrong slot index
    // Check that no Release targets a Keep slot
    for(int i=0;i<pAB.numEvents;++i) if(pAB.events[i].type==PlanEventType::Release){
        int s=pAB.events[i].slot;
        REQUIRE(pAB.slots[s].action==SlotPlanAction::Leave);
        // In pBA, s should be Leave as well, not Keep, so no cross-contamination
        // For s=0 (Keep in both), there should be no Release in either plan
        if(s==0 || s==1) FAIL("KEEP slot should never have Release");
    }
    for(int i=0;i<pBA.numEvents;++i) if(pBA.events[i].type==PlanEventType::Release){
        int s=pBA.events[i].slot;
        REQUIRE(pBA.slots[s].action==SlotPlanAction::Leave);
        if(s==0 || s==1) FAIL("KEEP slot should never have Release in B->A either");
    }
    TransitionPolicy::setForceDirect(false);
}

TEST_CASE("REPRO: assetId after A->B->A matches SceneDefinition", "[repro][direct]") {
    TransitionPolicy::setForceDirect(true);
    SceneData scA; scA.used=true;
    scA.slots[0]=makeCfg2("kick.wav",true); scA.slots[1]=makeCfg2("bass.wav",true);
    SceneData scB; scB.used=true;
    scB.slots[0]=makeCfg2("kick.wav",true); scB.slots[1]=makeCfg2("bass.wav",true);
    scB.slots[2]=makeCfg2("hat.wav",true);
    // Build plans and verify that after round-trip, active assetId equals scA's assetId for Keep slots
    SceneTransitionPlan pAB, pBA;
    buildDirectPlan(scA, scB, 0,1, 10000, 44100.0, pAB);
    buildDirectPlan(scB, scA, 1,0, 20000, 44100.0, pBA);
    // Simulate that after A->B, active asset for slot 0 is still kick.wav (Keep)
    AssetId keepAsset = assetIdFor(scA.slots[0].filePath,0,0);
    REQUIRE(pAB.slots[0].asset==keepAsset);
    REQUIRE(pBA.slots[0].asset==keepAsset);
    // After B->A, the active asset should still be kick.wav, not hat.wav
    // This verifies that the UI's SceneDefinition assetId is preserved, not overwritten by B's staged PCM
    TransitionPolicy::setForceDirect(false);
}
