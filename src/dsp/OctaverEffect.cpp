#include "OctaverEffect.h"

#include <algorithm>

namespace dsp
{

static constexpr ParamDescriptor kParams[3] = {{"oct1", "-1 Octave", 0.0f, 1.0f, 0.7f},
                                               {"oct2", "-2 Octave", 0.0f, 1.0f, 0.3f},
                                               {"dry", "Dry Signal", 0.0f, 1.0f, 1.0f}};

void OctaverEffect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    shifter1_.prepare(sampleRate, maxBlockSize);
    shifter2_.prepare(sampleRate, maxBlockSize);

    shifter1_.setShiftSemitones(-12.0f);
    shifter2_.setShiftSemitones(-24.0f);
}

void OctaverEffect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const int safeNumSamples = std::min(numSamples, kMaxBlock);

    for (int i = 0; i < safeNumSamples; ++i)
    {
        buf1_[i] = buf[i];
        buf2_[i] = buf[i];
    }

    const float o1 = oct1_.load(std::memory_order_relaxed);
    const float o2 = oct2_.load(std::memory_order_relaxed);
    const float dr = dry_.load(std::memory_order_relaxed);

    if (o1 > 0.001f)
    {
        shifter1_.process(buf1_.data(), buf1_.data(), safeNumSamples, pitchHz);
    }
    else
    {
        std::fill(buf1_.begin(), buf1_.begin() + safeNumSamples, 0.0f);
    }

    if (o2 > 0.001f)
    {
        shifter2_.process(buf2_.data(), buf2_.data(), safeNumSamples, pitchHz);
    }
    else
    {
        std::fill(buf2_.begin(), buf2_.begin() + safeNumSamples, 0.0f);
    }

    for (int i = 0; i < safeNumSamples; ++i)
    {
        buf[i] = buf[i] * dr + buf1_[i] * o1 + buf2_[i] * o2;
    }
}

void OctaverEffect::reset() noexcept
{
    shifter1_.reset();
    shifter2_.reset();
}

ParamDescriptor OctaverEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float OctaverEffect::getParam(int i) const noexcept
{
    switch (i)
    {
    case kOct1:
        return oct1_.load(std::memory_order_relaxed);
    case kOct2:
        return oct2_.load(std::memory_order_relaxed);
    case kDry:
        return dry_.load(std::memory_order_relaxed);
    default:
        return 0.0f;
    }
}

void OctaverEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
    case kOct1:
        oct1_.store(v, std::memory_order_relaxed);
        break;
    case kOct2:
        oct2_.store(v, std::memory_order_relaxed);
        break;
    case kDry:
        dry_.store(v, std::memory_order_relaxed);
        break;
    }
}

} // namespace dsp
