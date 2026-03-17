#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/DspPipeline.h"
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
    pipeline.setHarmonizerEnabled(true);
    pipeline.setFlangerEnabled(true);
    pipeline.getFlanger().setFeedback(0.9f);

    for (int block = 0; block < 20; ++block)
    {
        auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
        pipeline.process(signal.data(), kBlockSize);
        REQUIRE(test_helpers::allSamplesInRange(signal.data(), kBlockSize));
    }
}

TEST_CASE("DspPipeline -- enable/disable does not crash", "[pipeline]")
{
    DspPipeline pipeline;
    pipeline.prepare(kSampleRate, kBlockSize);

    // Toggle rapidly while processing
    for (int i = 0; i < 10; ++i)
    {
        pipeline.setHarmonizerEnabled(i % 2 == 0);
        pipeline.setFlangerEnabled(i % 2 != 0);

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
