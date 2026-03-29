#include "WsolaShifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void WsolaShifter::computeHann() noexcept
{
    constexpr float kPi = 3.14159265358979f;
    for (int i = 0; i < kWindowSize; ++i)
    {
        hann_[static_cast<std::size_t>(i)] =
            0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i)
                                             / static_cast<float>(kWindowSize - 1)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void WsolaShifter::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    computeHann();
    reset();
}

void WsolaShifter::reset() noexcept
{
    ring_.fill(0.0f);
    accum_.fill(0.0f);
    tail_.fill(0.0f);

    // Pre-fill virtual silence: writeAbs_ starts kWindowSize+kDelta ahead of
    // analysisAbs_ so the very first grain has enough analysis headroom
    // without reading any real samples.
    writeAbs_    = kWindowSize + kDelta;
    analysisAbs_ = 0;
    outputAbs_   = 0;
    readAbs_     = 0;
}

void WsolaShifter::setShiftSemitones(float semitones) noexcept
{
    shiftRatio_ = semitonesToRatio(semitones);
}

// ─────────────────────────────────────────────────────────────────────────────
// findBestOffset
//
// Exhaustive search over all offsets d in [−kDelta, +kDelta].
// For each candidate, computes the dot-product between tail_ (last kTailSize
// output samples) and the analysis ring at (analysisAbs_ + d).
// The winning offset minimises the phase discontinuity at the grain boundary.
//
// Note: we use static_cast<unsigned> before the mask to avoid undefined
// behaviour on negative intermediate values on the very first grain.
// ─────────────────────────────────────────────────────────────────────────────
int WsolaShifter::findBestOffset() const noexcept
{
    float bestCorr   = -1.0e30f;
    int   bestOffset = 0;

    for (int d = -kDelta; d <= kDelta; ++d)
    {
        const int base = analysisAbs_ + d;
        float corr = 0.0f;

        for (int i = 0; i < kTailSize; ++i)
        {
            const auto idx =
                static_cast<std::size_t>(static_cast<unsigned>(base + i) & static_cast<unsigned>(kRingMask));
            corr += tail_[static_cast<std::size_t>(i)] * ring_[idx];
        }

        if (corr > bestCorr)
        {
            bestCorr   = corr;
            bestOffset = d;
        }
    }

    return bestOffset;
}

// ─────────────────────────────────────────────────────────────────────────────
// process
//
// Step 1: Push numSamples into the analysis ring buffer.
// Step 2: Generate WSOLA grains until the accumulation buffer covers
//         [readAbs_, readAbs_ + numSamples + kHopS).  The extra kHopS margin
//         ensures that every output sample is always covered by at least the
//         right half of a previously OLA'd grain (prevents amplitude tapering
//         at block boundaries).
// Step 3: Copy numSamples from the accumulation buffer to output and zero the
//         consumed region so it is ready for future OLA writes.
// ─────────────────────────────────────────────────────────────────────────────
void WsolaShifter::process(const float* input, float* output,
                            int numSamples, float /*inputPitchHz*/) noexcept
{
    // ── 1. Push input ────────────────────────────────────────────────────────
    for (int i = 0; i < numSamples; ++i)
    {
        const auto idx =
            static_cast<std::size_t>(static_cast<unsigned>(writeAbs_ + i) & static_cast<unsigned>(kRingMask));
        ring_[idx] = input[i];
    }
    writeAbs_ += numSamples;

    // ── 2. Generate grains ───────────────────────────────────────────────────
    const int readEnd = readAbs_ + numSamples;

    // hopA: how many input samples we advance per synthesis hop.
    // ratio > 1 (pitch up) → hopA > kHopS (consume input faster → time-compress → pitch up).
    // ratio < 1 (pitch down) → hopA < kHopS.
    const int hopA = std::max(1,
        std::min(static_cast<int>(static_cast<float>(kHopS) * shiftRatio_ + 0.5f),
                 kRingSize / 2));

    // Keep one kHopS ahead to guarantee full OLA coverage of every output sample.
    while (outputAbs_ < readEnd + kHopS)
    {
        // Check sufficient analysis headroom (kWindowSize + kDelta samples ahead)
        if (writeAbs_ - analysisAbs_ < kWindowSize + kDelta)
            break;  // not enough input yet — accum already has previous-grain overlap

        // Find best analysis offset
        const int d         = findBestOffset();
        const int grainBase = analysisAbs_ + d;

        // OLA: Hann-window the grain into accum
        for (int i = 0; i < kWindowSize; ++i)
        {
            const auto rIdx =
                static_cast<std::size_t>(static_cast<unsigned>(grainBase + i) & static_cast<unsigned>(kRingMask));
            const auto aIdx =
                static_cast<std::size_t>(static_cast<unsigned>(outputAbs_ + i) & static_cast<unsigned>(kAccumMask));
            accum_[aIdx] += ring_[rIdx] * hann_[static_cast<std::size_t>(i)];
        }

        // Update tail: last kTailSize Hann-weighted ring samples from this grain
        const int tailBase = grainBase + kWindowSize - kTailSize;
        for (int i = 0; i < kTailSize; ++i)
        {
            const auto rIdx =
                static_cast<std::size_t>(static_cast<unsigned>(tailBase + i) & static_cast<unsigned>(kRingMask));
            tail_[static_cast<std::size_t>(i)] =
                ring_[rIdx] * hann_[static_cast<std::size_t>(kWindowSize - kTailSize + i)];
        }

        // Advance synthesis and analysis positions
        outputAbs_   += kHopS;
        analysisAbs_ += hopA;
    }

    // ── 3. Read output + zero consumed region ────────────────────────────────
    for (int i = 0; i < numSamples; ++i)
    {
        const auto aIdx =
            static_cast<std::size_t>(static_cast<unsigned>(readAbs_ + i) & static_cast<unsigned>(kAccumMask));
        output[i]     = clipSample(accum_[aIdx]);
        accum_[aIdx]  = 0.0f;
    }

    readAbs_ += numSamples;
}

} // namespace dsp
