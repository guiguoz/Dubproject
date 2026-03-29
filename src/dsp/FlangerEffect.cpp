#include "FlangerEffect.h"

namespace dsp {

static constexpr ParamDescriptor kFlangerParams[4] = {
    { "rate",     "Rate",      0.05f, 10.0f, 0.5f  },
    { "depth",    "Depth",     0.0f,  1.0f,  0.7f  },
    { "feedback", "Feedback", -0.95f, 0.95f, 0.3f  },
    { "mix",      "Mix",       0.0f,  1.0f,  0.5f  },
};

void FlangerEffect::prepare(double sampleRate, int maxBlockSize) noexcept
{
    flanger_.prepare(sampleRate, maxBlockSize);
}

void FlangerEffect::process(float* buf, int numSamples, float /*pitchHz*/) noexcept
{
    if (enabled.load(std::memory_order_acquire))
        flanger_.process(buf, numSamples);
}

void FlangerEffect::reset() noexcept
{
    flanger_.reset();
}

ParamDescriptor FlangerEffect::paramDescriptor(int i) const noexcept
{
    return (i >= 0 && i < kParamCount) ? kFlangerParams[i] : ParamDescriptor{};
}

float FlangerEffect::getParam(int i) const noexcept
{
    switch (i)
    {
        case kRate:     return flanger_.getRate();
        case kDepth:    return flanger_.getDepth();
        case kFeedback: return flanger_.getFeedback();
        case kMix:      return flanger_.getMix();
        default:        return 0.0f;
    }
}

void FlangerEffect::setParam(int i, float v) noexcept
{
    switch (i)
    {
        case kRate:     flanger_.setRate(v);     break;
        case kDepth:    flanger_.setDepth(v);    break;
        case kFeedback: flanger_.setFeedback(v); break;
        case kMix:      flanger_.setMix(v);      break;
        default:        break;
    }
}

} // namespace dsp
