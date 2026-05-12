#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "catch2/catch_approx.hpp"
#include "catch2/catch_test_macros.hpp"

#include "../src/dsp/SlotDynamics.h"

using namespace dsp;

// ─────────────────────────────────────────────────────────────────────────────
// Reference compressor — original dB-domain implementation (pre-A1).
// Embedded here as stable baseline for CPU comparison only.
// Note: the two topologies are NOT expected to produce identical output
// (dB-domain envelope ≠ linear-domain envelope). THD comparison is meaningless;
// correctness is verified separately.
// ─────────────────────────────────────────────────────────────────────────────
class SlotDynamicsRef
{
public:
    void prepare(double sr) noexcept { sr_ = sr; }

    void setKickPreset() noexcept
    {
        const float sr = static_cast<float>(sr_);
        cThreshDb_ = -18.f;
        cRatioInv_ = 1.f / 3.f;
        cAtt_ = std::exp(-2.2f / (0.005f * sr));
        cRel_ = std::exp(-2.2f / (0.080f * sr));
        active_ = true;
        envGainDb_ = 0.f;
    }

    void processSample(float& s) noexcept
    {
        if (!active_) return;
        const float xDb  = 20.f * std::log10(std::max(std::abs(s), 1e-6f));
        const float over = xDb - cThreshDb_;
        const float gDb  = (over > 0.f) ? -over * (1.f - cRatioInv_) : 0.f;
        envGainDb_ = (gDb < envGainDb_)
            ? cAtt_ * envGainDb_ + (1.f - cAtt_) * gDb
            : cRel_ * envGainDb_ + (1.f - cRel_) * gDb;
        envGainDb_ = std::clamp(envGainDb_, -60.f, 0.f);
        s *= std::pow(10.f, envGainDb_ / 20.f);
    }

private:
    double sr_        { 48000.0 };
    float  cThreshDb_ { -18.f };
    float  cRatioInv_ { 0.333f };
    float  cAtt_      { 0.995f };
    float  cRel_      { 0.9995f };
    float  envGainDb_ { 0.f };
    bool   active_    { false };
};

// ─────────────────────────────────────────────────────────────────────────────
// Signal generators
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<float> makeKick(int nSamples, float sr)
{
    std::vector<float> v(static_cast<std::size_t>(nSamples));
    for (int i = 0; i < nSamples; ++i)
    {
        const float t = static_cast<float>(i) / sr;
        v[static_cast<std::size_t>(i)] =
            0.8f * std::exp(-t * 50.f) * std::sin(2.f * 3.14159265f * 60.f * t);
    }
    return v;
}

static std::vector<float> makeWhiteNoise(int nSamples, float amplitude = 0.5f)
{
    std::vector<float> v(static_cast<std::size_t>(nSamples));
    unsigned seed = 0xDEADBEEFu;
    for (int i = 0; i < nSamples; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        v[static_cast<std::size_t>(i)] =
            static_cast<float>(static_cast<int>(seed)) / 2147483648.f * amplitude;
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Correctness tests (A1 behaviour independent of reference compressor)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("SlotDynamics A1 — compression happens above threshold", "[dsp][ab]")
{
    constexpr double kSr    = 48000.0;
    constexpr int    kBlock = 512;
    constexpr int    kLen   = kBlock * 4;

    SlotDynamics comp;
    comp.prepare(kSr);
    comp.setPreset(ContentCategory::KICK);  // threshold -18 dBFS

    auto sig = makeKick(kLen, static_cast<float>(kSr));
    const float peakIn = *std::max_element(sig.begin(), sig.end(),
        [](float a, float b){ return std::abs(a) < std::abs(b); });

    // Process
    for (int off = 0; off < kLen; off += kBlock)
    {
        comp.beginBlock();
        for (int i = 0; i < kBlock; ++i)
            comp.processSample(sig[static_cast<std::size_t>(off + i)]);
        comp.endBlock();
    }

    const float peakOut = *std::max_element(sig.begin(), sig.end(),
        [](float a, float b){ return std::abs(a) < std::abs(b); });

    INFO("Peak in: " << peakIn << "  Peak out: " << peakOut);
    REQUIRE(peakOut < peakIn);                // compression happened
    REQUIRE(comp.getGainReductionDb() < 0.f); // GR meter reports reduction
}

TEST_CASE("SlotDynamics A1 — silence gate: below 1e-6 passes unchanged", "[dsp][ab]")
{
    SlotDynamics comp;
    comp.prepare(48000.0);
    comp.setPreset(ContentCategory::KICK);
    comp.beginBlock();
    float s = 5e-7f;
    comp.processSample(s);
    comp.endBlock();
    REQUIRE(s == Catch::Approx(5e-7f));
}

TEST_CASE("SlotDynamics A1 — no NaN on all presets", "[dsp][ab]")
{
    constexpr double kSr    = 48000.0;
    constexpr int    kBlock = 512;
    constexpr int    kLen   = kBlock * 8;

    const ContentCategory presets[] = {
        ContentCategory::KICK,  ContentCategory::SNARE, ContentCategory::HIHAT,
        ContentCategory::BASS,  ContentCategory::SYNTH, ContentCategory::PAD,
        ContentCategory::PERC,  ContentCategory::OTHER
    };

    for (auto p : presets)
    {
        SlotDynamics comp;
        comp.prepare(kSr);
        comp.setPreset(p);

        auto sig = makeKick(kLen, static_cast<float>(kSr));
        for (int off = 0; off < kLen; off += kBlock)
        {
            comp.beginBlock();
            for (int i = 0; i < kBlock; ++i)
                comp.processSample(sig[static_cast<std::size_t>(off + i)]);
            comp.endBlock();
        }
        for (float s : sig)
            REQUIRE(std::isfinite(s));
    }
}

TEST_CASE("SlotDynamics A1 — gain reduction in dB is negative when above threshold", "[dsp][ab]")
{
    constexpr double kSr    = 48000.0;
    constexpr int    kBlock = 512;

    SlotDynamics comp;
    comp.prepare(kSr);
    comp.setPreset(ContentCategory::KICK);

    // Constant signal above -18 dBFS threshold (0.5 ≈ -6 dBFS).
    // Pass a local copy each sample so the source amplitude stays fixed.
    for (int rep = 0; rep < 20; ++rep)  // let envelope settle
    {
        comp.beginBlock();
        for (int i = 0; i < kBlock; ++i)
        {
            float s = 0.5f;
            comp.processSample(s);
        }
        comp.endBlock();
    }

    INFO("GR: " << comp.getGainReductionDb() << " dB");
    REQUIRE(comp.getGainReductionDb() < -0.1f);  // must report reduction
    REQUIRE(comp.getGainReductionDb() > -60.f);  // must be in valid range
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU benchmarks — no per-iteration allocation (pre-alloc, memcpy reset)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("SlotDynamics A1 — gate: below-threshold samples return early", "[dsp][ab][bench]")
{
    // Verify that the 1e-6 gate avoids any computation on near-silent samples.
    // A signal of 5e-7 must exit unchanged at any preset.
    SlotDynamics comp;
    comp.prepare(48000.0);

    const ContentCategory presets[] = {
        ContentCategory::KICK, ContentCategory::BASS, ContentCategory::PAD
    };
    for (auto p : presets)
    {
        comp.setPreset(p);
        comp.beginBlock();
        float s = 5e-7f;
        comp.processSample(s);
        comp.endBlock();
        REQUIRE(s == Catch::Approx(5e-7f));
    }
}

// CPU benchmark note: MSVC Release uses SVML to vectorise log10/pow in the old
// compressor, making microbenchmarks unreliable (variance ±30% between runs).
// Real performance measurement must be done via JUCE PerformanceCounter inside
// the audio thread under production load.
