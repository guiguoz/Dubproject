#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/EffectChainOptimizer.h"
#include "dsp/EffectChain.h"
#include "dsp/FlangerEffect.h"
#include "dsp/HarmonizerEffect.h"
#include "dsp/DelayEffect.h"
#include "dsp/OctaverEffect.h"
#include "dsp/AutoPitchCorrect.h"
#include "dsp/SlicerEffect.h"
#include "dsp/EnvelopeFilterEffect.h"

using dsp::EffectChain;
using dsp::EffectChainOptimizer;
using Catch::Matchers::WithinAbs;

static constexpr float kSampleRate = 44100.0f;
static constexpr int   kBlockSize  = 256;

static void prepareChain(EffectChain& chain)
{
    chain.prepare(kSampleRate, kBlockSize);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("EffectChainOptimizer -- optimize empty chain does not crash", "[optimizer]")
{
    EffectChain chain;
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    EffectChainOptimizer::Context ctx;

    REQUIRE_NOTHROW(optimizer.optimize(chain, ctx));
    REQUIRE(optimizer.isActive());
}

TEST_CASE("EffectChainOptimizer -- restore returns original params", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::FlangerEffect>());
    prepareChain(chain);

    // Set known param before optimization
    chain.getEffect(0)->setParam(0, 1.5f);  // rate = 1.5 Hz
    chain.getEffect(0)->setParam(1, 0.9f);  // depth = 0.9

    EffectChainOptimizer optimizer;
    optimizer.optimize(chain, {});

    // AI should have changed the params
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), !WithinAbs(1.5f, 0.01f));

    optimizer.restore(chain);

    // Params must be exactly restored
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(1.5f, 0.001f));
    REQUIRE_THAT(chain.getEffect(0)->getParam(1), WithinAbs(0.9f, 0.001f));
    REQUIRE_FALSE(optimizer.isActive());
}

TEST_CASE("EffectChainOptimizer -- delay BPM sync 120 BPM -> dotted 8th ~375ms", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::DelayEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    EffectChainOptimizer::Context ctx;
    ctx.bpm = 120.0f;
    optimizer.optimize(chain, ctx);

    // 60000/120 * 0.75 = 375 ms (dotted 8th at 120 BPM)
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(375.0f, 1.0f));
}

TEST_CASE("EffectChainOptimizer -- delay BPM sync 150 BPM -> 8th note ~200ms", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::DelayEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    EffectChainOptimizer::Context ctx;
    ctx.bpm = 150.0f;
    optimizer.optimize(chain, ctx);

    // 60000/150 * 0.5 = 200 ms (8th note at 150 BPM)
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(200.0f, 1.0f));
}

TEST_CASE("EffectChainOptimizer -- delay BPM sync 70 BPM -> quarter note ~857ms", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::DelayEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    EffectChainOptimizer::Context ctx;
    ctx.bpm = 70.0f;
    optimizer.optimize(chain, ctx);

    // 60000/70 * 1.0 ≈ 857 ms (quarter note at 70 BPM)
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(857.1f, 2.0f));
}

TEST_CASE("EffectChainOptimizer -- flanger depth reduced when harmonizer present", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::FlangerEffect>());
    chain.addEffect(std::make_unique<dsp::HarmonizerEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    optimizer.optimize(chain, {});

    // Flanger depth should be 0.25 (reduced) when harmonizer is present
    REQUIRE_THAT(chain.getEffect(0)->getParam(1), WithinAbs(0.25f, 0.01f));
}

TEST_CASE("EffectChainOptimizer -- flanger depth normal when harmonizer absent", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::FlangerEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    optimizer.optimize(chain, {});

    // Flanger depth should be 0.40 (full) when no harmonizer
    REQUIRE_THAT(chain.getEffect(0)->getParam(1), WithinAbs(0.40f, 0.01f));
}

TEST_CASE("EffectChainOptimizer -- harmonizer mix reduced when octaver present", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::HarmonizerEffect>());
    chain.addEffect(std::make_unique<dsp::OctaverEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    optimizer.optimize(chain, {});

    // Harmonizer mix should be 0.35 (reduced for gain staging) when octaver active
    REQUIRE_THAT(chain.getEffect(0)->getParam(2), WithinAbs(0.35f, 0.01f));
}

TEST_CASE("EffectChainOptimizer -- AutoPitch strength halved when harmonizer active", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::HarmonizerEffect>());
    chain.addEffect(std::make_unique<dsp::AutoPitchCorrect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    optimizer.optimize(chain, {});

    // AutoPitch strength[0] = 0.50 when harmonizer is active (avoids robotic sound)
    REQUIRE_THAT(chain.getEffect(1)->getParam(0), WithinAbs(0.50f, 0.01f));
}

TEST_CASE("EffectChainOptimizer -- AutoPitch strength full when harmonizer absent", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::AutoPitchCorrect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    optimizer.optimize(chain, {});

    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(1.00f, 0.01f));
}

TEST_CASE("EffectChainOptimizer -- slicer rate BPM-synced at 120 BPM", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::SlicerEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    EffectChainOptimizer::Context ctx;
    ctx.bpm = 120.0f;
    optimizer.optimize(chain, ctx);

    // 120/60 * 4 = 8 Hz (16th notes at 120 BPM)
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(8.0f, 0.1f));
}

TEST_CASE("EffectChainOptimizer -- env filter sensitivity scales with RMS level", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::EnvelopeFilterEffect>());
    prepareChain(chain);

    EffectChainOptimizer optimizer;
    EffectChainOptimizer::Context ctx;
    ctx.rmsLevel = 0.5f;  // moderate playing level
    optimizer.optimize(chain, ctx);

    // Sensitivity = clamp(0.5 * 8.0, 0.8, 7.0) = 4.0
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(4.0f, 0.1f));
}

TEST_CASE("EffectChainOptimizer -- restore is no-op when not active", "[optimizer]")
{
    EffectChain chain;
    chain.addEffect(std::make_unique<dsp::FlangerEffect>());
    prepareChain(chain);
    chain.getEffect(0)->setParam(0, 1.23f);

    EffectChainOptimizer optimizer;
    REQUIRE_NOTHROW(optimizer.restore(chain));

    // Param must be unchanged since optimize() was never called
    REQUIRE_THAT(chain.getEffect(0)->getParam(0), WithinAbs(1.23f, 0.001f));
}
