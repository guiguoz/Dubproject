#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "engine/AudioGraph.h"

using namespace engine;

static constexpr float kPI = 3.14159265f;

// Génère un signal sinusoïdal sur N samples.
static void genSine(float* buf, int numFrames, float freq, float sr, float amp = 1.f) {
    for (int i = 0; i < numFrames; ++i)
        buf[i] = amp * std::sin(2.f * kPI * freq * static_cast<float>(i) / sr);
}

// Calcule le RMS d'un buffer.
static float computeRms(const float* buf, int n) {
    float sum = 0.f;
    for (int i = 0; i < n; ++i) sum += buf[i] * buf[i];
    return std::sqrt(sum / static_cast<float>(n));
}

// ─── T-C1 : MonoSubFilter est un vrai LP (passe-bas) ────────────────────────
// Le filtre doit laisser passer les basses fréquences (80 Hz) et atténuer
// les hautes fréquences (8 kHz). Le test échouerait avec l'ancien
// comportement (mono-sum inject) car le 8 kHz serait conservé.
TEST_CASE("T-C1: MonoSubFilter passes 80 Hz and attenuates 8 kHz", "[monosub]") {
    MonoSubFilter filter;
    constexpr float sr = 44100.f;
    filter.prepare(sr);

    constexpr int N = 4096;
    float L[N], R[N];

    // ── Test 1 : signal grave (80 Hz) — doit passer avec atténuation faible ──
    genSine(L, N, 80.f, sr);
    genSine(R, N, 80.f, sr);
    const float inRms80 = computeRms(L, N);

    filter.process(L, R, N);
    // Ignorer le transient (1er bloc)
    const float outRms80 = computeRms(L + 1024, N - 1024);

    // Le 80 Hz est sous fc (120 Hz) → doit passer avec atténuation < 6 dB
    const float ratio80 = outRms80 / std::max(inRms80, 1e-10f);
    REQUIRE(ratio80 > 0.4f);

    // ── Test 2 : signal aigu (8 kHz) — doit être fortement atténué ──────────
    filter.reset();
    genSine(L, N, 8000.f, sr);
    genSine(R, N, 8000.f, sr);
    const float inRms8k = computeRms(L, N);

    filter.process(L, R, N);
    const float outRms8k = computeRms(L + 1024, N - 1024);

    // Le 8 kHz est >> 120 Hz → atténuation > 20 dB (ratio < 0.1)
    const float ratio8k = outRms8k / std::max(inRms8k, 1e-10f);
    REQUIRE(ratio8k < 0.1f);

    // Le 8 kHz doit être PLUS atténué que le 80 Hz
    REQUIRE(ratio8k < ratio80);
}

// ─── T-C1b : vérifie le coefficient LP à deux sample rates ──────────────────
// Le filtre doit fonctionner correctement à 44.1 kHz et 48 kHz.
TEST_CASE("T-C1b: MonoSubFilter coefficient at different sample rates", "[monosub]") {
    constexpr int N = 4096;

    for (float sr : { 44100.f, 48000.f }) {
        MonoSubFilter filter;
        filter.prepare(sr);

        constexpr int N = 8192;
        float L[N], R[N];
        genSine(L, N, 8000.f, sr);
        genSine(R, N, 8000.f, sr);
        const float inRms = computeRms(L, N);

        // Laisser le filtre se stabiliser (1er bloc), mesurer sur le 2e
        filter.process(L, R, N);
        const float outRms = computeRms(L + N / 2, N / 2);
        const float ratio = outRms / std::max(inRms, 1e-10f);

        // À tout sample rate, le 8 kHz doit être atténué > 20 dB
        REQUIRE(ratio < 0.1f);
    }
}
