#include "AutoPitchCorrect.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

static constexpr ParamDescriptor kParams[2] = {
    { "strength", "Strength", 0.0f, 1.0f,   1.0f  },
    { "refHz",    "Ref A4",   415.f, 466.f, 440.0f }
};

void AutoPitchCorrect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    shifter_.prepare(sampleRate, maxBlockSize);
}

void AutoPitchCorrect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    // Need a valid pitch to compute the correction
    if (pitchHz < 20.0f)
        return;

    const float str = strength_.load(std::memory_order_relaxed);
    const float ref = refHz_.load(std::memory_order_relaxed);

    // How many semitones away from nearest chromatic note?
    const float semitones        = 12.0f * std::log2(pitchHz / ref);
    const float nearestSemitones = std::round(semitones);
    const float correction       = nearestSemitones - semitones; // semitones to apply

    if (std::abs(correction) < 0.005f)
        return; // already in tune

    const int safeN = std::min(numSamples, kMaxBlock);
    std::copy(buf, buf + safeN, shifted_.data());

    shifter_.setShiftSemitones(correction);
    shifter_.process(shifted_.data(), shifted_.data(), safeN, pitchHz);

    const float dry = 1.0f - str;
    const float wet = str;
    for (int i = 0; i < safeN; ++i)
        buf[i] = buf[i] * dry + shifted_[i] * wet;
}

void AutoPitchCorrect::reset() noexcept
{
    shifter_.reset();
}

ParamDescriptor AutoPitchCorrect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float AutoPitchCorrect::getParam(int i) const noexcept
{
    if (i == kStrength) return strength_.load(std::memory_order_relaxed);
    if (i == kRefHz)    return refHz_.load(std::memory_order_relaxed);
    return 0.0f;
}

void AutoPitchCorrect::setParam(int i, float v) noexcept
{
    if (i == kStrength) strength_.store(v, std::memory_order_relaxed);
    if (i == kRefHz)    refHz_.store(v,    std::memory_order_relaxed);
}

} // namespace dsp
