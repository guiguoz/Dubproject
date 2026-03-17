#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/Flanger.h"
#include "TestHelpers.h"

using dsp::Flanger;
using Catch::Matchers::WithinAbs;

static constexpr float kSampleRate = 44100.0f;
static constexpr int   kBlockSize  = 512;

TEST_CASE("Flanger -- mix=0 passes signal through unchanged", "[flanger]")
{
    Flanger f;
    f.prepare(kSampleRate, kBlockSize);
    f.setMix(0.0f);
    f.setDepth(0.0f);

    auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    const auto original = signal; // copy

    f.process(signal.data(), kBlockSize);

    // With mix=0 dry only — signal must be unchanged
    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE_THAT(signal[static_cast<std::size_t>(i)],
                     WithinAbs(original[static_cast<std::size_t>(i)], 1e-5f));
}

TEST_CASE("Flanger -- output always within [-1, 1]", "[flanger]")
{
    Flanger f;
    f.prepare(kSampleRate, kBlockSize);
    f.setFeedback(0.9f);
    f.setDepth(1.0f);
    f.setMix(0.8f);

    auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    f.process(signal.data(), kBlockSize);

    REQUIRE(test_helpers::allSamplesInRange(signal.data(), kBlockSize));
}

TEST_CASE("Flanger -- silence in, silence out", "[flanger]")
{
    Flanger f;
    f.prepare(kSampleRate, kBlockSize);

    auto signal = test_helpers::generateSilence(kBlockSize);
    f.process(signal.data(), kBlockSize);

    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE_THAT(signal[static_cast<std::size_t>(i)], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Flanger -- high feedback does not blow up over 4 seconds", "[flanger]")
{
    Flanger f;
    f.prepare(kSampleRate, kBlockSize);
    f.setFeedback(0.95f);
    f.setMix(0.5f);
    f.setDepth(0.5f);

    // Process 4 seconds
    const int totalSamples = static_cast<int>(kSampleRate) * 4;
    for (int offset = 0; offset < totalSamples; offset += kBlockSize)
    {
        auto block = test_helpers::generateSine(300.0f, kSampleRate, kBlockSize);
        f.process(block.data(), kBlockSize);
        REQUIRE(test_helpers::allSamplesInRange(block.data(), kBlockSize));
    }
}

TEST_CASE("Flanger -- reset zeroes state", "[flanger]")
{
    Flanger f;
    f.prepare(kSampleRate, kBlockSize);
    f.setFeedback(0.9f);

    auto signal = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    f.process(signal.data(), kBlockSize);

    f.reset();

    // After reset, silence should produce silence
    auto silence = test_helpers::generateSilence(kBlockSize);
    f.process(silence.data(), kBlockSize);
    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE_THAT(silence[static_cast<std::size_t>(i)], WithinAbs(0.0f, 1e-6f));
}
