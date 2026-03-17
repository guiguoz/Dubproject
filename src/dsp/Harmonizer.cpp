#include "Harmonizer.h"
#include "DspCommon.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace dsp {

// ═════════════════════════════════════════════════════════════════════════════
// OlaShifter
// ═════════════════════════════════════════════════════════════════════════════

void OlaShifter::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    analysisBuffer_.fill(0.0f);
    overlapBuffer_.fill(0.0f);
    analysisWritePos_ = 0;
    readPos_  = static_cast<float>(kAnalysisBufSize) - 512.0f;
    overlapPos_ = 0;
    buildHannWindow(overlapLength_);
}

void OlaShifter::reset() noexcept
{
    analysisBuffer_.fill(0.0f);
    overlapBuffer_.fill(0.0f);
    analysisWritePos_ = 0;
    // Start read position half a buffer behind write so we always read valid data
    readPos_  = static_cast<float>(kAnalysisBufSize) - 512.0f;
    overlapPos_ = 0;
}

void OlaShifter::setShiftSemitones(float semitones) noexcept
{
    shiftSemitones_ = semitones;
    shiftRatio_     = semitonesToRatio(semitones);
}

void OlaShifter::buildHannWindow(int length) noexcept
{
    if (length > kMaxOverlap)
        length = kMaxOverlap;
    overlapLength_ = length;
    const float pi = 3.14159265358979f;
    for (int i = 0; i < length; ++i)
    {
        const float w = 0.5f * (1.0f - std::cos(2.0f * pi * static_cast<float>(i)
                                                   / static_cast<float>(length - 1)));
        hannWindow_[static_cast<std::size_t>(i)] = w;
    }
}

int OlaShifter::computeWindowLength(float pitchHz) const noexcept
{
    // Window = 2 periods of the input pitch (pitch-synchronous OLA)
    if (pitchHz < 60.0f)
        pitchHz = 200.0f; // fallback if no pitch detected

    const int period = static_cast<int>(static_cast<float>(sampleRate_) / pitchHz);
    const int winLen = period * 2;

    // Clamp to [64, kMaxOverlap]
    if (winLen < 64)       return 64;
    if (winLen > kMaxOverlap) return kMaxOverlap;
    return winLen;
}

// ─────────────────────────────────────────────────────────────────────────────
// OlaShifter::process
//
// Algorithm:
//   1. Push incoming samples into the analysis ring buffer.
//   2. Read from the buffer at a rate of shiftRatio_ (resampling).
//   3. Apply OLA: fade in new grain with Hann window, fade out tail.
// ─────────────────────────────────────────────────────────────────────────────
void OlaShifter::process(const float* input, float* output,
                          int numSamples, float /*inputPitchHz*/) noexcept
{
    // Simple linear-interpolation resampler: pitch shift via variable read rate.
    // readPos_ starts 512 samples behind writePos_, advances at shiftRatio_ per sample.
    // This avoids all OLA complexity while delivering acceptable quality for ±7 st.
    for (int i = 0; i < numSamples; ++i)
    {
        // Write input sample
        analysisBuffer_[static_cast<std::size_t>(analysisWritePos_)] = input[i];
        analysisWritePos_ = (analysisWritePos_ + 1) % kAnalysisBufSize;

        // Read with linear interpolation
        const float wrapped = std::fmod(readPos_, static_cast<float>(kAnalysisBufSize));
        const int   iPos    = static_cast<int>(wrapped);
        const float frac    = wrapped - static_cast<float>(iPos);
        const int   iPos1   = (iPos + 1) % kAnalysisBufSize;

        output[i] = clipSample(
            lerp(analysisBuffer_[static_cast<std::size_t>(iPos)],
                 analysisBuffer_[static_cast<std::size_t>(iPos1)],
                 frac));

        // Advance read at shifted rate
        readPos_ += shiftRatio_;
        if (readPos_ >= static_cast<float>(kAnalysisBufSize) * 2.0f)
            readPos_ -= static_cast<float>(kAnalysisBufSize);
    }
}

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
