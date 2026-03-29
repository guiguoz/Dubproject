#pragma once

#include "IEffect.h"

namespace dsp
{

// ─────────────────────────────────────────────────────────────────────────────
// SlicerEffect
//
// Square wave LFO amplitude gating.
//
// Parameters:
//   0  rate    LFO rate in Hz        [0.1 .. 20.0]  default 4.0
//   1  depth   Gating depth          [0.0 .. 1.0]   default 1.0
// ─────────────────────────────────────────────────────────────────────────────
class SlicerEffect : public IEffect
{
  public:
    EffectType type() const noexcept override
    {
        return EffectType::Slicer;
    }

    void prepare(double sampleRate, int maxBlockSize) noexcept override;
    void process(float* buf, int numSamples, float pitchHz) noexcept override;
    void reset() noexcept override;

    int paramCount() const noexcept override
    {
        return kParamCount;
    }
    ParamDescriptor paramDescriptor(int i) const noexcept override;
    float getParam(int i) const noexcept override;
    void setParam(int i, float v) noexcept override;

    // Presets (built-in, same pattern as SynthEffect)
    int         presetCount()                const noexcept override;
    const char* presetName(int index)        const noexcept override;
    void        applyPreset(int index)             noexcept override;

  private:
    enum : int
    {
        kRate = 0,
        kDepth,
        kParamCount
    };

    double sampleRate_{44100.0};
    float phase_{0.0f};

    std::atomic<float> rate_{4.0f};
    std::atomic<float> depth_{1.0f};
};

} // namespace dsp
