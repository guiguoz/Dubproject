#include "YinPitchTracker.h"
#include "DspCommon.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// prepare
// ─────────────────────────────────────────────────────────────────────────────
void YinPitchTracker::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;

    // Window size = 2 × longest period to detect.
    // kMinFrequencyHz = 60 Hz (baritone sax) → period ≈ 800 samples @ 48 kHz
    // → windowSize ≈ 1600 samples (auto-sized from kMinFrequencyHz).
    const int minPeriod = static_cast<int>(sampleRate / kMinFrequencyHz) + 1;
    windowSize_         = minPeriod * 2;

    yinBuffer_.assign(static_cast<std::size_t>(windowSize_ / 2), 0.0f);
    inputAccumulator_.assign(static_cast<std::size_t>(windowSize_), 0.0f);

    reset();
}

void YinPitchTracker::reset() noexcept
{
    accWritePos_ = 0;
    accFilled_   = 0;
    lastResult_  = { 0.0f, 0.0f };
    std::fill(inputAccumulator_.begin(), inputAccumulator_.end(), 0.0f);
    std::fill(yinBuffer_.begin(), yinBuffer_.end(), 0.0f);
}

void YinPitchTracker::setThreshold(float threshold) noexcept
{
    threshold_ = threshold;
}

// ─────────────────────────────────────────────────────────────────────────────
// process
// ─────────────────────────────────────────────────────────────────────────────
PitchResult YinPitchTracker::process(const float* input, int numSamples) noexcept
{
    // Accumulate incoming samples into the ring buffer
    for (int i = 0; i < numSamples; ++i)
    {
        inputAccumulator_[static_cast<std::size_t>(accWritePos_)] = input[i];
        accWritePos_ = (accWritePos_ + 1) % windowSize_;
        if (accFilled_ < windowSize_)
            ++accFilled_;
    }

    // Not enough data yet — return last known result
    if (accFilled_ < windowSize_)
        return lastResult_;

    // Build a linear (non-ring) view for the YIN analysis.
    // We copy the accumulator starting from the oldest sample.
    // This is the only copy — still realtime-safe (no allocation, fixed size).
    const int   W    = windowSize_;

    // Use yinBuffer_ as temporary aligned input copy reusing its memory.
    // We need W floats; yinBuffer_ has W/2. Use inputAccumulator_ itself:
    // since accWritePos_ is the oldest position, read W samples from there.
    // We'll work directly on inputAccumulator_ (circular) inside the sub-steps.

    differenceFunction(W);
    cumulativeMeanNormalize(W);

    const int tau = absoluteThreshold(W);

    if (tau == -1)
    {
        // No pitch detected below threshold
        lastResult_ = { 0.0f, 0.0f };
        return lastResult_;
    }

    const float refinedTau = parabolicInterpolation(tau, W);
    const float frequency  = static_cast<float>(sampleRate_) / refinedTau;

    if (frequency < kMinFrequencyHz || frequency > kMaxFrequencyHz)
    {
        lastResult_ = { 0.0f, 0.0f };
        return lastResult_;
    }

    const float confidence = 1.0f - yinBuffer_[static_cast<std::size_t>(tau)];
    lastResult_ = { frequency, std::max(0.0f, std::min(1.0f, confidence)) };
    return lastResult_;
}

// ─────────────────────────────────────────────────────────────────────────────
// YIN Step 2 — Difference function
// d(tau) = sum_{j=0}^{W/2-1} (x[W/2+j] - x[W/2+j - tau])^2
// Reads from inputAccumulator_ (circular, oldest at accWritePos_).
// Anchors on the newest samples (W/2 to W-1) and looks backwards by tau.
// This reduces detection latency by W/2 compared to looking forwards.
// Writes result into yinBuffer_.
// ─────────────────────────────────────────────────────────────────────────────
void YinPitchTracker::differenceFunction(int windowSize) noexcept
{
    const int half = windowSize / 2;
    const int W    = windowSize;

    auto circRead = [&](int i) -> float {
        int idx = (accWritePos_ + i) % W;
        return inputAccumulator_[static_cast<std::size_t>(idx)];
    };

    for (int tau = 0; tau < half; ++tau)
    {
        float sum = 0.0f;
        for (int j = 0; j < half; ++j)
        {
            const float delta = circRead(half + j) - circRead(half + j - tau);
            sum += delta * delta;
        }
        yinBuffer_[static_cast<std::size_t>(tau)] = sum;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// YIN Step 3 — Cumulative Mean Normalized Difference (CMND)
// d'(tau) = 1                          for tau == 0
//         = d(tau) / (1/tau * sum_{j=1}^{tau} d(j))  for tau > 0
// ─────────────────────────────────────────────────────────────────────────────
void YinPitchTracker::cumulativeMeanNormalize(int windowSize) noexcept
{
    const int half = windowSize / 2;
    yinBuffer_[0]  = 1.0f;

    float runningSum = 0.0f;
    for (int tau = 1; tau < half; ++tau)
    {
        runningSum += yinBuffer_[static_cast<std::size_t>(tau)];
        if (runningSum > 0.0f)
            yinBuffer_[static_cast<std::size_t>(tau)] *= static_cast<float>(tau) / runningSum;
        else
            yinBuffer_[static_cast<std::size_t>(tau)] = 1.0f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// YIN Step 4 — Absolute threshold
// Returns first tau where d'(tau) < threshold_ (after the first dip).
// Returns -1 if none found (unvoiced).
// ─────────────────────────────────────────────────────────────────────────────
int YinPitchTracker::absoluteThreshold(int windowSize) const noexcept
{
    const int half     = windowSize / 2;
    const int minTau   = static_cast<int>(sampleRate_ / kMaxFrequencyHz);
    const int maxTau   = static_cast<int>(static_cast<double>(sampleRate_) / kMinFrequencyHz) + 1;
    const int clampMax = std::min(half - 1, maxTau);

    for (int tau = std::max(2, minTau); tau < clampMax; ++tau)
    {
        if (yinBuffer_[static_cast<std::size_t>(tau)] < threshold_)
        {
            // Find the local minimum after this first dip
            while (tau + 1 < clampMax &&
                   yinBuffer_[static_cast<std::size_t>(tau + 1)] < yinBuffer_[static_cast<std::size_t>(tau)])
            {
                ++tau;
            }
            return tau;
        }
    }

    // No dip below threshold — return the global minimum as a fallback
    int bestTau = std::max(2, minTau);
    float bestVal = yinBuffer_[static_cast<std::size_t>(bestTau)];
    for (int tau = bestTau + 1; tau < clampMax; ++tau)
    {
        if (yinBuffer_[static_cast<std::size_t>(tau)] < bestVal)
        {
            bestVal = yinBuffer_[static_cast<std::size_t>(tau)];
            bestTau = tau;
        }
    }
    // Only accept if reasonably close to threshold
    return (bestVal < threshold_ * 2.0f) ? bestTau : -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// YIN Step 5 — Parabolic interpolation for sub-sample accuracy
// ─────────────────────────────────────────────────────────────────────────────
float YinPitchTracker::parabolicInterpolation(int tau, int windowSize) const noexcept
{
    const int half = windowSize / 2;
    if (tau <= 0 || tau >= half - 1)
        return static_cast<float>(tau);

    const float s0 = yinBuffer_[static_cast<std::size_t>(tau - 1)];
    const float s1 = yinBuffer_[static_cast<std::size_t>(tau)];
    const float s2 = yinBuffer_[static_cast<std::size_t>(tau + 1)];

    const float denom = 2.0f * (2.0f * s1 - s0 - s2);
    if (std::abs(denom) < 1e-6f)
        return static_cast<float>(tau);

    return static_cast<float>(tau) + (s0 - s2) / denom;
}

} // namespace dsp
