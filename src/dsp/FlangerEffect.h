#pragma once

#include "IEffect.h"
#include "Flanger.h"

namespace dsp {

// ─────────────────────────────────────────────────────────────────────────────
// FlangerEffect — IEffect wrapper around the existing Flanger DSP module.
//
// Parameters (indices 0-3):
//   0  rate      LFO rate in Hz       [0.05 .. 10.0]  default 0.5
//   1  depth     Modulation depth     [0.0  ..  1.0]  default 0.7
//   2  feedback  Feedback coefficient [-0.95 .. 0.95]  default 0.3
//   3  mix       Dry/wet mix          [0.0  ..  1.0]  default 0.5
// ─────────────────────────────────────────────────────────────────────────────
class FlangerEffect : public IEffect
{
public:
    EffectType type() const noexcept override { return EffectType::Flanger; }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset()  noexcept override;

    int             paramCount()           const noexcept override { return kParamCount; }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float           getParam(int i)        const noexcept override;
    void            setParam(int i, float v) noexcept override;

private:
    enum : int { kRate = 0, kDepth, kFeedback, kMix, kParamCount };
    Flanger flanger_;
};

} // namespace dsp
