#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/Harmonizer.h"
#include "TestHelpers.h"

using dsp::Harmonizer;
using Catch::Matchers::WithinAbs;

static constexpr float kSampleRate = 44100.0f;
static constexpr int   kBlockSize  = 512;
static constexpr int   kLongBlock  = 4096; // enough for OLA to stabilise

TEST_CASE("Harmonizer -- mix=0 output equals dry input", "[harmonizer]")
{
    Harmonizer h;
    h.prepare(kSampleRate, kBlockSize);
    h.setMix(0.0f);

    const auto input = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    std::vector<float> output(static_cast<std::size_t>(kBlockSize), 0.0f);

    h.process(input.data(), output.data(), kBlockSize, 440.0f);

    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE_THAT(output[static_cast<std::size_t>(i)],
                     WithinAbs(input[static_cast<std::size_t>(i)], 1e-5f));
}

TEST_CASE("Harmonizer -- output always within [-1, 1]", "[harmonizer]")
{
    Harmonizer h;
    h.prepare(kSampleRate, kBlockSize);
    h.setMix(0.8f);

    const auto input = test_helpers::generateSine(440.0f, kSampleRate, kLongBlock);
    std::vector<float> output(static_cast<std::size_t>(kLongBlock), 0.0f);

    h.process(input.data(), output.data(), kLongBlock, 440.0f);

    REQUIRE(test_helpers::allSamplesInRange(output.data(), kLongBlock));
}

TEST_CASE("Harmonizer -- silence in, silence out (all voices)", "[harmonizer]")
{
    Harmonizer h;
    h.prepare(kSampleRate, kBlockSize);
    h.setMix(0.5f);

    const auto input = test_helpers::generateSilence(kBlockSize);
    std::vector<float> output(static_cast<std::size_t>(kBlockSize), 0.0f);

    h.process(input.data(), output.data(), kBlockSize, 0.0f);

    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE_THAT(output[static_cast<std::size_t>(i)], WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Harmonizer -- disable all voices -> output equals dry input", "[harmonizer]")
{
    Harmonizer h;
    h.prepare(kSampleRate, kBlockSize);
    h.setMix(0.5f);
    h.setVoiceEnabled(0, false);
    h.setVoiceEnabled(1, false);

    const auto input = test_helpers::generateSine(440.0f, kSampleRate, kBlockSize);
    std::vector<float> output(static_cast<std::size_t>(kBlockSize), 0.0f);

    h.process(input.data(), output.data(), kBlockSize, 440.0f);

    // Only dry component: output = (1-mix)*input = 0.5*input
    for (int i = 0; i < kBlockSize; ++i)
        REQUIRE_THAT(output[static_cast<std::size_t>(i)],
                     WithinAbs(0.5f * input[static_cast<std::size_t>(i)], 1e-5f));
}

TEST_CASE("Harmonizer -- shifted output is non-silent for non-silent input", "[harmonizer]")
{
    Harmonizer h;
    h.prepare(kSampleRate, kBlockSize);
    h.setMix(1.0f); // wet only

    const auto input = test_helpers::generateSine(440.0f, kSampleRate, kLongBlock);
    std::vector<float> output(static_cast<std::size_t>(kLongBlock), 0.0f);

    h.process(input.data(), output.data(), kLongBlock, 440.0f);

    // Output must have energy (not silent)
    const float rms = test_helpers::computeRms(output.data(), kLongBlock);
    REQUIRE(rms > 0.01f);
}

TEST_CASE("Harmonizer -- setVoiceInterval does not crash", "[harmonizer]")
{
    Harmonizer h;
    h.prepare(kSampleRate, kBlockSize);

    REQUIRE_NOTHROW(h.setVoiceInterval(0, 7.0f));
    REQUIRE_NOTHROW(h.setVoiceInterval(1, -7.0f));
    REQUIRE_NOTHROW(h.setVoiceInterval(-1, 5.0f)); // out of range — should be ignored
    REQUIRE_NOTHROW(h.setVoiceInterval(5,  5.0f)); // out of range — should be ignored
}
