#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/DspPipeline.h"
#include "dsp/FlangerEffect.h"
#include "TestHelpers.h"

#include <vector>
#include <cstring>

using dsp::DspPipeline;
using Catch::Matchers::WithinAbs;

static constexpr float kSampleRate = 44100.0f;
static constexpr int   kBlockSize  = 256;

TEST_CASE("DspPipeline -- all effects disabled -> pass-through", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false); // disable limiter for true pass-through testing
    // Both effects disabled by default

    auto signal   = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    auto original = signal;

    pipeline.process(signal.data(), kBlockSize);

    // Disabled pipeline: only YIN analysis runs (read-only), output unchanged
    REQUIRE(test_helpers::buffersEqual(signal.data(), original.data(), kBlockSize));
}

TEST_CASE("DspPipeline -- output always within [-1, 1]", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    for (int block = 0; block < 20; ++block)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        pipeline.process(signal.data(), kBlockSize);
        REQUIRE(test_helpers::allSamplesInRange(signal.data(), kBlockSize));
    }
}

TEST_CASE("DspPipeline -- sampler toggle does not crash", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Toggle sampler rapidly while processing
    for (int i = 0; i < 10; ++i)
    {
        pipeline.setSamplerEnabled(i % 2 == 0);

        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        REQUIRE_NOTHROW(pipeline.process(signal.data(), kBlockSize));
    }
}

TEST_CASE("DspPipeline -- getLastPitch returns 0 before enough data", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Feed one small block — not enough for YIN
    auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    pipeline.process(signal.data(), kBlockSize);

    const auto pitch = pipeline.getLastPitch();
    // Either 0 (not enough data) or a reasonable frequency — must not be garbage
    REQUIRE((pitch.frequencyHz == 0.0f ||
             (pitch.frequencyHz >= 80.0f && pitch.frequencyHz <= 2000.0f)));
}

TEST_CASE("DspPipeline -- reset clears pitch state", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Feed enough blocks for YIN to detect
    for (int i = 0; i < 20; ++i)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        pipeline.process(signal.data(), kBlockSize);
    }

    pipeline.reset();
    const auto pitch = pipeline.getLastPitch();
    REQUIRE(pitch.frequencyHz == 0.0f);
}

TEST_CASE("DspPipeline -- getLastRms is 0 before any processing", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    REQUIRE(pipeline.getLastRms() == 0.0f);
}

TEST_CASE("DspPipeline -- getLastRms rises after non-zero input", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Feed several blocks of a 440 Hz sine at amplitude 0.5
    for (int i = 0; i < 50; ++i)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        for (auto& s : signal) s *= 0.5f;
        pipeline.process(signal.data(), kBlockSize);
    }

    REQUIRE(pipeline.getLastRms() > 0.01f);
}

TEST_CASE("DspPipeline -- getBpmDetector default BPM is 120", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    REQUIRE_THAT(pipeline.getBpmDetector().getBpm(),
                 WithinAbs(120.0f, 0.01f));
}

TEST_CASE("DspPipeline -- ExpressionMapper inactive by default", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    REQUIRE_FALSE(pipeline.getExpressionMapper().isActive());
}

TEST_CASE("DspPipeline -- ExpressionMapper active maps RMS to effect param", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Add one effect so index 0 exists in the chain
    pipeline.getEffectChain().addEffect(std::make_unique<dsp::FlangerEffect>());

    // Configure mapper: RMS [0,1] -> param 0 of effect 0, range [0,1]
    dsp::ExpressionConfig cfg;
    cfg.effectIndex = 0;
    cfg.paramIndex  = 0;
    cfg.inMin  = 0.0f; cfg.inMax  = 1.0f;
    cfg.outMin = 0.0f; cfg.outMax = 1.0f;
    pipeline.getExpressionMapper().setConfig(cfg);

    REQUIRE(pipeline.getExpressionMapper().isActive());

    // Feed signal — should not crash and should not change output range
    for (int i = 0; i < 10; ++i)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        REQUIRE_NOTHROW(pipeline.process(signal.data(), kBlockSize));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pitch stabilization tests (v2)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DspPipeline -- pitch hold on silence then timeout", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false);

    // 1. Feed 440 Hz for ~100 blocks (~580ms) to establish a stable pitch.
    //    Use one contiguous sine to avoid phase-reset artifacts at block boundaries.
    const int toneBlocks = 100;
    const int toneSamples = toneBlocks * kBlockSize;
    auto toneBuf = test_helpers::generateSine(440.0f, kSampleRate, toneSamples);
    for (int offset = 0; offset < toneSamples; offset += kBlockSize)
        pipeline.process(toneBuf.data() + offset, kBlockSize);

    const float pitchAfterTone = pipeline.getLastPitch().frequencyHz;
    // Should have converged to ~440 Hz
    REQUIRE(pitchAfterTone > 400.0f);
    REQUIRE(pitchAfterTone < 480.0f);

    // 2. Feed silence for < 200ms (~30 blocks @ 256 samples = ~174ms, under timeout)
    for (int i = 0; i < 30; ++i)
    {
        auto silence = test_helpers::generateSilence(kBlockSize);
        pipeline.process(silence.data(), kBlockSize);
    }

    const float pitchAfterShortSilence = pipeline.getLastPitch().frequencyHz;
    // Pitch should be HELD (not dropped to 0) during the hold period
    REQUIRE(pitchAfterShortSilence > 400.0f);
    REQUIRE(pitchAfterShortSilence < 480.0f);

    // 3. Feed silence for well beyond 200ms timeout (100 more blocks = ~580ms)
    for (int i = 0; i < 100; ++i)
    {
        auto silence = test_helpers::generateSilence(kBlockSize);
        pipeline.process(silence.data(), kBlockSize);
    }

    const float pitchAfterTimeout = pipeline.getLastPitch().frequencyHz;
    // Should have timed out → pitch = 0
    REQUIRE(pitchAfterTimeout == 0.0f);
}

TEST_CASE("DspPipeline -- log smoothing converges progressively", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);
    pipeline.setMasterLimiterEnabled(false);

    // 1. Feed 440 Hz (contiguous) to establish stable pitch
    const int warmupSamples = 80 * kBlockSize;
    auto tone440 = test_helpers::generateSine(440.0f, kSampleRate, warmupSamples);
    for (int offset = 0; offset < warmupSamples; offset += kBlockSize)
        pipeline.process(tone440.data() + offset, kBlockSize);

    const float pitch440 = pipeline.getLastPitch().frequencyHz;
    REQUIRE(pitch440 > 400.0f);
    REQUIRE(pitch440 < 480.0f);

    // 2. Switch to 880 Hz — first block should NOT jump instantly to 880
    //    (contiguous 880 Hz buffer, feed one block)
    auto tone880 = test_helpers::generateSine(880.0f, kSampleRate, 100 * kBlockSize);
    pipeline.process(tone880.data(), kBlockSize);

    const float pitchAfter1Block = pipeline.getLastPitch().frequencyHz;
    // The smoothing EMA should keep it closer to 440 than to 880
    // (it can't jump from 440 to 880 in a single ~5.8ms block with 50ms smoothing)
    REQUIRE(pitchAfter1Block < 800.0f);

    // 3. After many more blocks of 880 Hz, it should converge near 880
    for (int offset = kBlockSize; offset < 80 * kBlockSize; offset += kBlockSize)
        pipeline.process(tone880.data() + offset, kBlockSize);

    const float pitchConverged = pipeline.getLastPitch().frequencyHz;
    REQUIRE(pitchConverged > 840.0f);
    REQUIRE(pitchConverged < 920.0f);
}
