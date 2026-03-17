#pragma once

#include "RingBuffer.h"
#include <atomic>

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// Flanger
//
// Variable-delay comb filter driven by a sine LFO.
// All parameters are written via std::atomic (GUI thread → audio thread safe).
// ─────────────────────────────────────────────────────────────────────────────
class Flanger
{
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    /// LFO rate in Hz. Range: [0.05, 10.0]. Default: 0.5 Hz.
    void setRate(float hz) noexcept;

    /// Modulation depth [0.0, 1.0]. Default: 0.7.
    void setDepth(float depth) noexcept;

    /// Feedback coefficient [-0.95, 0.95]. Default: 0.3.
    void setFeedback(float feedback) noexcept;

    /// Dry/wet mix [0.0 = dry, 1.0 = wet]. Default: 0.5.
    void setMix(float mix) noexcept;

    /// Process a mono block in-place.  Realtime-safe.
    void process(float* buffer, int numSamples) noexcept;

    void reset() noexcept;

private:
    double sampleRate_ { 44100.0 };

    // LFO state (audio thread only — not atomic)
    float lfoPhase_  { 0.0f };

    // Parameters — written from GUI thread, read from audio thread
    std::atomic<float> rate_     { 0.5f };
    std::atomic<float> depth_    { 0.7f };
    std::atomic<float> feedback_ { 0.3f };
    std::atomic<float> mix_      { 0.5f };

    // Delay line: max ~20 ms @ 48 kHz = 960 samples → 1024 is enough
    static constexpr std::size_t kMaxDelaySamples = 1024;
    static constexpr float kMinDelayMs = 0.5f;
    static constexpr float kMaxDelayMs = 10.0f;

    RingBuffer<kMaxDelaySamples> delayLine_;
    float lastFeedbackSample_ { 0.0f };
};

} // namespace dsp
