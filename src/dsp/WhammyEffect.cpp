#include "WhammyEffect.h"

#include <algorithm>

namespace dsp
{

static constexpr ParamDescriptor kParams[4] = {{"expression", "Pedal", 0.0f, 1.0f, 0.0f},
                                               {"toePitch", "Toe Shift", -24.0f, 24.0f, 12.0f},
                                               {"heelPitch", "Heel Shift", -24.0f, 24.0f, 0.0f},
                                               {"mix", "Mix", 0.0f, 1.0f, 1.0f}};

void WhammyEffect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    shifter_.prepare(sampleRate, maxBlockSize);
}

void WhammyEffect::process(float* buf, int numSamples, float pitchHz) noexcept
{
    if (!enabled.load(std::memory_order_acquire))
        return;

    const int safeNumSamples = std::min(numSamples, kMaxBlock);

    const float expr = expression_.load(std::memory_order_relaxed);
    const float toe  = toePitch_.load(std::memory_order_relaxed);
    const float heel = heelPitch_.load(std::memory_order_relaxed);
    const float mx   = mix_.load(std::memory_order_relaxed);

    // Calculate current shift
    float currentShift = heel + expr * (toe - heel);

    // If shift is zero, simple bypass with mix
    if (std::abs(currentShift) < 0.01f)
    {
        // No shift needed
        return;
    }

    shifter_.setShiftSemitones(currentShift);

    // Copy to scratch
    for (int i = 0; i < safeNumSamples; ++i)
        shiftedBuf_[i] = buf[i];

    // Shift
    shifter_.process(shiftedBuf_.data(), shiftedBuf_.data(), safeNumSamples, pitchHz);

    // Mix
    const float dryMix = 1.0f - mx;
    const float wetMix = mx;

    for (int i = 0; i < safeNumSamples; ++i)
    {
        buf[i] = buf[i] * dryMix + shiftedBuf_[i] * wetMix;
    }
}

void WhammyEffect::reset() noexcept
{
    shifter_.reset();
}

ParamDescriptor WhammyEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kParams[i] : ParamDescriptor{};
}

float WhammyEffect::getParam(int i) const noexcept
{
    switch (i)
    {
    case kExpression:
        return expression_.load(std::memory_order_relaxed);
    case kToePitch:
        return toePitch_.load(std::memory_order_relaxed);
    case kHeelPitch:
        return heelPitch_.load(std::memory_order_relaxed);
    case kMix:
        return mix_.load(std::memory_order_relaxed);
    default:
        return 0.0f;
    }
}

void WhammyEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
    case kExpression:
        expression_.store(v, std::memory_order_relaxed);
        break;
    case kToePitch:
        toePitch_.store(v, std::memory_order_relaxed);
        break;
    case kHeelPitch:
        heelPitch_.store(v, std::memory_order_relaxed);
        break;
    case kMix:
        mix_.store(v, std::memory_order_relaxed);
        break;
    }
}

} // namespace dsp
