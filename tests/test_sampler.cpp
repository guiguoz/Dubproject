#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/Sampler.h"
#include "TestHelpers.h"

using dsp::Sampler;
using Catch::Matchers::WithinAbs;

static constexpr double kSR        = 44100.0;
static constexpr int    kBlockSize = 512;

// Helper: build a mono sine buffer
static std::vector<float> makeSine(int n, float freq = 440.0f, float sr = 44100.0f)
{
    return test_helpers::generateSine(freq, sr, n);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Sampler -- unloaded slot produces silence", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    std::vector<float> buf(kBlockSize, 0.0f);
    s.trigger(0);
    s.process(buf.data(), kBlockSize);

    REQUIRE(test_helpers::allSamplesInRange(buf.data(), kBlockSize));
    const float rms = test_helpers::computeRms(buf.data(), kBlockSize);
    REQUIRE_THAT(rms, WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Sampler -- load and trigger produces audio", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    const auto sine = makeSine(kBlockSize);
    s.loadSample(0, sine.data(), kBlockSize, kSR);
    s.trigger(0);

    std::vector<float> buf(kBlockSize, 0.0f);
    s.process(buf.data(), kBlockSize);

    const float rms = test_helpers::computeRms(buf.data(), kBlockSize);
    REQUIRE(rms > 0.01f);
}

TEST_CASE("Sampler -- one-shot stops at end of sample", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    // Short sample (128 samples), one-shot (default)
    constexpr int kShort = 128;
    const auto    sine   = makeSine(kShort);
    s.loadSample(0, sine.data(), kShort, kSR);
    s.setSlotOneShot(0, true);
    s.trigger(0);

    // First block: sample plays fully and stops mid-block
    std::vector<float> buf(kBlockSize, 0.0f);
    s.process(buf.data(), kBlockSize);

    // After end of sample, playback should have stopped
    REQUIRE_FALSE(s.isPlaying(0));

    // Second block: all silence
    std::fill(buf.begin(), buf.end(), 0.0f);
    s.process(buf.data(), kBlockSize);
    const float rms = test_helpers::computeRms(buf.data(), kBlockSize);
    REQUIRE_THAT(rms, WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Sampler -- loop mode wraps around", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    constexpr int kShort = 64;
    const auto    sine   = makeSine(kShort);
    s.loadSample(0, sine.data(), kShort, kSR);
    s.setSlotLoop(0, true);
    s.setSlotOneShot(0, false);
    s.trigger(0);

    // Process 4x the sample length — should still be playing
    std::vector<float> buf(kBlockSize, 0.0f);
    s.process(buf.data(), kBlockSize); // 512 > 64, wraps 8 times

    REQUIRE(s.isPlaying(0));

    const float rms = test_helpers::computeRms(buf.data(), kBlockSize);
    REQUIRE(rms > 0.01f);
}

TEST_CASE("Sampler -- gain applies correctly", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    const auto sine = makeSine(kBlockSize);
    s.loadSample(0, sine.data(), kBlockSize, kSR);
    s.setSlotGain(0, 0.5f);
    s.trigger(0);

    std::vector<float> buf(kBlockSize, 0.0f);
    s.process(buf.data(), kBlockSize);

    // Sampler with gain 0.5 should produce ~half the RMS of the original
    const float rmsOut  = test_helpers::computeRms(buf.data(), kBlockSize);
    const float rmsFull = test_helpers::computeRms(sine.data(), kBlockSize);
    REQUIRE_THAT(rmsOut, WithinAbs(0.5f * rmsFull, 0.01f));
}

TEST_CASE("Sampler -- stop halts playback", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    const auto sine = makeSine(kBlockSize * 4);
    s.loadSample(0, sine.data(), kBlockSize * 4, kSR);
    s.setSlotLoop(0, true);
    s.trigger(0);

    // First block: playing
    std::vector<float> buf(kBlockSize, 0.0f);
    s.process(buf.data(), kBlockSize);
    REQUIRE(s.isPlaying(0));

    // Stop, then process again
    s.stop(0);
    std::fill(buf.begin(), buf.end(), 0.0f);
    s.process(buf.data(), kBlockSize); // Fade out occurs over 256 samples here

    REQUIRE_FALSE(s.isPlaying(0));
    
    // Now it should be silent
    std::fill(buf.begin(), buf.end(), 0.0f);
    s.process(buf.data(), kBlockSize);
    const float rms = test_helpers::computeRms(buf.data(), kBlockSize);
    REQUIRE_THAT(rms, WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Sampler -- multiple slots mix additively", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    const auto sine0 = makeSine(kBlockSize, 440.0f);
    const auto sine1 = makeSine(kBlockSize, 880.0f);
    s.loadSample(0, sine0.data(), kBlockSize, kSR);
    s.loadSample(1, sine1.data(), kBlockSize, kSR);
    s.trigger(0);
    s.trigger(1);

    std::vector<float> buf(kBlockSize, 0.0f);
    s.process(buf.data(), kBlockSize);

    // Both voices active: RMS should be higher than a single voice
    Sampler s2;
    s2.prepare(kSR, kBlockSize);
    s2.loadSample(0, sine0.data(), kBlockSize, kSR);
    s2.trigger(0);
    std::vector<float> buf2(kBlockSize, 0.0f);
    s2.process(buf2.data(), kBlockSize);

    const float rmsTwo = test_helpers::computeRms(buf.data(),  kBlockSize);
    const float rmsOne = test_helpers::computeRms(buf2.data(), kBlockSize);
    REQUIRE(rmsTwo > rmsOne * 1.1f);
}

TEST_CASE("Sampler -- out-of-range slot does not crash", "[sampler]")
{
    Sampler s;
    s.prepare(kSR, kBlockSize);

    REQUIRE_NOTHROW(s.trigger(-1));
    REQUIRE_NOTHROW(s.trigger(Sampler::kMaxSlots));
    REQUIRE_NOTHROW(s.stop(-1));
    REQUIRE_NOTHROW(s.stop(Sampler::kMaxSlots));
    REQUIRE_NOTHROW(s.loadSample(-1, nullptr, 0, kSR));
    REQUIRE_NOTHROW(s.loadSample(Sampler::kMaxSlots, nullptr, 0, kSR));
}
