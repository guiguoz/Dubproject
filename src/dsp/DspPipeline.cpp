#include "DspPipeline.h"
#include "DspCommon.h"

#include <cstring>
#include <algorithm>

namespace dsp {

void DspPipeline::prepare(double sampleRate, int maxBlockSize) noexcept
{
    pitchTracker_.prepare(sampleRate, maxBlockSize);
    harmonizer_.prepare(sampleRate, maxBlockSize);
    flanger_.prepare(sampleRate, maxBlockSize);
    scratchBuffer_.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
}

void DspPipeline::reset() noexcept
{
    pitchTracker_.reset();
    harmonizer_.reset();
    flanger_.reset();
    lastPitchHz_.store(0.0f, std::memory_order_relaxed);
    lastConfidence_.store(0.0f, std::memory_order_relaxed);
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

    // 2. Harmonizer (reads buffer, writes to scratch, then copies back)
    if (harmonizerEnabled_.load(std::memory_order_acquire))
    {
        harmonizer_.process(buffer, scratchBuffer_.data(), numSamples, pitch.frequencyHz);
        std::memcpy(buffer, scratchBuffer_.data(),
                    static_cast<std::size_t>(numSamples) * sizeof(float));
    }

    // 3. Flanger (in-place)
    if (flangerEnabled_.load(std::memory_order_acquire))
        flanger_.process(buffer, numSamples);

    // 4. Final clip (safety net)
    for (int i = 0; i < numSamples; ++i)
        buffer[i] = clipSample(buffer[i]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Enable / disable — GUI thread
// ─────────────────────────────────────────────────────────────────────────────
void DspPipeline::setHarmonizerEnabled(bool enabled) noexcept
{
    harmonizerEnabled_.store(enabled, std::memory_order_release);
}

void DspPipeline::setFlangerEnabled(bool enabled) noexcept
{
    flangerEnabled_.store(enabled, std::memory_order_release);
}

bool DspPipeline::isHarmonizerEnabled() const noexcept
{
    return harmonizerEnabled_.load(std::memory_order_acquire);
}

bool DspPipeline::isFlangerEnabled() const noexcept
{
    return flangerEnabled_.load(std::memory_order_acquire);
}

PitchResult DspPipeline::getLastPitch() const noexcept
{
    return { lastPitchHz_.load(std::memory_order_relaxed),
             lastConfidence_.load(std::memory_order_relaxed) };
}

} // namespace dsp
