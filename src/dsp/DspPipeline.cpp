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

    // 3. Sampler: drain MIDI queue, then mix into buffer
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
        sampler_.process(buffer, numSamples);
    }

    // 4. Final clip (safety net)
    for (int i = 0; i < numSamples; ++i)
        buffer[i] = clipSample(buffer[i]);
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
