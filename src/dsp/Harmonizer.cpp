#include "Harmonizer.h"

#include <algorithm>

namespace dsp {

// ═════════════════════════════════════════════════════════════════════════════
// Harmonizer
// ═════════════════════════════════════════════════════════════════════════════

Harmonizer::Harmonizer() noexcept
{
    for (auto& flag : voiceEnabled_)
        flag.store(true, std::memory_order_relaxed);
}

void Harmonizer::prepare(double sampleRate, int maxBlockSize) noexcept
{
    voiceBuffer_.fill(0.0f);
    for (int v = 0; v < kNumVoices; ++v)
    {
        voices_[static_cast<std::size_t>(v)].prepare(sampleRate, maxBlockSize);
        voices_[static_cast<std::size_t>(v)].setShiftSemitones(voiceIntervals_[static_cast<std::size_t>(v)]);
    }
}

void Harmonizer::reset() noexcept
{
    for (auto& v : voices_)
        v.reset();
    voiceBuffer_.fill(0.0f);
}

void Harmonizer::setVoiceInterval(int voiceIndex, float semitones) noexcept
{
    if (voiceIndex < 0 || voiceIndex >= kNumVoices) return;
    voiceIntervals_[static_cast<std::size_t>(voiceIndex)] = semitones;
    voices_[static_cast<std::size_t>(voiceIndex)].setShiftSemitones(semitones);
}

void Harmonizer::setVoiceEnabled(int voiceIndex, bool enabled) noexcept
{
    if (voiceIndex < 0 || voiceIndex >= kNumVoices) return;
    voiceEnabled_[static_cast<std::size_t>(voiceIndex)].store(enabled, std::memory_order_release);
}

void Harmonizer::setMix(float mix) noexcept
{
    mix_.store(mix, std::memory_order_relaxed);
}

float Harmonizer::getVoiceInterval(int voiceIndex) const noexcept
{
    if (voiceIndex < 0 || voiceIndex >= kNumVoices) return 0.0f;
    return voiceIntervals_[static_cast<std::size_t>(voiceIndex)];
}

// ─────────────────────────────────────────────────────────────────────────────
// Harmonizer::process
// ─────────────────────────────────────────────────────────────────────────────
void Harmonizer::process(const float* input, float* output,
                          int numSamples, float inputPitchHz) noexcept
{
    if (numSamples > kMaxBlockSize) numSamples = kMaxBlockSize; // guard

    const float mix     = mix_.load(std::memory_order_relaxed);
    const float dryGain = 1.0f - mix;
    // Each voice contributes mix/kNumVoices so total wet = mix
    const float voiceGain = mix / static_cast<float>(kNumVoices);

    // Start with dry signal
    for (int i = 0; i < numSamples; ++i)
        output[i] = dryGain * input[i];

    // Add each enabled voice
    for (int v = 0; v < kNumVoices; ++v)
    {
        if (!voiceEnabled_[static_cast<std::size_t>(v)].load(std::memory_order_acquire))
            continue;

        voices_[static_cast<std::size_t>(v)].process(
            input, voiceBuffer_.data(), numSamples, inputPitchHz);

        for (int i = 0; i < numSamples; ++i)
            output[i] += voiceGain * voiceBuffer_[static_cast<std::size_t>(i)];
    }

    // Clip output
    for (int i = 0; i < numSamples; ++i)
        output[i] = clipSample(output[i]);
}

} // namespace dsp
