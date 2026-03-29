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
}

void DspPipeline::reset() noexcept
{
    pitchTracker_.reset();
    effectChain_.reset();
    sampler_.reset();
    bpmDetector_.reset();
    rmsRunning_ = 0.0f;
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
            // Ducking: if sax RMS > 0.05, reduce sampler volume (up to -6 dB at RMS=0.3)
            float duckGain = 1.0f;
            if (rmsRunning_ > 0.05f)
            {
                // Map [0.05 ... 0.3] -> [1.0 ... 0.5]
                float ratio = (rmsRunning_ - 0.05f) / 0.25f;
                ratio = std::min(1.0f, ratio);
                duckGain = 1.0f - (ratio * 0.5f); // Drops to 0.5 at max
            }

            // Smooth the duck gain slightly if needed, but per-block is fine
            // Render sampler to a temp buffer, then mix with ducking
            if (numSamples > static_cast<int>(tempBuffer_.size()))
                tempBuffer_.resize(numSamples, 0.0f);
            else
                std::fill(tempBuffer_.begin(), tempBuffer_.begin() + numSamples, 0.0f);

            sampler_.process(tempBuffer_.data(), numSamples);

            for (int i = 0; i < numSamples; ++i)
                buffer[i] += tempBuffer_[i] * duckGain;

            currentDuckingGain_.store(duckGain, std::memory_order_relaxed);
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
