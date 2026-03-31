#include "DspPipeline.h"

#include "DspCommon.h"

namespace dsp
{

void DspPipeline::prepare(double sampleRate, int maxBlockSize) noexcept
{
    pitchTracker_.prepare(sampleRate, maxBlockSize);
    effectChain_.prepare(sampleRate, maxBlockSize);
    sampler_.prepare(sampleRate, maxBlockSize);
    bpmDetector_.prepare(sampleRate);

    tempBuffer_.resize(std::max(maxBlockSize, 256), 0.0f);
    tempBufL_.resize(std::max(maxBlockSize, 256), 0.0f);
    tempBufR_.resize(std::max(maxBlockSize, 256), 0.0f);
}

void DspPipeline::reset() noexcept
{
    pitchTracker_.reset();
    effectChain_.reset();
    sampler_.reset();
    bpmDetector_.reset();
    rmsRunning_ = 0.0f;
    smoothDuck_ = 1.0f;
    lastPitchHz_.store(0.0f, std::memory_order_relaxed);
    lastConfidence_.store(0.0f, std::memory_order_relaxed);
    rmsLevel_.store(0.0f, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// process — audio thread
// ─────────────────────────────────────────────────────────────────────────────
void DspPipeline::process(float* buffer, int numSamples) noexcept
{
    // 1. Pitch tracking (analysis only — does not modify buffer)
    const PitchResult pitch = pitchTracker_.process(buffer, numSamples);
    lastPitchHz_.store(pitch.frequencyHz, std::memory_order_relaxed);
    lastConfidence_.store(pitch.confidence, std::memory_order_relaxed);

    // 2. BPM detection (analysis only)
    bpmDetector_.process(buffer, numSamples);

    // 3. Smoothed RMS (exponential moving average over samples)
    for (int i = 0; i < numSamples; ++i)
        rmsRunning_ = kRmsAlpha * rmsRunning_ + (1.0f - kRmsAlpha) * std::abs(buffer[i]);
    rmsLevel_.store(rmsRunning_, std::memory_order_relaxed);

    // 4. Effect Chain (processes in-place)
    // If piano keyboard is active, use the forced pitch instead of YIN output.
    const float forced = forcedPitchHz_.load(std::memory_order_relaxed);
    const float effectPitch = (forced > 0.f) ? forced : pitch.frequencyHz;
    effectChain_.process(buffer, numSamples, effectPitch);

    // 5. Expression Mapper — apply RMS→param mapping to the target effect
    if (expressionMapper_.isActive())
    {
        const float mapped = expressionMapper_.mapValue(rmsRunning_);
        IEffect* fx = effectChain_.getActiveEffect(expressionMapper_.getEffectIndex());
        if (fx != nullptr)
            fx->setParam(expressionMapper_.getParamIndex(), mapped);
    }

    // 6. Sampler: drain MIDI queue, mix into buffer with optional Ducking (Anti-masking)
    if (samplerEnabled_.load(std::memory_order_acquire))
    {
        SamplerEvent evt;
        while (midiEventQueue_.tryPop(evt))
        {
            if (evt.noteOn)
                sampler_.trigger(evt.slotIndex);
            else
                sampler_.stop(evt.slotIndex);
        }

        if (duckingEnabled_.load(std::memory_order_relaxed))
        {
            // Target duck gain: map RMS [0.05 … 0.3] → gain [1.0 … 0.5]
            float targetDuck = 1.0f;
            if (rmsRunning_ > 0.05f)
            {
                const float ratio = std::min(1.0f, (rmsRunning_ - 0.05f) / 0.25f);
                targetDuck = 1.0f - ratio * 0.5f;
            }

            // Render sampler to temp buffer
            if (numSamples > static_cast<int>(tempBuffer_.size()))
                tempBuffer_.resize(numSamples, 0.0f);
            else
                std::fill(tempBuffer_.begin(), tempBuffer_.begin() + numSamples, 0.0f);

            sampler_.process(tempBuffer_.data(), numSamples);

            // EMA ramp per-sample (~5 ms at 44100 Hz) — eliminates ducking clicks
            static constexpr float kDuckCoeff = 0.002f;
            for (int i = 0; i < numSamples; ++i)
            {
                smoothDuck_ = std::clamp(smoothDuck_ + kDuckCoeff * (targetDuck - smoothDuck_),
                                         0.0f, 2.0f);
                buffer[i] += tempBuffer_[i] * smoothDuck_;
            }

            currentDuckingGain_.store(smoothDuck_, std::memory_order_relaxed);
        }
        else
        {
            // No ducking: sampler mixes directly into the buffer
            sampler_.process(buffer, numSamples);
            currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
        }
    }
    else
    {
        currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
    }

    // 7. Final Master Limiter (Soft-Clipper safety net)
    masterLimiter_.process(buffer, numSamples);
}

// ─────────────────────────────────────────────────────────────────────────────
// processStereo
//
// Stereo variant of process(). Left channel carries the live sax signal.
// The sax is placed at a slight right bias (+0.2 pan) after the effect chain,
// so backing tracks can be spatially separated from it.
// Sampler is rendered with per-slot pan/Haas via processStereo().
// ─────────────────────────────────────────────────────────────────────────────
void DspPipeline::processStereo(float* left, float* right, int numSamples) noexcept
{
    // 1. Pitch tracking (analysis on left — does not modify buffer)
    const PitchResult pitch = pitchTracker_.process(left, numSamples);
    lastPitchHz_.store(pitch.frequencyHz, std::memory_order_relaxed);
    lastConfidence_.store(pitch.confidence, std::memory_order_relaxed);

    // 2. BPM detection (analysis on left)
    bpmDetector_.process(left, numSamples);

    // 3. Smoothed RMS (left channel only)
    for (int i = 0; i < numSamples; ++i)
        rmsRunning_ = kRmsAlpha * rmsRunning_ + (1.0f - kRmsAlpha) * std::abs(left[i]);
    rmsLevel_.store(rmsRunning_, std::memory_order_relaxed);

    // 4. Effect chain on left (in-place)
    const float forced     = forcedPitchHz_.load(std::memory_order_relaxed);
    const float effectPitch = (forced > 0.f) ? forced : pitch.frequencyHz;
    effectChain_.process(left, numSamples, effectPitch);

    // 5. Expression mapper
    if (expressionMapper_.isActive())
    {
        const float mapped = expressionMapper_.mapValue(rmsRunning_);
        IEffect* fx = effectChain_.getActiveEffect(expressionMapper_.getEffectIndex());
        if (fx != nullptr)
            fx->setParam(expressionMapper_.getParamIndex(), mapped);
    }

    // 6. Spread sax L/R: sax sits at pan = +0.2 (slightly right)
    //    Equal-power: angle = (0.2+1)*0.25*π → cos≈0.951, sin≈0.309
    //    That gives left ×0.951, right ×0.309 — audibly left-biased with slight R presence.
    //    We swap to make sax "mostly left" with a hint in right = more natural for live sax.
    //    Implementation: left is the main sax signal; right gets an attenuated copy.
    static constexpr float kSaxPanL = 0.9511f;  // cos(0.2+1)*π/4)
    static constexpr float kSaxPanR = 0.3090f;  // sin(...)
    for (int i = 0; i < numSamples; ++i)
    {
        right[i] = left[i] * kSaxPanR;
        left [i] *= kSaxPanL;
    }

    // 7. Sampler (MIDI drain + stereo mix with optional ducking)
    if (samplerEnabled_.load(std::memory_order_acquire))
    {
        SamplerEvent evt;
        while (midiEventQueue_.tryPop(evt))
        {
            if (evt.noteOn) sampler_.trigger(evt.slotIndex);
            else            sampler_.stop(evt.slotIndex);
        }

        if (static_cast<int>(tempBufL_.size()) < numSamples)
        {
            tempBufL_.resize(numSamples, 0.f);
            tempBufR_.resize(numSamples, 0.f);
        }
        std::fill(tempBufL_.begin(), tempBufL_.begin() + numSamples, 0.f);
        std::fill(tempBufR_.begin(), tempBufR_.begin() + numSamples, 0.f);

        sampler_.processStereo(tempBufL_.data(), tempBufR_.data(), numSamples);

        if (duckingEnabled_.load(std::memory_order_relaxed))
        {
            float targetDuck = 1.0f;
            if (rmsRunning_ > 0.05f)
            {
                const float ratio = std::min(1.0f, (rmsRunning_ - 0.05f) / 0.25f);
                targetDuck = 1.0f - ratio * 0.5f;
            }
            static constexpr float kDuckCoeff = 0.002f;
            for (int i = 0; i < numSamples; ++i)
            {
                smoothDuck_ = std::clamp(smoothDuck_ + kDuckCoeff * (targetDuck - smoothDuck_),
                                         0.0f, 2.0f);
                left [i] += tempBufL_[i] * smoothDuck_;
                right[i] += tempBufR_[i] * smoothDuck_;
            }
            currentDuckingGain_.store(smoothDuck_, std::memory_order_relaxed);
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left [i] += tempBufL_[i];
                right[i] += tempBufR_[i];
            }
            currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
        }
    }
    else
    {
        currentDuckingGain_.store(1.0f, std::memory_order_relaxed);
    }

    // 8. Master limiter on both channels independently
    masterLimiter_.process(left,  numSamples);
    masterLimiter_.process(right, numSamples);
}

// ─────────────────────────────────────────────────────────────────────────────
// Enable / disable — GUI thread
// ─────────────────────────────────────────────────────────────────────────────
void DspPipeline::setSamplerEnabled(bool enabled) noexcept
{
    samplerEnabled_.store(enabled, std::memory_order_release);
}

bool DspPipeline::isSamplerEnabled() const noexcept
{
    return samplerEnabled_.load(std::memory_order_acquire);
}

PitchResult DspPipeline::getLastPitch() const noexcept
{
    return {lastPitchHz_.load(std::memory_order_relaxed),
            lastConfidence_.load(std::memory_order_relaxed)};
}

} // namespace dsp
