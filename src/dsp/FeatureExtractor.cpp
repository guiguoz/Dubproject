#include "FeatureExtractor.h"

#include <algorithm>
#include <cmath>

namespace dsp {

static constexpr float kTwoPi = 6.283185307f;

// Analysis frequencies for Goertzel-based spectral centroid.
// 440 Hz is explicitly included so a pure A4 sine maps centroid ≈ 440 Hz.
static constexpr float kCentroidFreqs[] = {
     50.f,  100.f,  200.f,  300.f,  400.f,  440.f,
    500.f,  600.f,  750.f, 1000.f, 1500.f, 2000.f,
   3000.f, 4000.f, 6000.f, 8000.f,
  10000.f, 12000.f, 16000.f, 20000.f
};
static constexpr int kNumCentroidFreqs =
    static_cast<int>(sizeof(kCentroidFreqs) / sizeof(float));

// ─────────────────────────────────────────────────────────────────────────────

float FeatureExtractor::goertzelEnergy(const std::vector<float>& pcm,
                                        double sampleRate, float freqHz) noexcept
{
    if (pcm.empty() || freqHz <= 0.f
            || freqHz >= static_cast<float>(sampleRate) * 0.5f)
        return 0.f;

    const float omega = kTwoPi * freqHz / static_cast<float>(sampleRate);
    const float coeff = 2.f * std::cos(omega);
    float s1 = 0.f, s2 = 0.f;

    // Cap at 8192 samples — sufficient for ≥1 Hz resolution, avoids large accum.
    const std::size_t nMax = std::min(pcm.size(), std::size_t{8192});
    for (std::size_t i = 0; i < nMax; ++i)
    {
        const float s0 = pcm[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    // Goertzel power: |X(f)|² ≈ s1² + s2² − coeff·s1·s2
    const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power < 0.f ? 0.f : power;  // numerical guard
}

float FeatureExtractor::computeSpectralCentroid(const std::vector<float>& pcm,
                                                 double sampleRate) noexcept
{
    float totalEnergy = 0.f;
    float weightedSum = 0.f;

    for (int i = 0; i < kNumCentroidFreqs; ++i)
    {
        const float f = kCentroidFreqs[i];
        const float e = goertzelEnergy(pcm, sampleRate, f);
        totalEnergy  += e;
        weightedSum  += f * e;
    }

    if (totalEnergy < 1e-10f) return 0.f;
    return weightedSum / totalEnergy;
}

ContentCategory FeatureExtractor::classifyHeuristic(float transientRatio,
                                                     float lowFrac, float midFrac,
                                                     float highFrac,
                                                     float durationMs) noexcept
{
    if (transientRatio > 3.0f && lowFrac  > 0.28f && durationMs < 600.f)
        return ContentCategory::KICK;
    if (transientRatio > 2.5f && highFrac > 0.40f && durationMs < 400.f)
        return ContentCategory::HIHAT;
    if (transientRatio > 4.5f && highFrac > 0.30f && durationMs < 500.f)
        return ContentCategory::HIHAT;
    if (transientRatio > 2.5f && midFrac  > 0.28f && durationMs < 700.f)
        return ContentCategory::SNARE;
    if (transientRatio < 2.0f && lowFrac  > 0.50f)
        return ContentCategory::BASS;
    if (transientRatio < 1.8f && durationMs > 1500.f && highFrac < 0.35f)
        return ContentCategory::PAD;
    if (transientRatio > 2.5f)
        return ContentCategory::PERC;
    if (transientRatio < 2.0f && midFrac > 0.35f)
        return ContentCategory::SYNTH;
    return ContentCategory::OTHER;
}

MixFeatures FeatureExtractor::extract(const std::vector<float>& pcm,
                                       double sampleRate) noexcept
{
    MixFeatures feat;
    if (pcm.empty() || sampleRate <= 0.0)
        return feat;

    const float fs = static_cast<float>(sampleRate);
    const int N = static_cast<int>(pcm.size());

    // ── RMS + peak ────────────────────────────────────────────────────────────
    float sumSq = 0.f, peak = 0.f;
    for (float x : pcm)
    {
        sumSq += x * x;
        const float ax = std::abs(x);
        if (ax > peak) peak = ax;
    }
    feat.rms         = std::sqrt(sumSq / static_cast<float>(N));
    feat.crestFactor = (feat.rms > 1e-8f) ? peak / feat.rms : 1.f;

    // ── Duration ──────────────────────────────────────────────────────────────
    feat.durationMs = static_cast<float>(N) / fs * 1000.f;

    // ── Spectral centroid ─────────────────────────────────────────────────────
    feat.spectralCentroid = computeSpectralCentroid(pcm, sampleRate);

    // ── Band energy fractions (1st-order LP cascade, max 1 s) ─────────────────
    // Low  : 0 – ~500 Hz
    // Mid  : ~500 – ~4000 Hz
    // High : ~4000 Hz – Nyquist
    const auto alphaFor = [fs](float fc) noexcept
    {
        const float t = kTwoPi * fc / fs;
        return t / (t + 1.f);
    };
    const float a500  = alphaFor(500.f);
    const float a4000 = alphaFor(4000.f);

    float y500 = 0.f, y4000 = 0.f;
    float eLow = 0.f, eMid = 0.f, eHigh = 0.f, eTotal = 0.f;
    const int nBand = std::min(N, static_cast<int>(sampleRate));
    for (int i = 0; i < nBand; ++i)
    {
        const float x = pcm[static_cast<std::size_t>(i)];
        y500  = a500  * x + (1.f - a500)  * y500;
        y4000 = a4000 * x + (1.f - a4000) * y4000;
        eLow   += y500  * y500;
        eMid   += (y4000 - y500)  * (y4000 - y500);
        eHigh  += (x    - y4000)  * (x    - y4000);
        eTotal += x * x;
    }
    if (eTotal > 1e-8f)
    {
        feat.lowFrac  = eLow  / eTotal;
        feat.midFrac  = eMid  / eTotal;
        feat.highFrac = eHigh / eTotal;
    }

    // ── Transient ratio (peak/RMS of first 20 ms) ─────────────────────────────
    const int attackN = std::min(N, static_cast<int>(fs * 0.020f));
    float peakAtt = 0.f, sumSqAtt = 0.f;
    for (int i = 0; i < attackN; ++i)
    {
        const float ax = std::abs(pcm[static_cast<std::size_t>(i)]);
        if (ax > peakAtt) peakAtt = ax;
        sumSqAtt += pcm[static_cast<std::size_t>(i)] * pcm[static_cast<std::size_t>(i)];
    }
    const float rmsAtt = std::sqrt(sumSqAtt / static_cast<float>(std::max(attackN, 1)));
    const float transientRatio = (rmsAtt > 0.001f) ? peakAtt / rmsAtt : 1.0f;

    // ── Heuristic content type ────────────────────────────────────────────────
    feat.contentType = classifyHeuristic(
        transientRatio, feat.lowFrac, feat.midFrac, feat.highFrac, feat.durationMs);

    return feat;
}

} // namespace dsp
