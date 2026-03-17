#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "dsp/YinPitchTracker.h"
#include "TestHelpers.h"

using dsp::YinPitchTracker;
using dsp::PitchResult;
using Catch::Matchers::WithinAbs;

static constexpr float kSampleRate = 44100.0f;
static constexpr int   kBlockSize  = 128;

/// Feed a buffer to YIN in kBlockSize chunks, return last result.
static PitchResult feedYin(YinPitchTracker& yin,
                             const std::vector<float>& signal)
{
    PitchResult result{};
    const int   total  = static_cast<int>(signal.size());
    for (int offset = 0; offset < total; offset += kBlockSize)
    {
        const int n = std::min(kBlockSize, total - offset);
        result = yin.process(signal.data() + offset, n);
    }
    return result;
}

TEST_CASE("YIN -- detects A4 (440 Hz) within +/-5 Hz", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    // Feed 4096 samples (enough for window accumulation)
    const auto signal = test_helpers::generateSine(440.0f, kSampleRate, 4096);
    const PitchResult result = feedYin(yin, signal);

    REQUIRE(result.frequencyHz > 0.0f);
    REQUIRE_THAT(result.frequencyHz, WithinAbs(440.0f, 5.0f));
    REQUIRE(result.confidence > 0.5f);
}

TEST_CASE("YIN -- detects 220 Hz within +/-5 Hz", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    const auto signal = test_helpers::generateSine(220.0f, kSampleRate, 6000);
    const PitchResult result = feedYin(yin, signal);

    REQUIRE(result.frequencyHz > 0.0f);
    REQUIRE_THAT(result.frequencyHz, WithinAbs(220.0f, 5.0f));
}

TEST_CASE("YIN -- silence returns zero frequency", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    const auto signal = test_helpers::generateSilence(4096);
    const PitchResult result = feedYin(yin, signal);

    // Silence = no pitch or very low confidence
    REQUIRE((result.frequencyHz == 0.0f || result.confidence < 0.3f));
}

TEST_CASE("YIN -- reset clears state", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    // Feed 440 Hz
    auto signal = test_helpers::generateSine(440.0f, kSampleRate, 4096);
    feedYin(yin, signal);

    // Reset then feed silence
    yin.reset();
    signal = test_helpers::generateSilence(kBlockSize);
    const PitchResult result = yin.process(signal.data(), kBlockSize);

    // After reset, no accumulated data → no pitch
    REQUIRE(result.frequencyHz == 0.0f);
}

TEST_CASE("YIN -- result frequency stays in valid range when voiced", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    // Feed a tone in the middle of sax range
    const auto signal = test_helpers::generateSine(600.0f, kSampleRate, 4096);
    const PitchResult result = feedYin(yin, signal);

    if (result.frequencyHz > 0.0f)
    {
        REQUIRE(result.frequencyHz >= 80.0f);
        REQUIRE(result.frequencyHz <= 2000.0f);
    }
}
