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

    // Feed 6000 samples (enough for window accumulation + hop with v2 tracker)
    const auto signal = test_helpers::generateSine(440.0f, kSampleRate, 6000);
    const PitchResult result = feedYin(yin, signal);

    REQUIRE(result.frequencyHz > 0.0f);
    REQUIRE_THAT(result.frequencyHz, WithinAbs(440.0f, 5.0f));
    REQUIRE(result.confidence > 0.5f);
}

TEST_CASE("YIN -- detects 220 Hz within +/-5 Hz", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    const auto signal = test_helpers::generateSine(220.0f, kSampleRate, 8000);
    const PitchResult result = feedYin(yin, signal);

    REQUIRE(result.frequencyHz > 0.0f);
    REQUIRE_THAT(result.frequencyHz, WithinAbs(220.0f, 5.0f));
}

TEST_CASE("YIN -- silence returns zero frequency", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    const auto signal = test_helpers::generateSilence(6000);
    const PitchResult result = feedYin(yin, signal);

    // Silence = no pitch or very low confidence
    REQUIRE((result.frequencyHz == 0.0f || result.confidence < 0.3f));
}

TEST_CASE("YIN -- reset clears state", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    // Feed 440 Hz
    auto signal = test_helpers::generateSine(440.0f, kSampleRate, 6000);
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
    const auto signal = test_helpers::generateSine(600.0f, kSampleRate, 6000);
    const PitchResult result = feedYin(yin, signal);

    if (result.frequencyHz > 0.0f)
    {
        // minFrequency_ default = 85 Hz, kMaxFrequencyHz = 1400 Hz
        REQUIRE(result.frequencyHz >= 85.0f);
        REQUIRE(result.frequencyHz <= 1400.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// New v2 tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("YIN -- noise does not produce false pitch", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    // Feed white noise — the removed fallback should no longer invent pitch
    const auto noise = test_helpers::generateNoise(8000, 0.3f);

    int voicedCount = 0;
    int totalFrames = 0;
    const int total = static_cast<int>(noise.size());
    for (int offset = 0; offset < total; offset += kBlockSize)
    {
        const int n = std::min(kBlockSize, total - offset);
        PitchResult r = yin.process(noise.data() + offset, n);
        ++totalFrames;
        if (r.frequencyHz > 0.0f && r.confidence > 0.65f)
            ++voicedCount;
    }

    // Vast majority of frames should be unvoiced on noise.
    // Allow a small tolerance (1-2 frames) to avoid flaky test.
    REQUIRE(voicedCount <= 2);
}

TEST_CASE("YIN -- DC offset does not break detection", "[yin]")
{
    YinPitchTracker yin;
    yin.prepare(kSampleRate, kBlockSize);

    // 440 Hz sine with large DC offset of 0.3 — DC blocker should remove it
    const auto signal = test_helpers::generateSineWithDcOffset(
        440.0f, kSampleRate, 8000, 0.3f, 0.8f);
    const PitchResult result = feedYin(yin, signal);

    REQUIRE(result.frequencyHz > 0.0f);
    REQUIRE_THAT(result.frequencyHz, WithinAbs(440.0f, 10.0f));
    REQUIRE(result.confidence > 0.5f);
}
