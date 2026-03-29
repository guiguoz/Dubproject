#include "PitchForkEffect.h"
#include "DspCommon.h"

#include <algorithm>

namespace dsp {

static constexpr ParamDescriptor kPitchForkParams[2] = {
    { "semitones", "Semitones", -12.0f, 12.0f, 0.0f },
    { "mix",       "Mix",        0.0f,  1.0f,  0.5f },
};

void PitchForkEffect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    shifter_.prepare(sampleRate, maxBlockSize);
    shiftedBuf_.fill(0.0f);
}

void PitchForkEffect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const int   n   = std::min(numSamples, kMaxBlock);
    const float mx  = mix_.load(std::memory_order_relaxed);
    const float dry = 1.0f - mx;
    const float wet = mx;

    shifter_.process(buf, shiftedBuf_.data(), n, pitchHz);

    for (int i = 0; i < n; ++i)
        buf[i] = clipSample(dry * buf[i] + wet * shiftedBuf_[i]);
}

void PitchForkEffect::reset() noexcept
{
    shifter_.reset();
    shiftedBuf_.fill(0.0f);
}

ParamDescriptor PitchForkEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kPitchForkParams[i] : ParamDescriptor{};
}

float PitchForkEffect::getParam(int i) const noexcept
{
    switch (i)
    {
        case kSemitones: return semitones_.load(std::memory_order_relaxed);
        case kMix:       return mix_.load(std::memory_order_relaxed);
        default:         return 0.0f;
    }
}

void PitchForkEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
        case kSemitones:
            semitones_.store(v, std::memory_order_relaxed);
            shifter_.setShiftSemitones(v);
            break;
        case kMix:
            mix_.store(v, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

} // namespace dsp
