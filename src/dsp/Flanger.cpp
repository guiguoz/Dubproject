#include "Flanger.h"
#include "DspCommon.h"

#include <cmath>

namespace dsp {

static constexpr float kTwoPi = 6.28318530717958647f;

void Flanger::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    delayLine_.setSize(kMaxDelaySamples);
    reset();
}

void Flanger::reset() noexcept
{
    delayLine_.reset();
    lfoPhase_            = 0.0f;
    lastFeedbackSample_  = 0.0f;
}

void Flanger::setRate(float hz) noexcept
{
    rate_.store(hz, std::memory_order_relaxed);
}

void Flanger::setDepth(float depth) noexcept
{
    depth_.store(depth, std::memory_order_relaxed);
}

void Flanger::setFeedback(float feedback) noexcept
{
    // Clamp to safe range to prevent runaway feedback
    const float clamped = (feedback > 0.95f) ? 0.95f : (feedback < -0.95f) ? -0.95f : feedback;
    feedback_.store(clamped, std::memory_order_relaxed);
}

void Flanger::setMix(float mix) noexcept
{
    mix_.store(mix, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// process
// ─────────────────────────────────────────────────────────────────────────────
void Flanger::process(float* buffer, int numSamples) noexcept
{
    // Snapshot parameters once per block (relaxed loads — safe for floats)
    const float rate     = rate_.load(std::memory_order_relaxed);
    const float depth    = depth_.load(std::memory_order_relaxed);
    const float feedback = feedback_.load(std::memory_order_relaxed);
    const float mix      = mix_.load(std::memory_order_relaxed);
    const float dryGain  = 1.0f - mix;

    // Convert min/max delay from ms to samples
    const float minDelay = kMinDelayMs * static_cast<float>(sampleRate_) / 1000.0f;
    const float maxDelay = kMaxDelayMs * static_cast<float>(sampleRate_) / 1000.0f;
    const float halfSwing = (maxDelay - minDelay) * 0.5f * depth;
    const float midDelay  = minDelay + (maxDelay - minDelay) * 0.5f;

    const float lfoIncrement = rate * kTwoPi / static_cast<float>(sampleRate_);

    for (int i = 0; i < numSamples; ++i)
    {
        // LFO → delay modulation
        const float lfoVal     = std::sin(lfoPhase_);
        const float delaySamps = midDelay + halfSwing * lfoVal;

        lfoPhase_ += lfoIncrement;
        if (lfoPhase_ >= kTwoPi)
            lfoPhase_ -= kTwoPi;

        // Read delayed sample (with feedback applied at input)
        const float drySample  = buffer[i];
        const float inputSample = drySample + feedback * lastFeedbackSample_;

        delayLine_.push(inputSample);
        const float wetSample = delayLine_.read(delaySamps);

        lastFeedbackSample_ = wetSample;

        // Mix and clip
        buffer[i] = clipSample(dryGain * drySample + mix * wetSample);
    }
}

} // namespace dsp
