#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "dsp/DelayEffect.h"
#include "dsp/OctaverEffect.h"
#include "dsp/SlicerEffect.h"
#include "dsp/SoloAssistant.h"
#include "dsp/TunerEffect.h"
#include "dsp/WhammyEffect.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static constexpr double kSR       = 44100.0;
static constexpr int    kBlock    = 512;
static constexpr float  kPitchHz  = 440.0f;

static bool allZero(const float* buf, int n)
{
    for (int i = 0; i < n; ++i)
        if (buf[i] != 0.0f)
            return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DelayEffect
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DelayEffect type is Delay", "[delay]")
{
    dsp::DelayEffect e;
    REQUIRE(e.type() == dsp::EffectType::Delay);
}

TEST_CASE("DelayEffect has 4 parameters", "[delay]")
{
    dsp::DelayEffect e;
    REQUIRE(e.paramCount() == 4);
}

TEST_CASE("DelayEffect default params", "[delay]")
{
    dsp::DelayEffect e;
    REQUIRE(e.getParam(0) == Catch::Approx(500.0f)); // time
    REQUIRE(e.getParam(1) == Catch::Approx(0.3f));   // feedback
    REQUIRE(e.getParam(2) == Catch::Approx(0.5f));   // mix
}

TEST_CASE("DelayEffect setParam getParam roundtrip", "[delay]")
{
    dsp::DelayEffect e;
    e.setParam(0, 250.0f);
    e.setParam(1, 0.6f);
    e.setParam(2, 0.8f);
    REQUIRE(e.getParam(0) == Catch::Approx(250.0f));
    REQUIRE(e.getParam(1) == Catch::Approx(0.6f));
    REQUIRE(e.getParam(2) == Catch::Approx(0.8f));
}

TEST_CASE("DelayEffect silence in gives silence out", "[delay]")
{
    dsp::DelayEffect e;
    e.prepare(kSR, kBlock);

    std::array<float, kBlock> buf{};
    buf.fill(0.0f);

    e.process(buf.data(), kBlock, kPitchHz);
    REQUIRE(allZero(buf.data(), kBlock));
}

TEST_CASE("DelayEffect mix=0 gives dry pass-through", "[delay]")
{
    dsp::DelayEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(2, 0.0f); // mix = 0 => output = dry = input

    std::array<float, kBlock> buf{};
    for (int i = 0; i < kBlock; ++i)
        buf[i] = static_cast<float>(i) * 0.001f;

    std::array<float, kBlock> expected = buf;
    e.process(buf.data(), kBlock, kPitchHz);

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(expected[i]));
}

TEST_CASE("DelayEffect reset clears buffer", "[delay]")
{
    dsp::DelayEffect e;
    e.prepare(kSR, kBlock);

    // Feed non-zero signal
    std::array<float, kBlock> buf{};
    buf.fill(1.0f);
    e.process(buf.data(), kBlock, kPitchHz);

    // Reset then process silence
    e.reset();
    buf.fill(0.0f);
    e.process(buf.data(), kBlock, kPitchHz);
    REQUIRE(allZero(buf.data(), kBlock));
}

TEST_CASE("DelayEffect ping-pong: impulse in L echoes into R after 2 delays", "[delay][stereo]")
{
    // Ping-pong: impulse in L →
    //   +ds samples : echo fires in L, cross-feed 0.7 written into R buffer
    //   +2ds samples: echo fires in R
    dsp::DelayEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(0, 50.0f);   // 50 ms delay → 2205 samples @ 44100
    e.setParam(1, 0.7f);    // feedback
    e.setParam(2, 1.0f);    // 100% wet

    const int ds          = static_cast<int>(0.05 * kSR);  // 2205
    const int blocksTotal = (2 * ds / kBlock) + 3;

    float totalREnergy = 0.f;
    std::vector<float> L(kBlock, 0.f), R(kBlock, 0.f);
    for (int b = 0; b < blocksTotal; ++b)
    {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        if (b == 0) L[0] = 1.0f;
        e.processStereo(L.data(), R.data(), kBlock, 0.f);
        for (float v : R) totalREnergy += v * v;
    }
    REQUIRE(totalREnergy > 1e-4f);
}

// ─────────────────────────────────────────────────────────────────────────────
// WhammyEffect
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("WhammyEffect type is Whammy", "[whammy]")
{
    dsp::WhammyEffect e;
    REQUIRE(e.type() == dsp::EffectType::Whammy);
}

TEST_CASE("WhammyEffect has 4 parameters", "[whammy]")
{
    dsp::WhammyEffect e;
    REQUIRE(e.paramCount() == 4);
}

TEST_CASE("WhammyEffect default params", "[whammy]")
{
    dsp::WhammyEffect e;
    REQUIRE(e.getParam(0) == Catch::Approx(0.0f));  // expression
    REQUIRE(e.getParam(1) == Catch::Approx(12.0f)); // toePitch
    REQUIRE(e.getParam(2) == Catch::Approx(0.0f));  // heelPitch
    REQUIRE(e.getParam(3) == Catch::Approx(1.0f));  // mix
}

TEST_CASE("WhammyEffect setParam getParam roundtrip", "[whammy]")
{
    dsp::WhammyEffect e;
    e.setParam(0, 0.75f);
    e.setParam(1, -12.0f);
    e.setParam(2, 5.0f);
    e.setParam(3, 0.5f);
    REQUIRE(e.getParam(0) == Catch::Approx(0.75f));
    REQUIRE(e.getParam(1) == Catch::Approx(-12.0f));
    REQUIRE(e.getParam(2) == Catch::Approx(5.0f));
    REQUIRE(e.getParam(3) == Catch::Approx(0.5f));
}

TEST_CASE("WhammyEffect expression=0 heelPitch=0 bypasses signal", "[whammy]")
{
    // expression=0, heelPitch=0 → currentShift=0 → early return, signal untouched
    dsp::WhammyEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(0, 0.0f); // expression = 0
    e.setParam(2, 0.0f); // heelPitch = 0

    std::array<float, kBlock> buf{};
    for (int i = 0; i < kBlock; ++i)
        buf[i] = 0.5f;

    std::array<float, kBlock> expected = buf;
    e.process(buf.data(), kBlock, kPitchHz);

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(expected[i]));
}

TEST_CASE("WhammyEffect silence in gives silence out", "[whammy]")
{
    dsp::WhammyEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(0, 1.0f); // expression = 1 (active shift)

    std::array<float, kBlock> buf{};
    buf.fill(0.0f);
    e.process(buf.data(), kBlock, kPitchHz);
    REQUIRE(allZero(buf.data(), kBlock));
}

// ─────────────────────────────────────────────────────────────────────────────
// OctaverEffect
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("OctaverEffect type is Octaver", "[octaver]")
{
    dsp::OctaverEffect e;
    REQUIRE(e.type() == dsp::EffectType::Octaver);
}

TEST_CASE("OctaverEffect has 3 parameters", "[octaver]")
{
    dsp::OctaverEffect e;
    REQUIRE(e.paramCount() == 3);
}

TEST_CASE("OctaverEffect default params", "[octaver]")
{
    dsp::OctaverEffect e;
    REQUIRE(e.getParam(0) == Catch::Approx(0.7f)); // oct1
    REQUIRE(e.getParam(1) == Catch::Approx(0.3f)); // oct2
    REQUIRE(e.getParam(2) == Catch::Approx(1.0f)); // dry
}

TEST_CASE("OctaverEffect setParam getParam roundtrip", "[octaver]")
{
    dsp::OctaverEffect e;
    e.setParam(0, 0.5f);
    e.setParam(1, 0.1f);
    e.setParam(2, 0.8f);
    REQUIRE(e.getParam(0) == Catch::Approx(0.5f));
    REQUIRE(e.getParam(1) == Catch::Approx(0.1f));
    REQUIRE(e.getParam(2) == Catch::Approx(0.8f));
}

TEST_CASE("OctaverEffect dry=1 oct1=0 oct2=0 gives pass-through", "[octaver]")
{
    dsp::OctaverEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(0, 0.0f); // oct1 = 0
    e.setParam(1, 0.0f); // oct2 = 0
    e.setParam(2, 1.0f); // dry = 1

    std::array<float, kBlock> buf{};
    for (int i = 0; i < kBlock; ++i)
        buf[i] = static_cast<float>(i) * 0.001f;

    std::array<float, kBlock> expected = buf;
    e.process(buf.data(), kBlock, kPitchHz);

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(expected[i]));
}

TEST_CASE("OctaverEffect silence in gives silence out", "[octaver]")
{
    dsp::OctaverEffect e;
    e.prepare(kSR, kBlock);

    std::array<float, kBlock> buf{};
    buf.fill(0.0f);
    e.process(buf.data(), kBlock, kPitchHz);
    REQUIRE(allZero(buf.data(), kBlock));
}

// ─────────────────────────────────────────────────────────────────────────────
// TunerEffect
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TunerEffect type is Tuner", "[tuner]")
{
    dsp::TunerEffect e;
    REQUIRE(e.type() == dsp::EffectType::Tuner);
}

TEST_CASE("TunerEffect has 2 parameters", "[tuner]")
{
    dsp::TunerEffect e;
    REQUIRE(e.paramCount() == 2);
}

TEST_CASE("TunerEffect default mute is on", "[tuner]")
{
    dsp::TunerEffect e;
    REQUIRE(e.getParam(0) == Catch::Approx(1.0f));  // mute on by default (tuner mode)
    REQUIRE(e.getParam(1) == Catch::Approx(442.0f)); // A4 reference
}

TEST_CASE("TunerEffect setParam getParam roundtrip", "[tuner]")
{
    dsp::TunerEffect e;
    e.setParam(0, 1.0f);
    REQUIRE(e.getParam(0) == Catch::Approx(1.0f));
    e.setParam(0, 0.0f);
    REQUIRE(e.getParam(0) == Catch::Approx(0.0f));
}

TEST_CASE("TunerEffect mute=0 passes signal through", "[tuner]")
{
    dsp::TunerEffect e;
    e.setParam(0, 0.0f); // mute off

    std::array<float, kBlock> buf{};
    for (int i = 0; i < kBlock; ++i)
        buf[i] = 0.5f;

    std::array<float, kBlock> expected = buf;
    e.process(buf.data(), kBlock, kPitchHz);

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(expected[i]));
}

TEST_CASE("TunerEffect mute=1 silences output", "[tuner]")
{
    dsp::TunerEffect e;
    e.setParam(0, 1.0f); // mute on

    std::array<float, kBlock> buf{};
    buf.fill(1.0f);
    e.process(buf.data(), kBlock, kPitchHz);
    REQUIRE(allZero(buf.data(), kBlock));
}

TEST_CASE("TunerEffect disabled bypasses entirely", "[tuner]")
{
    dsp::TunerEffect e;
    e.enabled.store(false, std::memory_order_release);
    e.setParam(0, 1.0f); // mute on, but effect disabled

    std::array<float, kBlock> buf{};
    buf.fill(1.0f);
    e.process(buf.data(), kBlock, kPitchHz);

    // Disabled → no processing, signal unchanged
    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// SlicerEffect
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SlicerEffect type is Slicer", "[slicer]")
{
    dsp::SlicerEffect e;
    REQUIRE(e.type() == dsp::EffectType::Slicer);
}

TEST_CASE("SlicerEffect has 2 parameters", "[slicer]")
{
    dsp::SlicerEffect e;
    REQUIRE(e.paramCount() == 2);
}

TEST_CASE("SlicerEffect default params", "[slicer]")
{
    dsp::SlicerEffect e;
    REQUIRE(e.getParam(0) == Catch::Approx(4.0f));  // rate
    REQUIRE(e.getParam(1) == Catch::Approx(1.0f));  // depth
}

TEST_CASE("SlicerEffect setParam getParam roundtrip", "[slicer]")
{
    dsp::SlicerEffect e;
    e.setParam(0, 8.0f);
    e.setParam(1, 0.5f);
    REQUIRE(e.getParam(0) == Catch::Approx(8.0f));
    REQUIRE(e.getParam(1) == Catch::Approx(0.5f));
}

TEST_CASE("SlicerEffect silence in gives silence out", "[slicer]")
{
    dsp::SlicerEffect e;
    e.prepare(kSR, kBlock);

    std::array<float, kBlock> buf{};
    buf.fill(0.0f);
    e.process(buf.data(), kBlock, kPitchHz);
    REQUIRE(allZero(buf.data(), kBlock));
}

TEST_CASE("SlicerEffect depth=0 passes signal through", "[slicer]")
{
    // depth=0 → lfoVal is always 1.0 (ON: 1.0, OFF: 1.0-0.0=1.0) → no attenuation
    dsp::SlicerEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(1, 0.0f); // depth = 0

    std::array<float, kBlock> buf{};
    buf.fill(0.5f);

    std::array<float, kBlock> expected = buf;
    e.process(buf.data(), kBlock, kPitchHz);

    for (int i = 0; i < kBlock; ++i)
        REQUIRE(buf[i] == Catch::Approx(expected[i]));
}

TEST_CASE("SlicerEffect depth=1 attenuates during off phase", "[slicer]")
{
    // With depth=1 and a very fast rate, some samples should be zeroed.
    // rate=20 Hz at 44100 → half-period = 44100/20/2 = ~1102 samples OFF.
    // With kBlock=512 and phase starting at 0, first half of LFO is ON (lfoVal=1).
    // If rate is high enough that phase crosses 0.5 within the block, some OFF samples exist.
    // rate=100 Hz: phaseInc = 100/44100 ≈ 0.00227; 512 * 0.00227 ≈ 1.16 cycles → yes, crosses 0.5

    dsp::SlicerEffect e;
    e.prepare(kSR, kBlock);
    e.setParam(0, 100.0f); // rate = 100 Hz — fast LFO to ensure multiple cycles
    e.setParam(1, 1.0f);   // depth = 1 — full gating (OFF = silent)

    std::array<float, kBlock> buf{};
    buf.fill(1.0f);
    e.process(buf.data(), kBlock, kPitchHz);

    // At least some samples should be 0 (gated) and some should be 1 (pass)
    bool hasOnes  = std::any_of(buf.begin(), buf.end(), [](float s) { return s == 1.0f; });
    bool hasZeros = std::any_of(buf.begin(), buf.end(), [](float s) { return s == 0.0f; });
    REQUIRE(hasOnes);
    REQUIRE(hasZeros);
}

TEST_CASE("SlicerEffect reset resets phase", "[slicer]")
{
    dsp::SlicerEffect e;
    e.prepare(kSR, kBlock);

    // Process a block to advance phase
    std::array<float, kBlock> buf{};
    buf.fill(1.0f);
    e.process(buf.data(), kBlock, kPitchHz);

    // Reset should set phase back to 0
    e.reset();

    // Re-process — first sample should be unattenuated since phase=0 < 0.5 (ON)
    buf.fill(1.0f);
    e.process(buf.data(), 1, kPitchHz);
    REQUIRE(buf[0] == Catch::Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// T-C1 : SoloAssistant — suggestions all in-scale
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SoloAssistant: suggestions are all in C major scale", "[solo]")
{
    dsp::SoloAssistant sa;
    sa.configure(0, 0);  // C major
    sa.setPreset(dsp::SoloPreset::Dub);
    sa.recordNote(60);   // C4

    const auto suggestions = sa.suggest(48, 84);
    REQUIRE_FALSE(suggestions.empty());

    static constexpr bool kCMajor[12] = {
        true,false,true,false,true,true,false,true,false,true,false,true };
    for (int note : suggestions)
    {
        REQUIRE(note >= 48);
        REQUIRE(note <= 84);
        REQUIRE(kCMajor[note % 12]);
    }
}

TEST_CASE("SoloAssistant: Off preset returns empty suggestions", "[solo]")
{
    dsp::SoloAssistant sa;
    sa.configure(0, 0);
    sa.setPreset(dsp::SoloPreset::Off);
    sa.recordNote(60);
    REQUIRE(sa.suggest(48, 84).empty());
}

TEST_CASE("SoloAssistant: configure resets lastNote so no suggestions yet", "[solo]")
{
    dsp::SoloAssistant sa;
    sa.configure(0, 0);
    sa.setPreset(dsp::SoloPreset::Prudent);
    sa.recordNote(60);
    sa.configure(5, 1);  // change key — resets lastNote
    REQUIRE(sa.suggest(48, 84).empty());
}

TEST_CASE("SoloAssistant: Prudent returns 2 suggestions, Dub returns 3", "[solo]")
{
    dsp::SoloAssistant sa;
    sa.configure(0, 0);
    sa.recordNote(60);

    sa.setPreset(dsp::SoloPreset::Prudent);
    REQUIRE(sa.suggest(48, 84).size() == 2);

    sa.setPreset(dsp::SoloPreset::Dub);
    REQUIRE(sa.suggest(48, 84).size() == 3);
}

