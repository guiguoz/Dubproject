#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace test_helpers {

// ─────────────────────────────────────────────────────────────────────────────
// Signal generators
// ─────────────────────────────────────────────────────────────────────────────

/// Generate a sine wave buffer at `freqHz` Hz.
inline std::vector<float> generateSine(float freqHz,
                                        float sampleRate,
                                        int   numSamples,
                                        float amplitude = 0.8f)
{
    std::vector<float> buf(static_cast<std::size_t>(numSamples));
    const float angularFreq = 2.0f * 3.14159265358979f * freqHz / sampleRate;
    for (int i = 0; i < numSamples; ++i)
        buf[static_cast<std::size_t>(i)] = amplitude * std::sin(angularFreq * static_cast<float>(i));
    return buf;
}

/// Generate a silent (zero) buffer.
inline std::vector<float> generateSilence(int numSamples)
{
    return std::vector<float>(static_cast<std::size_t>(numSamples), 0.0f);
}

/// Generate a sine wave with a DC offset added.
inline std::vector<float> generateSineWithDcOffset(float freqHz,
                                                     float sampleRate,
                                                     int   numSamples,
                                                     float dcOffset,
                                                     float amplitude = 0.8f)
{
    auto buf = generateSine(freqHz, sampleRate, numSamples, amplitude);
    for (auto& s : buf)
        s += dcOffset;
    return buf;
}

/// Generate white noise (deterministic seed for reproducibility).
inline std::vector<float> generateNoise(int numSamples, float amplitude = 0.5f)
{
    std::vector<float> buf(static_cast<std::size_t>(numSamples));
    unsigned int seed = 12345u;
    for (int i = 0; i < numSamples; ++i)
    {
        // Simple LCG pseudo-random (deterministic, no stdlib rand)
        seed = seed * 1664525u + 1013904223u;
        const float r = static_cast<float>(seed) / static_cast<float>(0xFFFFFFFFu);
        buf[static_cast<std::size_t>(i)] = (r * 2.0f - 1.0f) * amplitude;
    }
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Measurement
// ─────────────────────────────────────────────────────────────────────────────

/// Compute RMS level of a buffer.
inline float computeRms(const float* buffer, int numSamples)
{
    float sum = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        sum += buffer[i] * buffer[i];
    return std::sqrt(sum / static_cast<float>(numSamples));
}

/// Find dominant frequency via autocorrelation peak search.
/// Good enough for test validation of pitch shift results.
inline float findDominantFrequency(const float* buffer, int numSamples, float sampleRate)
{
    // Autocorrelation from lag 1 to numSamples/2
    const int maxLag = numSamples / 2;
    float     bestCorr = -1.0f;
    int       bestLag  = 1;

    for (int lag = 1; lag < maxLag; ++lag)
    {
        float corr = 0.0f;
        for (int i = 0; i < numSamples - lag; ++i)
            corr += buffer[i] * buffer[i + lag];

        if (corr > bestCorr)
        {
            bestCorr = corr;
            bestLag  = lag;
        }
    }
    return sampleRate / static_cast<float>(bestLag);
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────

/// Check all samples are within [minVal, maxVal].
inline bool allSamplesInRange(const float* buffer, int numSamples,
                               float minVal = -1.0f, float maxVal = 1.0f)
{
    for (int i = 0; i < numSamples; ++i)
        if (buffer[i] < minVal || buffer[i] > maxVal)
            return false;
    return true;
}

/// Check two buffers are identical (bit-exact).
inline bool buffersEqual(const float* a, const float* b, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

} // namespace test_helpers
