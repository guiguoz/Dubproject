#pragma once

#include "engine/Analysis/KeyResult.h"
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace engine::analysis {

// ─────────────────────────────────────────────────────────────────────────────
// KeyDetector — Krumhansl-Schmuckler key detection from mono PCM.
// Runs offline (worker thread only — do NOT call from the audio thread).
// ─────────────────────────────────────────────────────────────────────────────
class KeyDetector
{
public:
    // Returns KeyResult with key=-1 if signal is too short or silent.
    // result.confidence is the raw Pearson correlation of the winning key [0, 1].
    // Threshold for reliable detection: confidence >= 0.7.
    static KeyResult detect(const float* pcm, int numSamples, double sr) noexcept;

private:
    // Goertzel power at frequency f (Hz) — O(N), no FFT needed.
    static float goertzel(const float* x, int n, double f, double sr) noexcept
    {
        const double omega = 6.28318530717959 * f / sr;
        const float  coeff = 2.f * static_cast<float>(std::cos(omega));
        float s1 = 0.f, s2 = 0.f;
        for (int i = 0; i < n; ++i)
        {
            const float s = x[i] + coeff * s1 - s2;
            s2 = s1; s1 = s;
        }
        return s1 * s1 + s2 * s2 - coeff * s1 * s2;
    }

    // Pearson correlation of chromagram x with K-S profile rotated by `shift` semitones.
    static float pearson(const std::array<float, 12>& x,
                         const std::array<float, 12>& profile,
                         int shift) noexcept
    {
        float xm = 0.f, pm = 0.f;
        for (int i = 0; i < 12; ++i) { xm += x[i]; pm += profile[i]; }
        xm /= 12.f; pm /= 12.f;
        float num = 0.f, dx2 = 0.f, dp2 = 0.f;
        for (int i = 0; i < 12; ++i)
        {
            const float xi = x[i] - xm;
            const float pi = profile[(i - shift + 12) % 12] - pm;
            num += xi * pi;
            dx2 += xi * xi;
            dp2 += pi * pi;
        }
        const float denom = std::sqrt(dx2 * dp2);
        return denom > 1e-9f ? num / denom : 0.f;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
inline KeyResult KeyDetector::detect(const float* pcm, int numSamples, double sr) noexcept
{
    // Need at least 1 s of audio for reliable chroma estimation.
    if (!pcm || numSamples < static_cast<int>(sr) || sr <= 0.0) return {};

    // Cap at 10 s to keep processing time bounded (~40 ms on a modern CPU).
    const int n = std::min(numSamples, static_cast<int>(sr * 10.0));

    // Krumhansl-Schmuckler 1990 profiles (major and minor).
    static constexpr std::array<float, 12> kMajor {
        6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
        2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static constexpr std::array<float, 12> kMinor {
        6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
        2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    // Build chromagram: sum Goertzel energy per pitch class over MIDI octaves 3–7 (C3–B7).
    std::array<float, 12> chroma {};
    for (int midi = 36; midi <= 95; ++midi)
    {
        const double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        if (freq >= sr * 0.49) continue;  // skip if above Nyquist
        chroma[static_cast<std::size_t>(midi % 12)] += goertzel(pcm, n, freq, sr);
    }

    // Normalize chromagram.
    float chromaSum = 0.f;
    for (float c : chroma) chromaSum += c;
    if (chromaSum < 1e-9f) return {};  // silence — detection impossible
    for (float& c : chroma) c /= chromaSum;

    // Find best key (24 combinations: 12 roots × 2 modes).
    int   bestKey  = -1, bestMode = 0;
    float bestCorr = -2.f;
    for (int key = 0; key < 12; ++key)
    {
        const float majC = pearson(chroma, kMajor, key);
        const float minC = pearson(chroma, kMinor, key);
        if (majC > bestCorr) { bestCorr = majC; bestKey = key; bestMode = 0; }
        if (minC > bestCorr) { bestCorr = minC; bestKey = key; bestMode = 1; }
    }

    if (bestKey < 0) return {};

    KeyResult result;
    result.key        = bestKey;
    result.mode       = bestMode;
    result.confidence = std::max(0.f, bestCorr);  // raw Pearson [0,1]; threshold: 0.7

    // Fill diatonic scale degrees.
    static constexpr std::array<int, 7> kMajDeg { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr std::array<int, 7> kMinDeg { 0, 2, 3, 5, 7, 8, 10 };
    const auto& deg = (bestMode == 0) ? kMajDeg : kMinDeg;
    for (int i = 0; i < 7; ++i)
        result.scaleDegrees[static_cast<std::size_t>(i)] = (bestKey + deg[i]) % 12;

    return result;
}

} // namespace engine::analysis
