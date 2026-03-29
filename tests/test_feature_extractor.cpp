#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "dsp/FeatureExtractor.h"

#include <cmath>
#include <vector>

using Catch::Approx;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<float> makeSine(float freqHz, float durationSec,
                                    double sampleRate = 44100.0,
                                    float amplitude   = 0.5f)
{
    const int N = static_cast<int>(sampleRate * durationSec);
    std::vector<float> pcm(N);
    for (int i = 0; i < N; ++i)
        pcm[i] = amplitude
                 * std::sin(2.f * 3.14159265f * freqHz
                            * static_cast<float>(i)
                            / static_cast<float>(sampleRate));
    return pcm;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sprint 9.1 — MixFeatures struct + basic extraction
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("FeatureExtractor -- empty buffer returns zero features", "[feature_extractor]")
{
    std::vector<float> empty;
    auto f = dsp::FeatureExtractor::extract(empty, 44100.0);
    REQUIRE(f.rms              == Approx(0.f));
    REQUIRE(f.spectralCentroid == Approx(0.f));
    REQUIRE(f.crestFactor      == Approx(0.f));
    REQUIRE(f.durationMs       == Approx(0.f));
}

TEST_CASE("FeatureExtractor -- duration is correct", "[feature_extractor]")
{
    auto pcm = makeSine(440.f, 1.0f);
    auto f = dsp::FeatureExtractor::extract(pcm, 44100.0);
    REQUIRE(f.durationMs == Approx(1000.f).margin(1.f));
}

TEST_CASE("FeatureExtractor -- low-freq signal low-band dominant", "[feature_extractor]")
{
    // 80 Hz sine: sub-bass → lowFrac should dominate over mid and high
    auto pcm = makeSine(80.f, 0.5f);
    auto f = dsp::FeatureExtractor::extract(pcm, 44100.0);
    REQUIRE(f.lowFrac > f.midFrac);
    REQUIRE(f.lowFrac > f.highFrac);
}

TEST_CASE("FeatureExtractor -- high-freq signal high-band dominant", "[feature_extractor]")
{
    // 8 kHz sine: high band dominant
    auto pcm = makeSine(8000.f, 0.2f);
    auto f = dsp::FeatureExtractor::extract(pcm, 44100.0);
    REQUIRE(f.highFrac > f.lowFrac);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sprint 9.2 — centroid ≈ 440 Hz, crest ≈ sqrt(2)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("FeatureExtractor -- sine 440 Hz centroid near 440 Hz", "[feature_extractor]")
{
    // 9.2 validation: sine 440 Hz → spectralCentroid ≈ 440 Hz (±30 Hz)
    auto pcm = makeSine(440.f, 0.5f);
    auto f = dsp::FeatureExtractor::extract(pcm, 44100.0);
    REQUIRE(f.spectralCentroid == Approx(440.f).margin(30.f));
}

TEST_CASE("FeatureExtractor -- sine 440 Hz crest near sqrt(2)", "[feature_extractor]")
{
    // 9.2 validation: sine → crestFactor ≈ 1.41
    auto pcm = makeSine(440.f, 0.5f);
    auto f = dsp::FeatureExtractor::extract(pcm, 44100.0);
    REQUIRE(f.crestFactor == Approx(std::sqrt(2.f)).margin(0.05f));
}

TEST_CASE("FeatureExtractor -- high centroid for 8 kHz sine", "[feature_extractor]")
{
    auto pcm = makeSine(8000.f, 0.2f);
    auto f = dsp::FeatureExtractor::extract(pcm, 44100.0);
    REQUIRE(f.spectralCentroid > 4000.f);
}
