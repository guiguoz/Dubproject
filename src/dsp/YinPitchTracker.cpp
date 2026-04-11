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
    // minFrequency_ default 85 Hz (alto/ténor sax) → period ≈ 565 samples @ 48 kHz
    // → windowSize ≈ 1130 samples. Half = 565 = 1 period in the difference function.
    const int minPeriod = static_cast<int>(sampleRate / static_cast<double>(minFrequency_)) + 1;
    windowSize_         = minPeriod * 2;

    // Hop size: ~4ms, min 128 samples. Controls analysis cadence.
    hopSize_ = std::max(128, static_cast<int>(sampleRate * 0.004));

    // DC blocker coefficient: HPF ~40 Hz, adapted to sample rate.
    // R = exp(-2π·fc/fs). At 48 kHz: R ≈ 0.9948. At 44.1 kHz: R ≈ 0.9943.
    constexpr float kDcBlockerCutoffHz = 40.0f;
    hpfR_ = std::exp(-2.0f * 3.14159265f * kDcBlockerCutoffHz / static_cast<float>(sampleRate));

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

    hpfPrevIn_  = 0.0f;
    hpfPrevOut_ = 0.0f;
    historyIndex_ = 0;
    historyPrimed_ = false;
    samplesSinceLastAnalysis_ = 0;
    std::fill(pitchHistory_.begin(), pitchHistory_.end(), 0.0f);
}

void YinPitchTracker::setThreshold(float threshold) noexcept
{
    threshold_ = std::max(0.08f, std::min(0.25f, threshold));
}

void YinPitchTracker::setMinFrequency(float hz) noexcept
{
    minFrequency_ = std::max(40.0f, std::min(200.0f, hz));
}

// ─────────────────────────────────────────────────────────────────────────────
// process  (v2)
//
// 1. Accumulate with DC blocker (~40 Hz)
// 2. At most 1 YIN analysis per call (CPU-safe)
// 3. Median filter on voiced detections only (zeros never pollute history)
// ─────────────────────────────────────────────────────────────────────────────
PitchResult YinPitchTracker::process(const float* input, int numSamples) noexcept
{
    // 1. Accumulate incoming samples with DC blocker
    //    y[n] = x[n] - x[n-1] + R * y[n-1]   (R ≈ 0.995 for ~40 Hz @ 48 kHz)
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = input[i];
        const float y = x - hpfPrevIn_ + hpfR_ * hpfPrevOut_;
        hpfPrevIn_  = x;
        hpfPrevOut_ = y;

        inputAccumulator_[static_cast<std::size_t>(accWritePos_)] = y;
        accWritePos_ = (accWritePos_ + 1) % windowSize_;
        if (accFilled_ < windowSize_)
            ++accFilled_;
    }

    samplesSinceLastAnalysis_ += numSamples;

    // 2. At most 1 YIN analysis per process() call.
    //    Prevents CPU spikes from multiple O(n²) differenceFunction calls.
    if (accFilled_ < windowSize_ || samplesSinceLastAnalysis_ < hopSize_)
        return lastResult_;

    samplesSinceLastAnalysis_ -= hopSize_;   // preserve remainder (no cadence drift)

    differenceFunction(windowSize_);
    cumulativeMeanNormalize(windowSize_);
    const int tau = absoluteThreshold(windowSize_);

    if (tau != -1)
    {
        const float refinedTau = parabolicInterpolation(tau, windowSize_);
        const float frequency  = static_cast<float>(sampleRate_) / refinedTau;

        if (frequency >= minFrequency_ && frequency <= kMaxFrequencyHz)
        {
            const float confidence = 1.0f - yinBuffer_[static_cast<std::size_t>(tau)];
            lastResult_ = { frequency, std::max(0.0f, std::min(1.0f, confidence)) };

            // 3. Median filter — only update on valid voiced detections.
            //    Zeros never enter the history → no false "dropout to 0 Hz".
            if (confidence > 0.65f)
            {
                // Warm-up: fill entire history on first valid pitch to avoid
                // initial median = 0 from the zeroed array.
                if (!historyPrimed_)
                {
                    pitchHistory_.fill(frequency);
                    historyPrimed_ = true;
                }

                pitchHistory_[static_cast<std::size_t>(historyIndex_)] = frequency;
                historyIndex_ = (historyIndex_ + 1) % 5;

                std::array<float, 5> sorted = pitchHistory_;
                std::sort(sorted.begin(), sorted.end());
                lastResult_.frequencyHz = sorted[2];   // median
            }

            return lastResult_;
        }
    }

    // Unvoiced — no fallback, no invented pitch.
    lastResult_ = { 0.0f, 0.0f };
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
//
// v2: removed dangerous "global minimum" fallback that invented pitch in noise.
// ─────────────────────────────────────────────────────────────────────────────
int YinPitchTracker::absoluteThreshold(int windowSize) const noexcept
{
    const int half     = windowSize / 2;
    const int minTau   = static_cast<int>(sampleRate_ / kMaxFrequencyHz);
    const int maxTau   = static_cast<int>(sampleRate_ / static_cast<double>(minFrequency_)) + 1;
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

    return -1;
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
