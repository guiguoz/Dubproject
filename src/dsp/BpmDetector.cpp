#include "BpmDetector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp
{

void BpmDetector::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
}

void BpmDetector::process(const float* buf, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    // Block RMS
    float sumSq = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        sumSq += buf[i] * buf[i];
    const float rms = std::sqrt(sumSq / static_cast<float>(numSamples));

    // Positive RMS derivative above threshold → onset
    if ((rms - prevRms_) > threshold_ && rms > threshold_)
        onOnset();

    prevRms_       = rms;
    currentSample_ += static_cast<double>(numSamples);
}

void BpmDetector::onOnset() noexcept
{
    if (lastOnsetSample_ >= 0.0)
    {
        const double intervalSamples = currentSample_ - lastOnsetSample_;
        const float  intervalSecs    = static_cast<float>(intervalSamples / sampleRate_);
        const float  candidate       = 60.0f / intervalSecs;

        if (candidate >= kMinBpm && candidate <= kMaxBpm)
        {
            ioi_[ioiHead_] = intervalSecs;
            ioiHead_        = (ioiHead_ + 1) % kIoiSize;
            if (ioiCount_ < kIoiSize)
                ++ioiCount_;

            const float est = estimateFromIoi();
            if (est > 0.0f)
                bpm_.store(medianFilter(est), std::memory_order_relaxed);
        }
    }
    lastOnsetSample_ = currentSample_;
}

float BpmDetector::estimateFromIoi() noexcept
{
    if (ioiCount_ == 0)
        return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < ioiCount_; ++i)
        sum += ioi_[i];
    return 60.0f / (sum / static_cast<float>(ioiCount_));
}

float BpmDetector::medianFilter(float newBpm) noexcept
{
    hist_[histHead_] = newBpm;
    histHead_        = (histHead_ + 1) % kHistSize;
    if (histCount_ < kHistSize)
        ++histCount_;

    std::array<float, kHistSize> sorted{};
    for (int i = 0; i < histCount_; ++i)
        sorted[i] = hist_[i];
    std::sort(sorted.begin(), sorted.begin() + histCount_);
    return sorted[histCount_ / 2];
}

float BpmDetector::getBpm() const noexcept
{
    return bpm_.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// detectOffline
//
// Autocorrelation-based BPM detector for offline (file) analysis.
//
// 1. Downsample audio to an RMS envelope at ~50 ms hops.
// 2. Compute normalised autocorrelation over lags spanning 40–240 BPM.
// 3. Pick the lag with the highest correlation.
// 4. Apply octave normalisation to land in 60–180 BPM.
// ─────────────────────────────────────────────────────────────────────────────
float BpmDetector::detectOffline(const float* data, int numSamples,
                                  double sampleRate) noexcept
{
    if (!data || numSamples <= 0 || sampleRate <= 0.0)
        return kDefaultBpm;

    // ── Step 1: RMS envelope at ~50 ms hops ─────────────────────────────────
    const int hopSize  = std::max(1, static_cast<int>(sampleRate * 0.05));
    const int nFrames  = numSamples / hopSize;
    if (nFrames < 8) return kDefaultBpm;

    std::vector<float> env(static_cast<std::size_t>(nFrames));
    for (int f = 0; f < nFrames; ++f)
    {
        float sumSq = 0.f;
        const int start = f * hopSize;
        const int end   = std::min(start + hopSize, numSamples);
        for (int i = start; i < end; ++i)
            sumSq += data[i] * data[i];
        env[static_cast<std::size_t>(f)] =
            std::sqrt(sumSq / static_cast<float>(end - start));
    }

    // ── Step 2: Autocorrelation over BPM lag range ───────────────────────────
    const float hopSecs = static_cast<float>(hopSize) / static_cast<float>(sampleRate);
    const int lagMin = std::max(1, static_cast<int>(60.f / kMaxBpm / hopSecs));
    const int lagMax = std::min(nFrames / 2,
                                static_cast<int>(60.f / kMinBpm / hopSecs) + 1);
    if (lagMin >= lagMax) return kDefaultBpm;

    // Mean of envelope (for normalisation)
    float mean = 0.f;
    for (float v : env) mean += v;
    mean /= static_cast<float>(nFrames);

    float bestCorr = -1.f;
    int   bestLag  = lagMin;

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        float corr = 0.f;
        int   cnt  = 0;
        for (int i = 0; i + lag < nFrames; ++i)
        {
            corr += (env[static_cast<std::size_t>(i)]       - mean)
                  * (env[static_cast<std::size_t>(i + lag)] - mean);
            ++cnt;
        }
        if (cnt > 0) corr /= static_cast<float>(cnt);

        // Reward lags whose harmonics (×2, ×3) also show correlation
        float score = corr;
        if (lag * 2 <= lagMax)
        {
            float c2 = 0.f; int n2 = 0;
            for (int i = 0; i + lag * 2 < nFrames; ++i)
            {
                c2 += (env[static_cast<std::size_t>(i)]           - mean)
                    * (env[static_cast<std::size_t>(i + lag * 2)] - mean);
                ++n2;
            }
            if (n2 > 0) score += 0.5f * c2 / static_cast<float>(n2);
        }

        if (score > bestCorr)
        {
            bestCorr = score;
            bestLag  = lag;
        }
    }

    // ── Step 3: Lag → BPM + octave normalisation into 60–180 BPM ────────────
    float bpm = 60.f / (static_cast<float>(bestLag) * hopSecs);
    while (bpm > 180.f) bpm *= 0.5f;
    while (bpm <  60.f) bpm *= 2.0f;

    return (bpm >= kMinBpm && bpm <= kMaxBpm) ? bpm : kDefaultBpm;
}

void BpmDetector::reset() noexcept
{
    currentSample_   = 0.0;
    lastOnsetSample_ = -1.0;
    prevRms_         = 0.0f;
    ioi_.fill(0.0f);
    ioiHead_  = 0;
    ioiCount_ = 0;
    hist_.fill(kDefaultBpm);
    histHead_  = 0;
    histCount_ = 0;
    bpm_.store(kDefaultBpm, std::memory_order_relaxed);
}

} // namespace dsp
