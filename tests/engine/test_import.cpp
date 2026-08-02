#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/ImportPipeline.h"
#include "engine/Analysis/BpmDetector.h"

#include <cmath>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Génère un signal sinusoïdal à `freq` Hz pendant `durationSec` secondes.
static std::vector<float> makeSine(float freq, float durationSec, float sr = 44100.0f)
{
    const int n = static_cast<int>(durationSec * sr);
    std::vector<float> out(static_cast<std::size_t>(n));
    const float twoPiF = 2.0f * 3.14159265f * freq / sr;
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] = std::sin(twoPiF * static_cast<float>(i));
    return out;
}

/// Génère un signal pulsé à `bpm` battements par minute pendant `durationSec` s.
/// Chaque battement est une impulsion gaussienne courte.
static std::vector<float> makePulsed(float bpm, float durationSec, float sr = 44100.0f)
{
    const int   n          = static_cast<int>(durationSec * sr);
    const float beatPeriod = sr * 60.0f / bpm;
    std::vector<float> out(static_cast<std::size_t>(n), 0.0f);

    for (int beat = 0; ; ++beat) {
        const int center = static_cast<int>(static_cast<float>(beat) * beatPeriod);
        if (center >= n) break;
        // Impulsion gaussienne de σ = 200 samples
        for (int i = std::max(0, center - 800); i < std::min(n, center + 800); ++i) {
            const float d = static_cast<float>(i - center);
            out[static_cast<std::size_t>(i)] += std::exp(-d * d / (2.0f * 200.0f * 200.0f));
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// T-IM1 : analyzeSync retourne immédiatement (synchrone)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-IM1: analyzeSync returns without blocking", "[import]")
{
    engine::ImportPipeline pipeline;
    const auto pcm = makeSine(440.0f, 2.0f);
    const auto result = pipeline.analyzeSync(pcm.data(),
                                              static_cast<int>(pcm.size()),
                                              1, 44100.0f, 120.0f);
    // Si on arrive ici, c'est synchrone. Vérification minimale de struct valide.
    CHECK(result.timeRatio > 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-IM2 : sinusoïde ~63 BPM, projectBpm=120 → correction d'octave ≈ 126
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-IM2: octave correction doubles 63 BPM to ~126 at projectBpm=120", "[import]")
{
    // Générer un signal pulsé à ~63 BPM (= 60/120 × 2 → moitié de 126)
    const auto pcm = makePulsed(63.0f, 10.0f, 44100.0f);

    engine::ImportPipeline pipeline;
    const auto result = pipeline.analyzeSync(pcm.data(),
                                              static_cast<int>(pcm.size()),
                                              1, 44100.0f, 120.0f);

    // Le BPM brut doit être proche de 63, le corrigé proche de 126
    INFO("detectedBpm=" << result.detectedBpm
         << " correctedBpm=" << result.correctedBpm);

    // La correction doit avoir doublé le BPM
    CHECK(result.correctedBpm > result.detectedBpm * 1.5f);
    CHECK(result.correctedBpm == Catch::Approx(126.0f).margin(15.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// T-IM3 : roleConfidence < 0.75 → autoLoopSync == false
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-IM3: low roleConfidence disables autoLoopSync", "[import]")
{
    // Sans ONNX, roleConfidence = 0 (stub) → autoLoopSync doit être false
    engine::ImportPipeline pipeline;
    const auto pcm = makePulsed(120.0f, 8.0f, 44100.0f);
    const auto result = pipeline.analyzeSync(pcm.data(),
                                              static_cast<int>(pcm.size()),
                                              1, 44100.0f, 120.0f);
    CHECK_FALSE(result.autoLoopSync);
    CHECK(result.roleConfidence < 0.75f);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-IM4 : projectBpm == 0 → pas de correction d'octave
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-IM4: projectBpm=0 disables octave correction", "[import]")
{
    const auto pcm = makePulsed(63.0f, 10.0f, 44100.0f);

    engine::ImportPipeline pipeline;
    const auto result = pipeline.analyzeSync(pcm.data(),
                                              static_cast<int>(pcm.size()),
                                              1, 44100.0f, 0.0f);

    // Sans correction, detectedBpm == correctedBpm
    INFO("detectedBpm=" << result.detectedBpm
         << " correctedBpm=" << result.correctedBpm);
    CHECK(result.detectedBpm == Catch::Approx(result.correctedBpm).margin(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// T-IM5 : silence (30 s de zéros) → autoLoopSync == false
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-IM5: silence produces no loopable result", "[import]")
{
    const int   n   = static_cast<int>(30.0f * 44100.0f);
    const std::vector<float> silence(static_cast<std::size_t>(n), 0.0f);

    engine::ImportPipeline pipeline;
    const auto result = pipeline.analyzeSync(silence.data(), n, 1, 44100.0f, 120.0f);

    CHECK_FALSE(result.autoLoopSync);
    CHECK(result.loopBeats == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// T-IM-OCT : correction d'octave directement sur BpmDetector
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("T-IM-OCT: BpmDetector octave correction cases", "[import][bpm]")
{
    using BPM = engine::analysis::BpmDetector;

    SECTION("60 BPM brut, projectBpm=120 → corrigé ≈ 120")
    {
        // Générer un signal pulsé à 60 BPM
        const auto pcm = makePulsed(60.0f, 12.0f, 44100.0f);
        const auto res = BPM::detectWithOctaveCorrection(
            pcm.data(), static_cast<int>(pcm.size()), 44100.0f, 120.0f);
        INFO("raw/corrected bpm=" << res.bpm);
        CHECK(res.bpm == Catch::Approx(120.0f).margin(15.0f));
    }

    SECTION("240 BPM brut, projectBpm=120 → corrigé ≈ 120")
    {
        const auto pcm = makePulsed(240.0f, 12.0f, 44100.0f);
        const auto res = BPM::detectWithOctaveCorrection(
            pcm.data(), static_cast<int>(pcm.size()), 44100.0f, 120.0f);
        INFO("raw/corrected bpm=" << res.bpm);
        CHECK(res.bpm == Catch::Approx(120.0f).margin(15.0f));
    }

    SECTION("133 BPM brut, projectBpm=120 → reste ≈ 133")
    {
        const auto pcm = makePulsed(133.0f, 12.0f, 44100.0f);
        const auto res = BPM::detectWithOctaveCorrection(
            pcm.data(), static_cast<int>(pcm.size()), 44100.0f, 120.0f);
        INFO("raw/corrected bpm=" << res.bpm);
        // 133 est plus proche de 120 que 66.5 ou 266 → pas de correction
        CHECK(res.bpm == Catch::Approx(133.0f).margin(15.0f));
    }
}
